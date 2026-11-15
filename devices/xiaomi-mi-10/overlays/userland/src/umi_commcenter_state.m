/* Project the active SDX55 subscription and Internet bearer into 20F66 CommCenter. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/sysctl.h>
#include <sys/umi_sdx55_cell_status.h>

@interface CTXPCServiceSubscriptionContext : NSObject <NSCopying>
+ (instancetype)contextWithSlot:(NSInteger)slot;
- (NSInteger)slotID;
- (NSUUID *)uuid;
- (void)setIsSimPresent:(BOOL)present;
- (void)setIsSimGood:(BOOL)ready;
- (void)setUserDataPreferred:(NSNumber *)preferred;
- (void)setUserDefaultVoice:(NSNumber *)preferred;
@end

@interface CTXPCServiceSubscriptionInfo : NSObject <NSCopying>
- (NSArray *)subscriptions;
- (void)setSubscriptions:(NSArray *)subscriptions;
- (void)setSubscriptionsValid:(NSNumber *)valid;
@end

@interface CTXPCContexts : NSObject <NSCopying>
- (NSArray *)subscriptions;
- (void)setSubscriptions:(NSArray *)subscriptions;
- (void)setDataPreferred:(NSUUID *)preferred;
- (void)setVoicePreferred:(NSUUID *)preferred;
@end

@interface CTXPCContextInfo : NSObject
+ (instancetype)contextWithUUID:(NSUUID *)uuid andAccountID:(NSString *)account andSlot:(NSInteger)slot;
- (NSInteger)slotID;
- (NSUUID *)uuid;
@end

@interface CTDataConnectionStatus : NSObject <NSCopying>
- (void)setPdp:(NSNumber *)pdp;
- (void)setState:(int)state;
- (void)setInterfaceName:(NSString *)name;
- (void)setApnName:(NSString *)name;
- (void)setIpFamily:(int)family;
- (void)setSuspended:(BOOL)suspended;
- (void)setPublicNetAllowed:(BOOL)allowed;
@end

@interface CTDataStatus : NSObject <NSCopying>
- (void)setAttached:(BOOL)attached;
- (void)setDataSim:(BOOL)dataSim;
- (void)setCellularDataPossible:(BOOL)possible;
- (void)setActiveContexts:(unsigned)active;
- (void)setTotalActiveContexts:(unsigned)active;
@end

extern NSString *const kCTSIMSupportSIMStatusReady;
extern NSString *const kCTSIMSupportSIMStatusInserted;

enum {
    /* iPhone10,3 presents one telephony line. UIM chooses its physical card. */
    logicalSlot = 1,
    internetContext = 0,
    activeContext = 2,
    ipv4Context = 1,
};

typedef void (^Result)(id value, NSError *error);
typedef void (*ContextMethod)(id self, SEL name, id context, Result result);

static BOOL read_cell(struct umi_sdx55_cell_snapshot *cell)
{
    size_t length = sizeof(*cell);
    if (sysctlbyname(UMI_SDX55_CELL_SYSCTL, cell, &length, NULL, 0) ||
        length != sizeof(*cell) || cell->size != length || cell->version != UMI_SDX55_CELL_VERSION ||
        !cell->active || !(cell->valid & UMI_CELL_UIM)) return NO;
    /* A single native line is selectable only with an unambiguous physical card. */
    return cell->present_mask && !(cell->present_mask & (cell->present_mask - 1));
}

static BOOL read_radio(struct umi_sdx55_radio_snapshot *radio)
{
    size_t length = sizeof(*radio);
    return !sysctlbyname(UMI_SDX55_RADIO_SYSCTL, radio, &length, NULL, 0) &&
        length == sizeof(*radio) && radio->size == length && radio->version == UMI_SDX55_RADIO_VERSION &&
        radio->state != UMI_DATA_UNAVAILABLE;
}

static BOOL has_internet_bearer(struct umi_sdx55_cell_snapshot cell, struct umi_sdx55_radio_snapshot radio)
{
    if (radio.state != UMI_DATA_ON || !radio.enabled || !cell.nas_subscription ||
        cell.nas_subscription != cell.wds_subscription || !cell.ready_mask) return NO;
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list)) return NO;
    BOOL live = NO;
    for (struct ifaddrs *entry = list; entry; entry = entry->ifa_next) {
        if (strcmp(entry->ifa_name, "en1") || !(entry->ifa_flags & IFF_UP) ||
            !entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET) continue;
        uint32_t address = ntohl(((struct sockaddr_in *)entry->ifa_addr)->sin_addr.s_addr);
        if (address && (address >> 16) != 0xa9fe) {
            live = YES;
            break;
        }
    }
    freeifaddrs(list);
    return live;
}

static BOOL maps_line(id context, struct umi_sdx55_cell_snapshot *cell)
{
    Class cls = NSClassFromString(@"CTXPCServiceSubscriptionContext");
    return (!context || ([context isKindOfClass:cls] && [context slotID] == logicalSlot)) && read_cell(cell);
}

static CTXPCServiceSubscriptionContext *subscription(id existing)
{
    struct umi_sdx55_cell_snapshot cell;
    if (!read_cell(&cell)) return nil;
    CTXPCServiceSubscriptionContext *context = [existing copy];
    if (!context) context = [NSClassFromString(@"CTXPCServiceSubscriptionContext") contextWithSlot:logicalSlot];
    [context setIsSimPresent:YES];
    [context setIsSimGood:cell.ready_mask != 0];
    if (cell.nas_subscription && cell.ready_mask) {
        [context setUserDefaultVoice:@YES];
        [context setUserDataPreferred:@YES];
    }
    return context;
}

static NSArray *reported_subscriptions(NSArray *existing)
{
    if ((existing && ![existing isKindOfClass:NSArray.class]) || existing.count > 1 ||
        (existing.count && (![existing.firstObject isKindOfClass:NSClassFromString(@"CTXPCServiceSubscriptionContext")] ||
        [existing.firstObject slotID] != logicalSlot))) return existing;
    CTXPCServiceSubscriptionContext *context = subscription(existing.firstObject);
    return context ? @[context] : existing;
}

static id (*original_info)(id, SEL);
static id report_info(id self, SEL name)
{
    CTXPCServiceSubscriptionInfo *stock = original_info(self, name);
    if (stock && ![stock isKindOfClass:NSClassFromString(@"CTXPCServiceSubscriptionInfo")]) return stock;
    NSArray *existing = [stock subscriptions];
    NSArray *line = reported_subscriptions(existing);
    if (line == existing) return stock;
    CTXPCServiceSubscriptionInfo *value = [stock copy] ?: [NSClassFromString(@"CTXPCServiceSubscriptionInfo") new];
    [value setSubscriptions:line];
    [value setSubscriptionsValid:@YES];
    return value;
}

static id (*original_active)(id, SEL);
static id report_active(id self, SEL name)
{
    CTXPCContexts *stock = original_active(self, name);
    struct umi_sdx55_cell_snapshot cell;
    Class cls = NSClassFromString(@"CTXPCContexts");
    if ((stock && ![stock isKindOfClass:cls]) || !read_cell(&cell) || !cell.ready_mask) return stock;
    NSArray *existing = [stock subscriptions];
    if (existing.count) return stock;
    CTXPCServiceSubscriptionContext *service =
        [NSClassFromString(@"CTXPCServiceSubscriptionContext") contextWithSlot:logicalSlot];
    NSUUID *uuid = [service uuid];
    if (!uuid) return stock;
    /* The secure decoder expects ContextInfo inside Contexts and
     * ServiceSubscriptionContext inside ServiceSubscriptionInfo. */
    CTXPCContextInfo *context = [NSClassFromString(@"CTXPCContextInfo")
        contextWithUUID:uuid andAccountID:nil andSlot:logicalSlot];
    if (!context) return stock;
    CTXPCContexts *value = [stock copy] ?: [cls new];
    [value setSubscriptions:@[context]];
    if (cell.nas_subscription) {
        [value setDataPreferred:uuid];
        [value setVoicePreferred:uuid];
    }
    return value;
}

static ContextMethod original_sim;
static void report_sim(id self, SEL name, id context, Result result)
{
    if (!result) { original_sim(self, name, context, result); return; }
    original_sim(self, name, context, ^(id value, NSError *error) {
        struct umi_sdx55_cell_snapshot cell;
        if (!error && maps_line(context, &cell)) {
            value = cell.ready_mask ? kCTSIMSupportSIMStatusReady : kCTSIMSupportSIMStatusInserted;
        }
        result(value, error);
    });
}

static ContextMethod original_operator, original_localized_operator, original_short_operator;
static void report_operator(id self, SEL name, id context, Result result)
{
    ContextMethod original = original_operator;
    if (name == @selector(copyLocalizedOperatorName:completion:)) original = original_localized_operator;
    if (name == @selector(copyAbbreviatedOperatorName:completion:)) original = original_short_operator;
    if (!result) { original(self, name, context, result); return; }
    original(self, name, context, ^(id value, NSError *error) {
        struct umi_sdx55_cell_snapshot cell;
        struct umi_sdx55_radio_snapshot radio;
        if (!error && maps_line(context, &cell) && cell.nas_subscription && read_radio(&radio) && radio.rat) {
            NSString *network = [[NSString alloc] initWithBytes:radio.operator_name
                length:strnlen(radio.operator_name, sizeof(radio.operator_name)) encoding:NSUTF8StringEncoding];
            if (network.length) value = network;
        }
        result(value, error);
    });
}

static ContextMethod original_data;
static void report_data(id self, SEL name, id context, Result result)
{
    if (!result) { original_data(self, name, context, result); return; }
    original_data(self, name, context, ^(id value, NSError *error) {
        struct umi_sdx55_cell_snapshot cell;
        struct umi_sdx55_radio_snapshot radio;
        if (!error && [value isKindOfClass:NSClassFromString(@"CTDataStatus")] &&
            maps_line(context, &cell) && read_radio(&radio)) {
            CTDataStatus *status = [value copy];
            BOOL live = has_internet_bearer(cell, radio);
            [status setAttached:live || (cell.ps_attach == 1 && (cell.valid & UMI_CELL_NAS))];
            [status setDataSim:cell.nas_subscription != 0];
            [status setCellularDataPossible:radio.rat != 0];
            [status setActiveContexts:live];
            [status setTotalActiveContexts:live];
            value = status;
        }
        result(value, error);
    });
}

static void (*original_connection)(id, SEL, id, int, Result);
static void report_connection(id self, SEL name, id context, int type, Result result)
{
    if (!result || type != internetContext) {
        original_connection(self, name, context, type, result);
        return;
    }
    original_connection(self, name, context, type, ^(id value, NSError *error) {
        struct umi_sdx55_cell_snapshot cell;
        struct umi_sdx55_radio_snapshot radio;
        if (!error && [value isKindOfClass:NSClassFromString(@"CTDataConnectionStatus")] &&
            maps_line(context, &cell) && read_radio(&radio) && has_internet_bearer(cell, radio)) {
            CTDataConnectionStatus *status = [value copy];
            [status setPdp:@(internetContext)];
            [status setInterfaceName:@"en1"];
            [status setApnName:nil];
            [status setState:activeContext];
            [status setIpFamily:ipv4Context];
            [status setSuspended:NO];
            [status setPublicNetAllowed:YES];
            value = status;
        }
        result(value, error);
    });
}

void umi_commcenter_install_state_bridge(void)
{
    Class server = NSClassFromString(@"CTXPCServer");
    Class client = NSClassFromString(@"CTXPCClientHandler");
    const struct {
        Class cls;
        const char *name, *encoding;
        IMP replacement;
        IMP *original;
    } hooks[] = {
        {server, "subscriptionInfo", "@16@0:8", (IMP)report_info, (IMP *)&original_info},
        {server, "activeSubscriptions", "@16@0:8", (IMP)report_active, (IMP *)&original_active},
        {client, "getSIMStatus:completion:", "v32@0:8@16@?24", (IMP)report_sim, (IMP *)&original_sim},
        {client, "copyOperatorName:completion:", "v32@0:8@16@?24", (IMP)report_operator, (IMP *)&original_operator},
        {client, "copyLocalizedOperatorName:completion:", "v32@0:8@16@?24", (IMP)report_operator, (IMP *)&original_localized_operator},
        {client, "copyAbbreviatedOperatorName:completion:", "v32@0:8@16@?24", (IMP)report_operator, (IMP *)&original_short_operator},
        {client, "getDataStatus:completion:", "v32@0:8@16@?24", (IMP)report_data, (IMP *)&original_data},
        {client, "getConnectionState:connectionType:completion:", "v36@0:8@16i24@?28", (IMP)report_connection, (IMP *)&original_connection},
    };
    Method methods[sizeof(hooks) / sizeof(hooks[0])];
    for (size_t i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++) {
        methods[i] = class_getInstanceMethod(hooks[i].cls, sel_registerName(hooks[i].name));
        if (!methods[i] || strcmp(method_getTypeEncoding(methods[i]), hooks[i].encoding)) {
            fprintf(stderr, "UMI CC state: 20F66 method mismatch %s\n", hooks[i].name);
            return;
        }
    }
    for (size_t i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++) {
        *hooks[i].original = method_setImplementation(methods[i], hooks[i].replacement);
    }
    fprintf(stderr, "UMI CC state: SDX55 subscription and Internet PDP queries installed\n");
}
