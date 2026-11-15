/* Bridge ordinary UMI carrier SMS to the iOS 16.5 Messages service. */
#import <CommonCrypto/CommonDigest.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/sysctl.h>
#include <sys/umi_sdx55_sms.h>
#include <unistd.h>

#include "umi_sms_submit.h"

/* Selectors verified against the 20F66 dyld cache. Resolve the classes only
 * in imagent, after its own SMS service has loaded. */
@interface NSObject (UMISMSPrivate)
+ (id)sharedAccountController;
- (id)anySessionForServiceName:(NSString *)name;
- (void)_processReceivedSMSMessage:(id)message storageContext:(id)context
               withCompletionBlock:(void (^)(BOOL, NSError *))completion;
- (void)_processSMSOrSMSMessageSentWithContext:(id)context success:(BOOL)success
    messageID:(uint32_t)messageID err1:(NSInteger)err1 err2:(NSInteger)err2 isSMS:(BOOL)isSMS;
+ (id)contextWithSlot:(NSInteger)slot;
- (void)setIsSimPresent:(BOOL)present;
- (void)setIsSimGood:(BOOL)good;
- (void)setUserDefaultVoice:(NSNumber *)preferred;
- (void)setUserDataPreferred:(NSNumber *)preferred;
- (void)setSubscriptions:(NSArray *)subscriptions;
- (NSArray *)subscriptions;
- (id)initWithSubscriptionInfo:(id)info;
- (uint32_t)messageId;
- (NSArray *)recipients;
- (NSArray *)items;
- (NSString *)digits;
- (NSString *)canonicalFormat;
- (NSString *)contentType;
- (NSData *)data;
@end

/* The 20F66 SMS session consumes these properties after parsing. Create
 * this object only from a decoded SMS-DELIVER with plain Unicode text.
 * Its one text part has no attachments, HTML, SMIL, or relay metadata. */
@interface UMIPlainSMSPart : NSObject
@property(nonatomic, copy) NSString *plainTextContentBody;
@end

@implementation UMIPlainSMSPart
- (NSString *)contentType { return @"text/plain"; }
- (NSString *)contentId { return nil; }
- (NSString *)contentLocation { return nil; }
- (UMIPlainSMSPart *)content { return self; }
- (NSUInteger)type { return 0; }
- (UMIPlainSMSPart *)plain { return self; }
@end

@interface UMIPlainSMS : NSObject
@property(nonatomic, copy) NSString *sender;
@property(nonatomic, copy) NSString *countryCode;
@property(nonatomic, copy) NSString *GUID;
@property(nonatomic, copy) NSDate *date;
@property(nonatomic, copy) NSArray<UMIPlainSMSPart *> *items;
@end

@implementation UMIPlainSMS
- (NSInteger)type { return 1; }
- (NSArray *)recipients { return @[]; }
- (NSNumber *)version { return @1; }
- (NSString *)serviceCenter { return nil; }
- (NSString *)subject { return nil; }
- (NSString *)originatedDeviceEmail { return nil; }
- (NSString *)originatedDeviceNumber { return nil; }
- (NSString *)originatedDeviceSIM { return nil; }
- (NSInteger)replaceMessage { return 0; }
- (BOOL)trackMessage { return NO; }
- (BOOL)has_sendEnabled { return NO; }
- (BOOL)sendEnabled { return NO; }
- (BOOL)has_iMessageCapability { return NO; }
- (NSInteger)iMessageCapability { return 0; }
- (BOOL)has_spamCategory { return NO; }
- (NSInteger)spamCategory { return 0; }
- (BOOL)has_trackMessage { return NO; }
- (BOOL)has_replaceMessage { return NO; }
@end

static void sms_log(const char *message, int value)
{
    int fd = open("/dev/console", O_WRONLY);
    if (fd < 0) return;
    char line[112];
    int length = snprintf(line, sizeof(line), "UMI SMS: %s %d\n", message, value);
    if (length > 0 && (size_t)length < sizeof(line)) (void)write(fd, line, (size_t)length);
    close(fd);
}

typedef struct { int domain, error; } UMISMSError;
static UMISMSError (*original_send_sms)(id, SEL, id, BOOL, uint32_t *);
static id (*original_subscription_info)(id, SEL, NSError **);
static BOOL (*original_local_sms)(id, SEL);

static BOOL sms_read_snapshot(struct umi_sms_snapshot *snapshot)
{
    size_t size = sizeof(*snapshot);
    return !sysctlbyname(UMI_SMS_SYSCTL, snapshot, &size, NULL, 0) &&
        size == sizeof(*snapshot) && snapshot->version == UMI_SMS_VERSION;
}

static BOOL sms_local_service(id session, SEL selector)
{
    struct umi_sms_snapshot snapshot;
    if (sms_read_snapshot(&snapshot) && snapshot.ready) return YES;
    return original_local_sms(session, selector);
}

/* The native SMS service selects the sole active subscription before calling
 * CTMessageCenter. Supply a context derived from the WMS client binding when
 * CommCenter cannot provide a subscription. This hook runs only in imagent. */
static id sms_subscription_info(id utility, SEL selector, NSError **error)
{
    id info = original_subscription_info(utility, selector, error);
    if ([[info subscriptions] count]) return info;
    struct umi_sms_snapshot snapshot;
    if (!sms_read_snapshot(&snapshot) || !snapshot.ready ||
        snapshot.subscription < 1 || snapshot.subscription > 2) return info;

    id context = [NSClassFromString(@"CTXPCServiceSubscriptionContext") contextWithSlot:snapshot.subscription];
    id telephonyInfo = [[NSClassFromString(@"CTXPCServiceSubscriptionInfo") alloc] init];
    if (!context || !telephonyInfo) return info;
    [context setIsSimPresent:YES];
    [context setIsSimGood:YES];
    [context setUserDefaultVoice:@YES];
    [context setUserDataPreferred:@YES];
    [telephonyInfo setSubscriptions:@[context]];
    id fallback = [[NSClassFromString(@"IMCTXPCServiceSubscriptionInfo") alloc]
        initWithSubscriptionInfo:telephonyInfo];
    if (!fallback) return info;
    if (error) *error = nil;
    static uint32_t logged;
    if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED))
        sms_log("Messages using UMI subscription=", snapshot.subscription);
    return fallback;
}

@interface UMISMSOutgoing : NSObject {
@public
    struct umi_sms_submit_plan plan;
}
@property(nonatomic) uint32_t messageID;
@property(nonatomic) uint8_t reference;
@property(nonatomic) unsigned part;
@property(nonatomic) uint64_t token;
@property(nonatomic) NSData *text;
@property(nonatomic, copy) NSString *number;
@property(nonatomic) id session;
@end

@implementation UMISMSOutgoing
@end

/* Keep the queued messages and the kernel's single in-flight submission on
 * the imagent main queue. CTMessageCenter may be called off that queue. */
static NSMutableArray<UMISMSOutgoing *> *outgoing;

static void sms_send_result(id session, uint32_t messageID, int error)
{
    sms_log("Messages send completion errno=", error);
    [session _processSMSOrSMSMessageSentWithContext:nil success:error == 0
        messageID:messageID err1:error ? 1 : 0 err2:error isSMS:YES];
}

static void sms_send_poll(void)
{
    UMISMSOutgoing *item = outgoing.firstObject;
    if (!item) return;
    int error = 0;
    if (item.token) {
        struct umi_sms_send_status snapshot;
        size_t size = sizeof(snapshot);
        if (sysctlbyname(UMI_SMS_SEND_SYSCTL, &snapshot, &size, NULL, 0)) error = errno;
        else if (size != sizeof(snapshot) || snapshot.version != UMI_SMS_VERSION ||
            snapshot.token != item.token) error = EIO;
        else if (snapshot.pending) return;
        else error = snapshot.error;
        item.token = 0;
        if (!error) item.part++;
    }
    if (!error && item.part < item->plan.count) {
        struct umi_sms_submission submit;
        if (!umi_sms_submit(item.number.UTF8String, item.text.bytes, &item->plan,
            item.part, item.reference, &submit)) error = EINVAL;
        else {
            static uint64_t nextToken;
            if (!nextToken) arc4random_buf(&nextToken, sizeof(nextToken));
            submit.token = ++nextToken;
            if (sysctlbyname(UMI_SMS_SEND_SYSCTL, NULL, NULL, &submit, sizeof(submit))) error = errno;
            else item.token = submit.token;
            memset_s(&submit, sizeof(submit), 0, sizeof(submit));
        }
        if (!error) return;
    }
    [outgoing removeObjectAtIndex:0];
    sms_send_result(item.session, item.messageID, error);
}

static UMISMSError sms_send(id center, SEL selector, id message, BOOL more, uint32_t *tracking)
{
    uint32_t messageID = [message messageId];
    if (messageID == UINT32_MAX)
        return original_send_sms(center, selector, message, more, tracking);

    NSArray *recipients = [message recipients], *parts = [message items];
    id recipient = recipients.firstObject, part = parts.firstObject;
    NSString *contentType = [part respondsToSelector:@selector(contentType)] ? [part contentType] : nil;
    if (recipients.count != 1 || parts.count != 1 ||
        ![recipient respondsToSelector:@selector(digits)] ||
        !contentType || [contentType caseInsensitiveCompare:@"text/plain"] != NSOrderedSame) {
        sms_log("Messages send outside plain SMS path, parts=", (int)parts.count);
        return original_send_sms(center, selector, message, more, tracking);
    }

    NSString *number = [recipient digits];
    NSString *canonical = [recipient respondsToSelector:@selector(canonicalFormat)] ? [recipient canonicalFormat] : nil;
    if ([canonical hasPrefix:@"+"]) number = canonical;
    number = [[number componentsSeparatedByCharactersInSet:
        [NSCharacterSet characterSetWithCharactersInString:@" -().\u00a0"]] componentsJoinedByString:@""];
    NSData *data = [part respondsToSelector:@selector(data)] ? [part data] : nil;
    NSString *body = data.length ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : nil;
    if (!number.length || !body.length || body.length > 255 * 153) {
        sms_log("invalid SMS recipient or text errno=", EINVAL);
        return (UMISMSError){1, EINVAL};
    }
    UMISMSOutgoing *item = [[UMISMSOutgoing alloc] init];
    item.number = number;
    item.messageID = messageID;
    item.reference = arc4random_uniform(256);
    NSMutableData *units = [NSMutableData dataWithLength:body.length * sizeof(uint16_t)];
    [body getCharacters:units.mutableBytes range:NSMakeRange(0, body.length)];
    if (!umi_sms_plan(units.bytes, body.length, &item->plan)) return (UMISMSError){1, EINVAL};
    item.text = units;
    if (tracking) *tracking = messageID;
    dispatch_async(dispatch_get_main_queue(), ^{
        id account = [NSClassFromString(@"IMDAccountController") sharedAccountController];
        item.session = [account anySessionForServiceName:@"SMS"];
        if (!outgoing) outgoing = [[NSMutableArray alloc] init];
        if (outgoing.count >= 16) sms_send_result(item.session, messageID, EBUSY);
        else {
            [outgoing addObject:item];
            sms_log("Messages send queued, segments=", item->plan.count);
            if (outgoing.count == 1) sms_send_poll();
        }
    });
    return (UMISMSError){0, 0};
}

static void sms_install_sender(id session)
{
    if (original_send_sms ||
        ![session respondsToSelector:@selector(_processSMSOrSMSMessageSentWithContext:success:messageID:err1:err2:isSMS:)])
        return;
    Method send = class_getInstanceMethod(NSClassFromString(@"CTMessageCenter"),
        sel_registerName("sendSMS:withMoreToFollow:trackingID:"));
    if (!send) return;
    original_send_sms = (typeof(original_send_sms))method_getImplementation(send);
    method_setImplementation(send, (IMP)sms_send);
    sms_log("Messages sender installed", 0);

    Method subscription = class_getInstanceMethod(NSClassFromString(@"IMCTSubscriptionUtilities"),
        sel_registerName("ctSubscriptionInfoWithError:"));
    if (subscription) {
        original_subscription_info = (typeof(original_subscription_info))method_getImplementation(subscription);
        method_setImplementation(subscription, (IMP)sms_subscription_info);
    }
    Method local = class_getInstanceMethod(NSClassFromString(@"SMSServiceSession"),
        sel_registerName("_localDeviceSupportsSMS"));
    if (local) {
        original_local_sms = (typeof(original_local_sms))method_getImplementation(local);
        method_setImplementation(local, (IMP)sms_local_service);
    }
}

static int sms_receipt(uint64_t sequence, BOOL accepted)
{
    struct umi_sms_receipt receipt = {
        .version = UMI_SMS_VERSION, .sequence = sequence, .accepted = accepted
    };
    return sysctlbyname(UMI_SMS_SYSCTL, NULL, NULL, &receipt, sizeof(receipt)) ? errno : 0;
}

static unsigned sms_decimal(uint8_t byte)
{
    unsigned lo = byte & 15, hi = byte >> 4;
    return lo < 10 && hi < 10 ? lo * 10 + hi : UINT_MAX;
}

static NSDate *sms_date(const uint8_t *timestamp)
{
    unsigned year = sms_decimal(timestamp[0]), month = sms_decimal(timestamp[1]);
    unsigned day = sms_decimal(timestamp[2]), hour = sms_decimal(timestamp[3]);
    unsigned minute = sms_decimal(timestamp[4]), second = sms_decimal(timestamp[5]);
    unsigned zone = sms_decimal(timestamp[6] & 0xf7);
    if (year > 99 || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 ||
        minute > 59 || second > 59 || zone > 56) return NSDate.date;
    NSDateComponents *fields = [[NSDateComponents alloc] init];
    fields.year = (year >= 90 ? 1900 : 2000) + year;
    fields.month = month;
    fields.day = day;
    fields.hour = hour;
    fields.minute = minute;
    fields.second = second;
    int offset = (int)zone * 15 * 60;
    fields.timeZone = [NSTimeZone timeZoneForSecondsFromGMT:(timestamp[6] & 8) ? -offset : offset];
    NSCalendar *calendar = [[NSCalendar alloc] initWithCalendarIdentifier:NSCalendarIdentifierGregorian];
    return [calendar dateFromComponents:fields] ?: NSDate.date;
}

static NSUUID *sms_uuid(NSData *input)
{
    unsigned char sha[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(input.bytes, (CC_LONG)input.length, sha);
    sha[6] = (sha[6] & 0x0f) | 0x80;
    sha[8] = (sha[8] & 0x3f) | 0x80;
    return [[NSUUID alloc] initWithUUIDBytes:sha];
}

@interface UMISMSParts : NSObject
@property(nonatomic) NSDate *expires;
@property(nonatomic) NSDate *date;
@property(nonatomic) NSMutableDictionary<NSNumber *, NSData *> *parts;
@end
@implementation UMISMSParts
@end

static void sms_poll(void)
{
    static id lastSession;
    static uint64_t inflight;
    static int lastError;
    static NSMutableDictionary<NSString *, UMISMSParts *> *multipart;
    static NSMutableOrderedSet<NSUUID *> *completed;
    id account = [NSClassFromString(@"IMDAccountController") sharedAccountController];
    id session = [account anySessionForServiceName:@"SMS"];
    if (![session respondsToSelector:@selector(_processReceivedSMSMessage:storageContext:withCompletionBlock:)]) return;
    int error = sms_receipt(0, NO);
    if (session != lastSession || error != lastError) {
        lastSession = session;
        lastError = error;
        sms_log("Messages receiver lease errno=", error);
    }
    if (error) return;
    sms_install_sender(session);
    sms_send_poll();
    struct umi_sms_snapshot snapshot;
    if (!sms_read_snapshot(&snapshot) || !snapshot.sequence || snapshot.sequence == inflight) return;

    struct umi_sms_deliver message;
    if (snapshot.length > sizeof(snapshot.pdu) || !umi_sms_deliver_decode(snapshot.pdu, snapshot.length, &message)) {
        sms_log("unsupported or malformed TPDU, bytes=", snapshot.length);
        sms_receipt(snapshot.sequence, NO);
        return;
    }
    if (message.silent) {
        sms_receipt(snapshot.sequence, YES);
        return;
    }
    NSString *sender = [NSString stringWithCharacters:message.sender length:message.sender_length];
    NSDate *date = sms_date(message.timestamp);
    NSData *text = [NSData dataWithBytes:message.text length:message.text_length * sizeof(uint16_t)];
    NSData *identity = [NSData dataWithBytes:snapshot.pdu length:snapshot.length];
    NSString *partKey = nil;
    if (message.parts > 1) {
        if (!multipart) multipart = [[NSMutableDictionary alloc] init];
        for (NSString *key in multipart.allKeys)
            if ([multipart[key].expires timeIntervalSinceNow] <= 0) [multipart removeObjectForKey:key];
        partKey = [NSString stringWithFormat:@"%u:%@:%u:%u:%u",
            snapshot.subscription, sender, message.long_reference, message.reference, message.parts];
        UMISMSParts *group = multipart[partKey];
        if (!group) {
            if (multipart.count >= 64) {
                sms_receipt(snapshot.sequence, NO);
                return;
            }
            group = [[UMISMSParts alloc] init];
            group.expires = [NSDate dateWithTimeIntervalSinceNow:15 * 60];
            group.date = date;
            group.parts = [[NSMutableDictionary alloc] init];
            multipart[partKey] = group;
        }
        group.parts[@(message.part)] = text;
        if (message.part == 1) group.date = date;
        if (group.parts.count < message.parts) {
            sms_log("segment queued, total expected=", message.parts);
            sms_receipt(snapshot.sequence, YES);
            return;
        }
        date = group.date;
        NSMutableData *assembled = [[NSMutableData alloc] init];
        for (unsigned i = 1; i <= message.parts; i++) [assembled appendData:group.parts[@(i)]];
        text = assembled;
        NSMutableData *fingerprint = [NSMutableData dataWithData:[partKey dataUsingEncoding:NSUTF8StringEncoding]];
        NSTimeInterval stamp = date.timeIntervalSince1970;
        [fingerprint appendBytes:&stamp length:sizeof(stamp)];
        [fingerprint appendData:assembled];
        identity = fingerprint;
    }

    NSString *body = [[NSString alloc] initWithData:text encoding:NSUTF16LittleEndianStringEncoding];
    if (!body || !sender.length) {
        sms_receipt(snapshot.sequence, NO);
        return;
    }
    NSUUID *uuid = sms_uuid(identity);
    if ([completed containsObject:uuid]) {
        sms_receipt(snapshot.sequence, YES);
        return;
    }
    UMIPlainSMSPart *part = [[UMIPlainSMSPart alloc] init];
    part.plainTextContentBody = body;
    UMIPlainSMS *sms = [[UMIPlainSMS alloc] init];
    sms.sender = sender;
    sms.countryCode = NSLocale.currentLocale.countryCode.lowercaseString;
    sms.GUID = uuid.UUIDString;
    sms.date = date;
    sms.items = @[part];
    uint64_t sequence = snapshot.sequence;
    inflight = sequence;
    sms_log("delivering plain SMS to Messages", 0);
    [session _processReceivedSMSMessage:sms storageContext:nil withCompletionBlock:^(BOOL accepted, NSError *failure) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (accepted) {
                if (!completed) completed = [[NSMutableOrderedSet alloc] init];
                if (completed.count >= 128) [completed removeObjectAtIndex:0];
                [completed addObject:uuid];
                if (partKey) [multipart removeObjectForKey:partKey];
            }
            sms_log("Messages completion accepted=", accepted);
            if (failure) sms_log("Messages completion error=", (int)failure.code);
            int receiptError = sms_receipt(sequence, accepted);
            if (receiptError) sms_log("receipt errno=", receiptError);
            if (inflight == sequence) inflight = 0;
        });
    }];
}

__attribute__((constructor)) static void start_sms_receiver(void)
{
    if (strcmp(getprogname(), "imagent")) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        char args[1024];
        size_t size = sizeof(args);
        if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) || size > sizeof(args) ||
            !memchr(args, 0, size) || !strstr(args, " sdx55_cell=1 ")) return;
        sms_log("receiver started", 0);
        static dispatch_source_t timer;
        timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
        dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC),
            NSEC_PER_SEC, 100 * NSEC_PER_MSEC);
        dispatch_source_set_event_handler(timer, ^{ @autoreleasepool { sms_poll(); } });
        dispatch_resume(timer);
    });
}
