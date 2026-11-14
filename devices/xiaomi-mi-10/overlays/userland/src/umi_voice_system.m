/* Translate CallKit transactions to the SDX55 voice worker. */
#import <CallKit/CallKit.h>
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#include <notify.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "umi_voice_system.h"

@interface CXCallUpdate (UMISystemCallCapabilities)
- (void)setSupportsAddCall:(BOOL)supported;
- (void)setSupportsDTMF:(BOOL)supported;
@end

@interface FBSOpenApplicationOptions : NSObject
+ (instancetype)optionsWithDictionary:(NSDictionary *)options;
@end

@interface FBSOpenApplicationService : NSObject
+ (instancetype)serviceWithDefaultShellEndpoint;
- (void)openApplication:(NSString *)bundleIdentifier withOptions:(FBSOpenApplicationOptions *)options
    completion:(void (^)(id process, NSError *error))completion;
@end

extern NSString * const FBSOpenApplicationOptionKeyPayloadURL;

@interface TUProxyCall : NSObject
- (NSString *)callUUID;
- (id)provider;
- (BOOL)isOutgoing;
@end

@interface TUCallCenter : NSObject
+ (id)sharedInstance;
- (TUProxyCall *)frontmostAudioOrVideoCall;
@end

@interface NSObject (UMISystemCallRoute)
- (BOOL)isReceiver;
- (BOOL)isSpeaker;
@end

@class SBSUIInCallWindowScene;
@interface ICSApplicationDelegate : UIResponder
- (id)configurationForCall:(TUProxyCall *)call;
- (void)performPresentationWithConfiguration:(id)configuration forSceneType:(NSInteger)type;
- (SBSUIInCallWindowScene *)currentInCallScene;
@end

@interface SBSUIInCallWindowScene : UIWindowScene
- (NSInteger)presentationMode;
- (void)requestTransitionToPresentationMode:(NSInteger)mode isUserInitiated:(BOOL)userInitiated;
@end

@interface SBSInCallPresentationConfiguration : NSObject
- (void)setPreferredPresentationMode:(NSInteger)mode;
- (void)setUserInitiated:(BOOL)userInitiated;
@end

@interface PHAudioCallControlsView : UIView
- (void)updateControls;
@end

@interface PHAudioCallControlsViewController : UIViewController
- (PHAudioCallControlsView *)controlsView;
@end

static NSString * const incallAcknowledgementPrefix = @"org.hackios.umi.incall.";
static const NSInteger fullscreen_call_mode = 2;
/* The two audio control types in 20F66 are the speaker toggle and route picker. */
static const NSInteger speaker_control = 2;
static const NSInteger audio_route_control = 3;

@interface UMISystemCall : NSObject <CXProviderDelegate>
- (void)startNumber:(NSString *)number fallback:(void (^)(BOOL))fallback;
@end

@implementation UMISystemCall {
    CXProvider *_provider;
    CXCallController *_controller;
    NSUUID *_callID;
    NSString *_number;
    void (^_fallback)(BOOL);
    dispatch_source_t _timer;
    CXEndCallAction *_endAction;
    CXSetMutedCallAction *_muteAction;
    BOOL _submitted, _connected, _ready, _transactionRequested, _presentationRequested;
    UIBackgroundTaskIdentifier _backgroundTask;
    int _presentationToken;
}

- (instancetype)init
{
    self = [super init];
    if (!self) return nil;
    CXProviderConfiguration *config = [[CXProviderConfiguration alloc] init];
    config.supportedHandleTypes = [NSSet setWithObject:@(CXHandleTypePhoneNumber)];
    config.maximumCallGroups = 1;
    config.maximumCallsPerCallGroup = 1;
    _provider = [[CXProvider alloc] initWithConfiguration:config];
    _controller = [[CXCallController alloc] init];
    _backgroundTask = UIBackgroundTaskInvalid;
    _presentationToken = -1;
    [_provider setDelegate:self queue:dispatch_get_main_queue()];
    return self;
}

- (void)clearCall
{
    if (_presentationToken != -1) {
        notify_cancel(_presentationToken);
        _presentationToken = -1;
    }
    if (_timer) {
        dispatch_source_cancel(_timer);
        _timer = nil;
    }
    if (_backgroundTask != UIBackgroundTaskInvalid) {
        [UIApplication.sharedApplication endBackgroundTask:_backgroundTask];
        _backgroundTask = UIBackgroundTaskInvalid;
    }
    _callID = nil;
    _number = nil;
    _fallback = nil;
    _endAction = nil;
    _muteAction = nil;
    _submitted = _connected = _transactionRequested = _presentationRequested = NO;
}

- (void)fallbackBeforeDial
{
    if (!_callID || _submitted) return;
    void (^fallback)(BOOL) = _fallback;
    [self clearCall];
    if (fallback) fallback(NO);
}

- (void)startNumber:(NSString *)number fallback:(void (^)(BOOL))fallback
{
    if (_callID) return;
    NSUUID *callID = NSUUID.UUID;
    _callID = callID;
    _number = [number copy];
    _fallback = [fallback copy];
    [self observePresentationForCall:callID];
    if (_ready) [self requestStart];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if ([self->_callID isEqual:callID] && !self->_submitted) {
            umi_voice_diagnostic("UMI Phone: system call transaction timed out\n");
            [self fallbackBeforeDial];
        }
    });
}

- (void)requestStart
{
    if (!_ready || !_callID || _transactionRequested) return;
    _transactionRequested = YES;
    NSUUID *callID = _callID;
    CXHandle *handle = [[CXHandle alloc] initWithType:CXHandleTypePhoneNumber value:_number];
    CXStartCallAction *action = [[CXStartCallAction alloc] initWithCallUUID:callID handle:handle];
    [_controller requestTransaction:[[CXTransaction alloc] initWithAction:action] completion:^(NSError *error) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (![self->_callID isEqual:callID] || !error || self->_submitted) return;
            char message[160];
            snprintf(message, sizeof(message), "UMI Phone: system call transaction rejected %s=%ld\n",
                error.domain.UTF8String, (long)error.code);
            umi_voice_diagnostic(message);
            [self fallbackBeforeDial];
        });
    }];
}

- (void)poll
{
    struct umi_voice_snapshot status = {0};
    if (!_submitted || umi_voice_read_status(&status)) return;
    if (status.state == UMI_VOICE_ACTIVE && !_connected) {
        _connected = YES;
        [_provider reportOutgoingCallWithUUID:_callID connectedAtDate:NSDate.date];
        umi_voice_diagnostic("UMI Phone: system call connected\n");
        NSUUID *callID = _callID;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            if ([self->_callID isEqual:callID]) [self presentCall:callID];
        });
    }
    if (_muteAction && status.state == UMI_VOICE_ACTIVE) {
        struct umi_voice_mute_status mute = {0};
        size_t size = sizeof(mute);
        if (sysctlbyname(UMI_VOICE_MUTE_SYSCTL, &mute, &size, NULL, 0) || size != sizeof(mute)) {
            [_muteAction fail];
            _muteAction = nil;
        } else if (!mute.pending) {
            if (mute.error || mute.muted != (uint32_t)_muteAction.muted) [_muteAction fail];
            else [_muteAction fulfill];
            _muteAction = nil;
        }
    }
    if (status.state == UMI_VOICE_ENDED || status.state == UMI_VOICE_FAILED || status.state == UMI_VOICE_UNAVAILABLE) {
        BOOL ended = status.state == UMI_VOICE_ENDED;
        if (_endAction) {
            if (ended) [_endAction fulfill];
            else [_endAction fail];
        }
        if (!_endAction || !ended) {
            [_provider reportCallWithUUID:_callID endedAtDate:NSDate.date
                reason:ended ? CXCallEndedReasonRemoteEnded : CXCallEndedReasonFailed];
        }
        [_muteAction fail];
        umi_voice_diagnostic("UMI Phone: system call finished\n");
        [self clearCall];
    }
}

- (void)observePresentationForCall:(NSUUID *)callID
{
    /* CallKit can present InCallService as soon as it accepts the transaction. */
    NSString *acknowledgement = [incallAcknowledgementPrefix stringByAppendingString:callID.UUIDString];
    if (notify_register_dispatch(acknowledgement.UTF8String, &_presentationToken,
        dispatch_get_main_queue(), ^(int token) {
            if (self->_presentationToken != token || ![self->_callID isEqual:callID] || !self->_fallback) return;
            self->_fallback = nil;
            umi_voice_diagnostic("UMI Phone: native call view appeared\n");
        }) != NOTIFY_STATUS_OK) _presentationToken = -1;
}

- (void)presentCall:(NSUUID *)callID
{
    if (_presentationRequested || _endAction || !_fallback) return;
    _presentationRequested = YES;
    /* The URL brings the existing call to the foreground. Target InCallService
     * explicitly since FaceTime also handles the URL scheme. */
    FBSOpenApplicationService *service = [FBSOpenApplicationService serviceWithDefaultShellEndpoint];
    NSURL *url = [NSURL URLWithString:@"facetime://?showInCallUI=1"];
    FBSOpenApplicationOptions *options = [FBSOpenApplicationOptions optionsWithDictionary:@{
        FBSOpenApplicationOptionKeyPayloadURL: url,
    }];
    [service openApplication:@"com.apple.InCallService" withOptions:options completion:^(__unused id process, NSError *error) {
        char message[168];
        snprintf(message, sizeof(message), "UMI Phone: InCallService launch %s=%ld\n",
            error.domain.UTF8String ?: "accepted", (long)error.code);
        umi_voice_diagnostic(message);
    }];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 4 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if (![self->_callID isEqual:callID] || !self->_fallback) return;
        void (^fallback)(BOOL) = self->_fallback;
        self->_fallback = nil;
        umi_voice_diagnostic("UMI Phone: native view did not appear; using call controls\n");
        fallback(YES);
    });
}

- (void)providerDidBegin:(__unused CXProvider *)provider
{
    _ready = YES;
    umi_voice_diagnostic("UMI Phone: system call provider ready\n");
    [self requestStart];
}

- (void)providerDidReset:(__unused CXProvider *)provider
{
    _ready = NO;
    if (_submitted) {
        (void)umi_voice_submit(UMI_VOICE_END, nil);
        [self clearCall];
    } else {
        [self fallbackBeforeDial];
    }
}

- (void)provider:(CXProvider *)provider performStartCallAction:(CXStartCallAction *)action
{
    if (![_callID isEqual:action.callUUID] || _submitted || ![_number isEqualToString:action.handle.value]) {
        [action fail];
        return;
    }

    if (umi_voice_submit(UMI_VOICE_DIAL, _number)) {
        [action fail];
        [self clearCall];
        return;
    }
    _submitted = YES;
    _backgroundTask = [UIApplication.sharedApplication beginBackgroundTaskWithName:@"Cellular call" expirationHandler:^{
        if (self->_submitted) (void)umi_voice_submit(UMI_VOICE_END, nil);
        if (self->_backgroundTask != UIBackgroundTaskInvalid) {
            [UIApplication.sharedApplication endBackgroundTask:self->_backgroundTask];
            self->_backgroundTask = UIBackgroundTaskInvalid;
        }
    }];
    [action fulfill];
    [provider reportOutgoingCallWithUUID:_callID startedConnectingAtDate:NSDate.date];
    CXCallUpdate *update = [[CXCallUpdate alloc] init];
    update.remoteHandle = action.handle;
    update.supportsHolding = NO;
    update.supportsGrouping = NO;
    update.supportsUngrouping = NO;
    if ([update respondsToSelector:@selector(setSupportsAddCall:)]) [update setSupportsAddCall:NO];
    if ([update respondsToSelector:@selector(setSupportsDTMF:)]) [update setSupportsDTMF:NO];
    [provider reportCallWithUUID:_callID updated:update];
    umi_voice_diagnostic(_ready ? "UMI Phone: system call started\n" : "UMI Phone: system call started before begin callback\n");
    _timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(_timer, DISPATCH_TIME_NOW, NSEC_PER_SEC / 4, NSEC_PER_MSEC * 30);
    dispatch_source_set_event_handler(_timer, ^{ [self poll]; });
    dispatch_resume(_timer);

    /* Retain a hangup control if the carrier rings without answering. */
    NSUUID *callID = _callID;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 4 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if ([self->_callID isEqual:callID]) [self presentCall:callID];
    });
}

- (void)provider:(__unused CXProvider *)provider performEndCallAction:(CXEndCallAction *)action
{
    if (![_callID isEqual:action.callUUID] || !_submitted || _endAction) {
        [action fail];
        return;
    }
    _endAction = action;
    umi_voice_diagnostic("UMI Phone: system hangup requested\n");
    int error = umi_voice_submit(UMI_VOICE_END, nil);
    [self poll];
    if (error && _endAction) {
        [_endAction fail];
        _endAction = nil;
    }
}

- (void)provider:(__unused CXProvider *)provider timedOutPerformingAction:(CXAction *)action
{
    if (action == _endAction) _endAction = nil;
    if (action == _muteAction) _muteAction = nil;
}

- (void)provider:(__unused CXProvider *)provider performSetMutedCallAction:(CXSetMutedCallAction *)action
{
    if (!_connected || ![_callID isEqual:action.callUUID] || _endAction || _muteAction) {
        [action fail];
        return;
    }
    uint32_t requested = action.muted;
    if (sysctlbyname(UMI_VOICE_MUTE_SYSCTL, NULL, NULL, &requested, sizeof(requested))) {
        [action fail];
        return;
    }
    _muteAction = action;
    [self poll];
}
@end

static UMISystemCall *system_call(void)
{
    if (access("/redwood/bin/callservicesd", X_OK)) return nil;
    static UMISystemCall *bridge;
    if (!bridge) bridge = [[UMISystemCall alloc] init];
    return bridge;
}

BOOL umi_voice_start_system_call(NSString *number, void (^fallback)(BOOL))
{
    UMISystemCall *bridge = system_call();
    if (!bridge) return NO;
    [bridge startNumber:number fallback:fallback];
    return YES;
}

/* The audio buttons live in InCallService. For the active SDX55 call, apply
 * its chosen built-in audio device to the modem's DSP route as well. */
static void (*original_pick_route)(id, SEL, id);
static void (*original_audio_call_view_appeared)(id, SEL, BOOL);
static void (*original_request_call_presentation)(ICSApplicationDelegate *, SEL, TUProxyCall *, id, BOOL);
static BOOL (*original_present_dismissed)(ICSApplicationDelegate *, SEL, TUProxyCall *);
static BOOL (*original_auto_punch_out)(TUCallCenter *, SEL);
static BOOL (*original_activate_provider)(TUCallCenter *, SEL, TUProxyCall *);
static BOOL (*original_audio_control_enabled)(PHAudioCallControlsViewController *, SEL, NSInteger);
static BOOL (*original_audio_control_selected)(PHAudioCallControlsViewController *, SEL, NSInteger);
static void (*original_audio_control_tapped)(PHAudioCallControlsViewController *, SEL, NSInteger, UIView *);
static void (*original_audio_controls_appeared)(PHAudioCallControlsViewController *, SEL, BOOL);

static BOOL owns_sdx55_call(TUProxyCall *call)
{
    id provider = [call provider];
    return [call isOutgoing] && [[provider bundleIdentifier] isEqualToString:@"com.apple.mobilephone"];
}

/* Phone owns the CallKit transaction and stays alive for the duration of the
 * call. InCallService would otherwise reopen Phone as a VoIP provider. */
static BOOL auto_punch_out(TUCallCenter *center, SEL command)
{
    BOOL open = original_auto_punch_out(center, command);
    if (open && owns_sdx55_call([center frontmostAudioOrVideoCall])) {
        umi_voice_diagnostic("UMI InCallService: retained the system call page instead of opening Phone\n");
        return NO;
    }
    return open;
}

static BOOL activate_provider(TUCallCenter *center, SEL command, TUProxyCall *call)
{
    BOOL open = original_activate_provider(center, command, call);
    if (open && owns_sdx55_call(call)) {
        umi_voice_diagnostic("UMI InCallService: Phone provider is already active\n");
        return NO;
    }
    return open;
}

static BOOL present_dismissed(ICSApplicationDelegate *delegate, SEL command, TUProxyCall *call)
{
    /* The stock configuration initially dismisses third-party outgoing calls.
     * The Phone keypad is the primary calling UI for these SDX55 calls. */
    if (owns_sdx55_call(call)) return NO;
    return original_present_dismissed(delegate, command, call);
}

static void request_call_presentation(ICSApplicationDelegate *delegate, SEL command, TUProxyCall *call,
    id dialRequest, BOOL tappedPill)
{
    if (tappedPill && owns_sdx55_call(call)) {
        SBSUIInCallWindowScene *scene = [delegate currentInCallScene];
        if ([scene respondsToSelector:@selector(requestTransitionToPresentationMode:isUserInitiated:)]) {
            if ([scene presentationMode] != fullscreen_call_mode ||
                scene.activationState != UISceneActivationStateForegroundActive) {
                [scene requestTransitionToPresentationMode:fullscreen_call_mode isUserInitiated:YES];
                umi_voice_diagnostic("UMI InCallService: requested transition to the full-screen call page\n");
            }
            return;
        }
        SBSInCallPresentationConfiguration *configuration = [delegate configurationForCall:call];
        if (configuration && [configuration respondsToSelector:@selector(setUserInitiated:)]) {
            [configuration setPreferredPresentationMode:fullscreen_call_mode];
            [configuration setUserInitiated:YES];
            [delegate performPresentationWithConfiguration:configuration forSceneType:0];
            umi_voice_diagnostic("UMI InCallService: requested a full-screen call scene\n");
            return;
        }
    }
    original_request_call_presentation(delegate, command, call, dialRequest, tappedPill);
}

static BOOL acknowledge_full_screen_call(UIViewController *controller, NSUUID *uuid)
{
    Class inCallScene = NSClassFromString(@"SBSUIInCallWindowScene");
    UIWindow *window = controller.viewIfLoaded.window;
    SBSUIInCallWindowScene *scene = (id)window.windowScene;
    if (window && !window.hidden && [scene isKindOfClass:inCallScene] &&
        [scene presentationMode] == fullscreen_call_mode && scene.activationState == UISceneActivationStateForegroundActive) {
        NSString *name = [incallAcknowledgementPrefix stringByAppendingString:uuid.UUIDString];
        notify_post(name.UTF8String);
        umi_voice_diagnostic("UMI InCallService: full-screen audio call is active\n");
        return YES;
    }
    return NO;
}

static void verify_audio_call_view(UIViewController *controller)
{
    Class center = NSClassFromString(@"TUCallCenter");
    TUProxyCall *call = [[center sharedInstance] frontmostAudioOrVideoCall];
    if (!owns_sdx55_call(call)) return;
    id identifier = [call callUUID];
    NSUUID *uuid = [identifier isKindOfClass:NSString.class] ? [[NSUUID alloc] initWithUUIDString:identifier] : nil;
    if (!uuid || acknowledge_full_screen_call(controller, uuid)) return;
    __block BOOL shown = NO;
    for (int step = 1; step <= 4; step++) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, step * NSEC_PER_SEC / 2), dispatch_get_main_queue(), ^{
            TUProxyCall *foreground = [[center sharedInstance] frontmostAudioOrVideoCall];
            if (!shown && [[foreground callUUID] isEqual:identifier])
                shown = acknowledge_full_screen_call(controller, uuid);
        });
    }
}

static void audio_call_view_appeared(id controller, SEL command, BOOL animated)
{
    original_audio_call_view_appeared(controller, command, animated);
    verify_audio_call_view(controller);
}

static BOOL read_incall_route(struct umi_voice_route_status *route)
{
    size_t size = sizeof(*route);
    return !sysctlbyname(UMI_VOICE_ROUTE_SYSCTL, route, &size, NULL, 0) && size == sizeof(*route) &&
        (route->route == UMI_VOICE_ROUTE_RECEIVER || route->route == UMI_VOICE_ROUTE_SPEAKER);
}

static BOOL is_sdx55_audio_control(NSInteger type)
{
    if (type != speaker_control && type != audio_route_control) return NO;
    return owns_sdx55_call([[NSClassFromString(@"TUCallCenter") sharedInstance] frontmostAudioOrVideoCall]);
}

static BOOL audio_control_enabled(PHAudioCallControlsViewController *controller, SEL command, NSInteger type)
{
    if (!is_sdx55_audio_control(type)) return original_audio_control_enabled(controller, command, type);
    struct umi_voice_route_status route = {0};
    return read_incall_route(&route) && !route.pending;
}

static BOOL audio_control_selected(PHAudioCallControlsViewController *controller, SEL command, NSInteger type)
{
    if (!is_sdx55_audio_control(type)) return original_audio_control_selected(controller, command, type);
    struct umi_voice_route_status route = {0};
    return read_incall_route(&route) && route.route == UMI_VOICE_ROUTE_SPEAKER;
}

static void refresh_audio_control(PHAudioCallControlsViewController *controller, unsigned remaining)
{
    [controller.controlsView updateControls];
    struct umi_voice_route_status route = {0};
    if (remaining && (!read_incall_route(&route) || route.pending)) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_MSEC * 200), dispatch_get_main_queue(), ^{
            if (controller.viewIfLoaded.window) refresh_audio_control(controller, remaining - 1);
        });
    }
}

static void audio_controls_appeared(PHAudioCallControlsViewController *controller, SEL command, BOOL animated)
{
    original_audio_controls_appeared(controller, command, animated);
    if (is_sdx55_audio_control(speaker_control)) refresh_audio_control(controller, 15);
}

static void audio_control_tapped(PHAudioCallControlsViewController *controller, SEL command,
    NSInteger type, UIView *view)
{
    if (!is_sdx55_audio_control(type)) {
        original_audio_control_tapped(controller, command, type, view);
        return;
    }
    struct umi_voice_route_status route = {0};
    if (read_incall_route(&route) && !route.pending) {
        uint32_t wanted = route.route == UMI_VOICE_ROUTE_SPEAKER ? UMI_VOICE_ROUTE_RECEIVER : UMI_VOICE_ROUTE_SPEAKER;
        int error = sysctlbyname(UMI_VOICE_ROUTE_SYSCTL, NULL, NULL, &wanted, sizeof(wanted));
        if (!error) umi_voice_diagnostic("UMI InCallService: built-in audio route requested\n");
        refresh_audio_control(controller, 12);
    }
}

static void pick_route(id controller, SEL command, id route)
{
    TUCallCenter *center = [NSClassFromString(@"TUCallCenter") sharedInstance];
    struct umi_voice_route_status current = {0};
    size_t size = sizeof(current);
    if (owns_sdx55_call(center.frontmostAudioOrVideoCall) &&
        [route respondsToSelector:@selector(isSpeaker)] && [route respondsToSelector:@selector(isReceiver)] &&
        ([route isSpeaker] || [route isReceiver]) &&
        !sysctlbyname(UMI_VOICE_ROUTE_SYSCTL, &current, &size, NULL, 0) && size == sizeof(current) && current.route) {
        uint32_t wanted = [route isSpeaker] ? UMI_VOICE_ROUTE_SPEAKER : UMI_VOICE_ROUTE_RECEIVER;
        if (sysctlbyname(UMI_VOICE_ROUTE_SYSCTL, NULL, NULL, &wanted, sizeof(wanted))) return;
        umi_voice_diagnostic("UMI InCallService: built-in audio route requested\n");
    }
    original_pick_route(controller, command, route);
}

__attribute__((constructor)) static void start_incall_route(void)
{
    if (strcmp(getprogname(), "InCallService")) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (access("/redwood/bin/callservicesd", X_OK)) return;
        Class cls = NSClassFromString(@"TURouteController");
        SEL selector = sel_registerName("pickRoute:");
        Method method = cls ? class_getInstanceMethod(cls, selector) : NULL;
        if (!method || method_getNumberOfArguments(method) != 3) return;
        original_pick_route = (void *)method_getImplementation(method);
        class_replaceMethod(cls, selector, (IMP)pick_route, method_getTypeEncoding(method));
        umi_voice_diagnostic("UMI InCallService: audio route handler installed\n");

        cls = NSClassFromString(@"TUCallCenter");
        selector = sel_registerName("autoPunchOutBehaviorRequiredForCurrentCalls");
        method = cls ? class_getInstanceMethod(cls, selector) : NULL;
        if (method && method_getNumberOfArguments(method) == 2) {
            original_auto_punch_out = (void *)method_getImplementation(method);
            class_replaceMethod(cls, selector, (IMP)auto_punch_out, method_getTypeEncoding(method));
        }
        selector = sel_registerName("shouldActivateProviderInBackgroundForCall:");
        method = cls ? class_getInstanceMethod(cls, selector) : NULL;
        if (method && method_getNumberOfArguments(method) == 3) {
            original_activate_provider = (void *)method_getImplementation(method);
            class_replaceMethod(cls, selector, (IMP)activate_provider, method_getTypeEncoding(method));
        }

        cls = NSClassFromString(@"PHAudioCallViewController");
        selector = @selector(viewDidAppear:);
        method = cls ? class_getInstanceMethod(cls, selector) : NULL;
        if (method && method_getNumberOfArguments(method) == 3) {
            original_audio_call_view_appeared = (void *)method_getImplementation(method);
            class_replaceMethod(cls, selector, (IMP)audio_call_view_appeared, method_getTypeEncoding(method));
        }

        cls = NSClassFromString(@"ICSApplicationDelegate");
        selector = sel_registerName("shouldPresentDismissedForCall:");
        method = cls ? class_getInstanceMethod(cls, selector) : NULL;
        if (method && method_getNumberOfArguments(method) == 3) {
            original_present_dismissed = (void *)method_getImplementation(method);
            class_replaceMethod(cls, selector, (IMP)present_dismissed, method_getTypeEncoding(method));
        }
        selector = sel_registerName("requestSpringBoardPresentationForCall:dialRequest:tappedPill:");
        method = cls ? class_getInstanceMethod(cls, selector) : NULL;
        if (method && method_getNumberOfArguments(method) == 5) {
            original_request_call_presentation = (void *)method_getImplementation(method);
            class_replaceMethod(cls, selector, (IMP)request_call_presentation, method_getTypeEncoding(method));
        }

        cls = NSClassFromString(@"PHAudioCallControlsViewController");
        SEL enabled = sel_registerName("controlTypeIsEnabled:");
        SEL selected = sel_registerName("controlTypeIsSelected:");
        SEL tapped = sel_registerName("controlTypeTapped:forView:");
        SEL appeared = @selector(viewDidAppear:);
        Method mEnabled = cls ? class_getInstanceMethod(cls, enabled) : NULL;
        Method mSelected = cls ? class_getInstanceMethod(cls, selected) : NULL;
        Method mTapped = cls ? class_getInstanceMethod(cls, tapped) : NULL;
        Method mAppeared = cls ? class_getInstanceMethod(cls, appeared) : NULL;
        if (mEnabled && mSelected && mTapped && mAppeared &&
            method_getNumberOfArguments(mEnabled) == 3 && method_getNumberOfArguments(mSelected) == 3 &&
            method_getNumberOfArguments(mTapped) == 4 && method_getNumberOfArguments(mAppeared) == 3) {
            original_audio_control_enabled = (void *)method_getImplementation(mEnabled);
            original_audio_control_selected = (void *)method_getImplementation(mSelected);
            original_audio_control_tapped = (void *)method_getImplementation(mTapped);
            original_audio_controls_appeared = (void *)method_getImplementation(mAppeared);
            class_replaceMethod(cls, enabled, (IMP)audio_control_enabled, method_getTypeEncoding(mEnabled));
            class_replaceMethod(cls, selected, (IMP)audio_control_selected, method_getTypeEncoding(mSelected));
            class_replaceMethod(cls, tapped, (IMP)audio_control_tapped, method_getTypeEncoding(mTapped));
            class_replaceMethod(cls, appeared, (IMP)audio_controls_appeared, method_getTypeEncoding(mAppeared));
            umi_voice_diagnostic("UMI InCallService: audio controls use the baseband output\n");
        }
    });
}
