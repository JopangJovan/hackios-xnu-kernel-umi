/* Route iOS 16.5 Phone calls from Keypad and Recents to SDX55. */
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/sysctl.h>
#include <sys/umi_sdx55_voice.h>
#include <unistd.h>
#include "umi_voice_system.h"

static NSString *voice_text(NSString *english, NSString *chinese)
{
    return [NSLocale.preferredLanguages.firstObject hasPrefix:@"zh"] ? chinese : english;
}

void umi_voice_diagnostic(const char *message)
{
    int fd = open("/dev/console", O_WRONLY);
    if (fd < 0) return;
    (void)write(fd, message, strlen(message));
    close(fd);
}

int umi_voice_submit(unsigned action, NSString *number)
{
    struct umi_voice_request request = {.size=sizeof(request), .version=UMI_VOICE_VERSION, .action=action};
    if (number && ![number getCString:request.number maxLength:sizeof(request.number) encoding:NSASCIIStringEncoding]) return EINVAL;
    if (action == UMI_VOICE_DIAL && !umi_voice_number_length(request.number)) return EINVAL;
    int error = sysctlbyname(UMI_VOICE_SYSCTL, NULL, NULL, &request, sizeof(request)) ? errno : 0;
    char message[80];
    snprintf(message, sizeof(message), "UMI Phone: voice request=%u errno=%d\n", action, error);
    umi_voice_diagnostic(message);
    return error;
}

int umi_voice_read_status(struct umi_voice_snapshot *status)
{
    size_t length = sizeof(*status);
    if (sysctlbyname(UMI_VOICE_SYSCTL, status, &length, NULL, 0)) return errno;
    return length == sizeof(*status) && status->size == length && status->version == UMI_VOICE_VERSION ? 0 : EPROTO;
}

static NSString *voice_state(const struct umi_voice_snapshot *status)
{
    switch (status->state) {
    case UMI_VOICE_QUEUED: case UMI_VOICE_DIALING: return voice_text(@"Dialing…", @"正在拨号…");
    case UMI_VOICE_RINGING: return voice_text(@"Ringing…", @"对方振铃中…");
    case UMI_VOICE_ACTIVE: return voice_text(@"Connected", @"已接通");
    case UMI_VOICE_ENDING: return voice_text(@"Ending…", @"正在挂断…");
    case UMI_VOICE_ENDED: return voice_text(@"Call ended", @"通话已结束");
    case UMI_VOICE_UNCERTAIN: return voice_text(@"Waiting for modem confirmation", @"等待基带确认通话状态");
    case UMI_VOICE_FAILED:
        return status->error == ENETUNREACH ? voice_text(@"Carrier voice service unavailable", @"运营商语音服务未就绪") :
            voice_text(@"Call failed", @"拨号失败");
    default: return voice_text(@"Modem not ready", @"基带尚未就绪");
    }
}

@interface UMIVoiceCallPage : UIViewController
@property(nonatomic, copy) NSString *number;
@property(nonatomic) BOOL alreadyDialed;
@end

@implementation UMIVoiceCallPage {
    UILabel *_stateLabel;
    UIButton *_action, *_speaker;
    NSTimer *_timer;
    BOOL _submitted, _callLive;
    unsigned _audioRoute;
    int _requestError;
}

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;
    self.title = voice_text(@"Cellular Call", @"蜂窝通话");
    UILabel *number = [[UILabel alloc] init];
    number.text = self.number;
    number.font = [UIFont preferredFontForTextStyle:UIFontTextStyleTitle1];
    _stateLabel = [[UILabel alloc] init];
    _stateLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
    _stateLabel.textColor = UIColor.secondaryLabelColor;
    for (UILabel *label in @[number, _stateLabel]) {
        label.textAlignment = NSTextAlignmentCenter;
        label.numberOfLines = 0;
        label.adjustsFontForContentSizeCategory = YES;
    }
    _action = [UIButton buttonWithType:UIButtonTypeSystem];
    [_action addTarget:self action:@selector(endOrDismiss) forControlEvents:UIControlEventTouchUpInside];
    [_action setTitle:voice_text(@"End Call", @"挂断") forState:UIControlStateNormal];
    _speaker = [UIButton buttonWithType:UIButtonTypeSystem];
    _speaker.hidden = YES;
    [_speaker addTarget:self action:@selector(switchAudioRoute) forControlEvents:UIControlEventTouchUpInside];
    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[number, _stateLabel, _speaker, _action]];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 18;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:stack];
    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:24],
        [stack.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-24],
        [stack.topAnchor constraintEqualToAnchor:safe.topAnchor constant:48],
        [_action.heightAnchor constraintGreaterThanOrEqualToConstant:48],
    ]];
}

- (void)submitCall
{
    _submitted = YES;
    _requestError = umi_voice_submit(UMI_VOICE_DIAL, self.number);
    [self refreshCall];
}

- (void)viewDidAppear:(BOOL)animated
{
    [super viewDidAppear:animated];
    if (self.alreadyDialed) _submitted = YES;
    if (!_submitted && self.number) [self submitCall];
    [self refreshCall];
    __weak UMIVoiceCallPage *weak = self;
    _timer = [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *timer) {
        UMIVoiceCallPage *page = weak;
        if (page) [page refreshCall];
        else [timer invalidate];
    }];
}

- (void)viewDidDisappear:(BOOL)animated
{
    [_timer invalidate];
    _timer = nil;
    [super viewDidDisappear:animated];
}

- (void)refreshCall
{
    if (!_submitted) return;
    struct umi_voice_snapshot status = {0};
    int readError = umi_voice_read_status(&status);
    if (!readError && !_requestError)
        _callLive = (status.state >= UMI_VOICE_QUEUED && status.state <= UMI_VOICE_ENDING) || status.state == UMI_VOICE_UNCERTAIN;
    self.modalInPresentation = _callLive;
    _stateLabel.text = _requestError ? voice_text(@"Dial request unavailable", @"拨号请求未受理") :
        readError ? voice_text(@"Modem status unavailable", @"无法读取基带状态") : voice_state(&status);
    struct umi_voice_route_status audio = {0};
    size_t audioLength = sizeof(audio);
    BOOL audioReady = !readError && status.state == UMI_VOICE_ACTIVE &&
        !sysctlbyname(UMI_VOICE_ROUTE_SYSCTL, &audio, &audioLength, NULL, 0) &&
        audioLength == sizeof(audio) && (audio.route == UMI_VOICE_ROUTE_RECEIVER || audio.route == UMI_VOICE_ROUTE_SPEAKER);
    _audioRoute = audioReady ? audio.route : UMI_VOICE_ROUTE_NONE;
    _speaker.hidden = !audioReady;
    _speaker.enabled = audioReady && !audio.pending;
    _speaker.selected = audio.route == UMI_VOICE_ROUTE_SPEAKER;
    [_speaker setTitle:audio.pending ? voice_text(@"Switching audio…", @"正在切换音频…") :
        voice_text(@"Speaker", @"扬声器") forState:UIControlStateNormal];
    _speaker.tintColor = _speaker.selected ? UIColor.systemBlueColor : UIColor.labelColor;
    _speaker.accessibilityLabel = voice_text(@"Speaker", @"扬声器");
    _speaker.accessibilityValue = _speaker.selected ? voice_text(@"On", @"打开") : voice_text(@"Off", @"关闭");
    if (audioReady && audio.error)
        _stateLabel.text = voice_text(@"Could not switch audio output.", @"无法切换音频输出。");
    [_action setTitle:_callLive ? voice_text(@"End Call", @"挂断") : voice_text(@"Done", @"完成") forState:UIControlStateNormal];
    _action.enabled = status.state != UMI_VOICE_ENDING;
    _action.tintColor = _callLive ? UIColor.systemRedColor : UIColor.systemBlueColor;
}

- (void)switchAudioRoute
{
    if (!_speaker.enabled || _audioRoute == UMI_VOICE_ROUTE_NONE) return;
    uint32_t route = _audioRoute == UMI_VOICE_ROUTE_SPEAKER ? UMI_VOICE_ROUTE_RECEIVER : UMI_VOICE_ROUTE_SPEAKER;
    int error = sysctlbyname(UMI_VOICE_ROUTE_SYSCTL, NULL, NULL, &route, sizeof(route)) ? errno : 0;
    [self refreshCall];
    if (error) _stateLabel.text = voice_text(@"Could not switch audio output.", @"无法切换音频输出。");
}

- (void)endOrDismiss
{
    if (!_submitted) return;
    if (!_callLive) { [self dismissViewControllerAnimated:YES completion:nil]; return; }
    int error = umi_voice_submit(UMI_VOICE_END, nil);
    if (!error) { [self refreshCall]; return; }
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:
        voice_text(@"Couldn’t end the call", @"无法挂断通话") message:nil preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:voice_text(@"OK", @"好") style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}
@end

@interface UIViewController (UMIPhoneDialer)
- (id)dialerView;
@end
@interface UIView (UMIPhoneLCD)
- (UIView *)lcdView;
- (UIControl *)callButton;
- (NSString *)text;
@end

@interface CHRecentCall : NSObject
- (NSString *)serviceProvider;
- (NSInteger)mediaType;
- (NSInteger)handleType;
- (NSSet *)validRemoteParticipantHandles;
- (NSString *)callerId;
@end

static void (*original_dial)(UIViewController *, SEL, UIControl *);
static void (*original_update)(UIViewController *, SEL, id);
static void (*original_redial)(id, SEL, CHRecentCall *);

/* CallHistory's values for a voice call and a phone-number handle in 20F66. */
static const NSInteger history_audio = 1;
static const NSInteger history_phone_number = 2;

static BOOL phone_has_visual_voicemail(__unused id phone, __unused SEL command)
{
    return NO;
}

static NSString *dialable_number(NSString *text)
{
    if (![text isKindOfClass:NSString.class]) return nil;
    NSCharacterSet *formatting = [NSCharacterSet characterSetWithCharactersInString:
        @" ()-\u00a0\u2007\u202f\u200e\u200f\u202a\u202b\u202c\u202d\u202e\u2066\u2067\u2068\u2069"];
    NSString *number = [[text componentsSeparatedByCharactersInSet:formatting] componentsJoinedByString:@""];
    char wire[UMI_VOICE_NUMBER_SIZE] = {0};
    return [number getCString:wire maxLength:sizeof(wire) encoding:NSASCIIStringEncoding] &&
        umi_voice_number_length(wire) ? number : nil;
}

static void start_phone_call(UIViewController *controller, NSString *number)
{
    if (controller.presentedViewController) return;
    __weak UIViewController *weakController = controller;
    void (^fallback)(BOOL) = ^(BOOL alreadyDialed) {
        UIViewController *presenter = weakController;
        if (!presenter.viewIfLoaded.window || presenter.presentedViewController) return;
        UMIVoiceCallPage *page = [[UMIVoiceCallPage alloc] init];
        page.number = number;
        page.alreadyDialed = alreadyDialed;
        page.modalInPresentation = YES;
        [presenter presentViewController:page animated:YES completion:nil];
    };
    if (!umi_voice_start_system_call(number, fallback)) fallback(NO);
}

static void keypad_dial(UIViewController *controller, SEL cmd, UIControl *sender)
{
    id dialer = [controller dialerView];
    if (!sender || sender != [dialer callButton] || !controller.viewIfLoaded.window) {
        original_dial(controller, cmd, sender);
        return;
    }
    UIView *lcd = [dialer lcdView];
    NSString *text = [lcd respondsToSelector:@selector(text)] ? [lcd text] : nil;
    NSString *number = dialable_number(text);
    if (!number) {
        original_dial(controller, cmd, sender);
        return;
    }
    umi_voice_diagnostic("UMI Phone: native call button tapped\n");
    start_phone_call(controller, number);
}

static UIViewController *recents_presenter(void)
{
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:UIWindowScene.class] ||
            scene.activationState != UISceneActivationStateForegroundActive) continue;
        UIWindow *window = ((UIWindowScene *)scene).keyWindow;
        if (window && !window.hidden && window.rootViewController.viewIfLoaded.window)
            return window.rootViewController;
    }
    return nil;
}

static void redial_recent_call(id controller, SEL command, CHRecentCall *call)
{
    if (NSThread.isMainThread &&
        [call isKindOfClass:NSClassFromString(@"CHRecentCall")] &&
        call.mediaType == history_audio && call.handleType == history_phone_number &&
        call.validRemoteParticipantHandles.count <= 1) {
        NSString *provider = call.serviceProvider;
        if ([provider isEqualToString:@"com.apple.mobilephone"] ||
            [provider isEqualToString:@"com.apple.Telephony"]) {
            NSString *number = dialable_number(call.callerId);
            UIViewController *presenter = number ? recents_presenter() : nil;
            if (presenter) {
                umi_voice_diagnostic("UMI Phone: recent cellular call selected\n");
                start_phone_call(presenter, number);
                return;
            }
        }
    }
    original_redial(controller, command, call);
}

static void keypad_update(UIViewController *controller, SEL cmd, id event)
{
    original_update(controller, cmd, event);
    id dialer = [controller dialerView];
    UIView *lcd = [dialer lcdView];
    if ([lcd respondsToSelector:@selector(text)] && dialable_number([lcd text])) {
        UIControl *button = [dialer callButton];
        button.enabled = YES;
    }
}

static BOOL voice_keypad_enabled(void)
{
    char args[1024];
    size_t length = sizeof(args);
    if (sysctlbyname("kern.bootargs", args, &length, NULL, 0) || length > sizeof(args) ||
        !memchr(args, 0, length)) return NO;
    const char *word = args;
    const char *spaces = " \t\r\n\v\f";
    while (*word) {
        word += strspn(word, spaces);
        size_t size = strcspn(word, spaces);
        if (size == sizeof("sdx55_voice=1") - 1 && !memcmp(word, "sdx55_voice=1", size)) return YES;
        word += size;
    }
    return NO;
}

__attribute__((constructor)) static void install_voice_keypad(void)
{
    if (strcmp(getprogname(), "MobilePhone") || !voice_keypad_enabled()) return;
    if (access("/redwood/bin/callservicesd", X_OK) == 0) {
        /* Install before UIApplicationMain can ask Visual Voicemail to query
         * CommCenter while constructing the first tab bar. */
        Class phone = objc_lookUpClass("MobilePhoneApplication");
        SEL showsVoicemail = sel_registerName("showsPhoneVoicemail");
        Method method = phone ? class_getInstanceMethod(phone, showsVoicemail) : NULL;
        if (method && method_getNumberOfArguments(method) == 2) {
            class_replaceMethod(phone, showsVoicemail, (IMP)phone_has_visual_voicemail,
                method_getTypeEncoding(method));
            umi_voice_diagnostic("UMI Phone: visual voicemail unavailable on SDX55\n");
        }
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        if (access("/redwood/bin/callservicesd", X_OK) == 0) {
            Class recents = NSClassFromString(@"PHRecentsController");
            SEL redial = sel_registerName("performDialRequestForRecentCall:");
            Method method = recents ? class_getInstanceMethod(recents, redial) : NULL;
            if (method && method_getNumberOfArguments(method) == 3) {
                original_redial = (void *)method_getImplementation(method);
                class_replaceMethod(recents, redial, (IMP)redial_recent_call, method_getTypeEncoding(method));
                umi_voice_diagnostic("UMI Phone: Recents callback installed\n");
            }
        }
        Class dialer = NSClassFromString(@"DialerController");
        SEL call = sel_registerName("_callButtonPressed:");
        SEL update = sel_registerName("_updateCallButtonEnabledState:");
        Method callMethod = dialer ? class_getInstanceMethod(dialer, call) : NULL;
        Method updateMethod = dialer ? class_getInstanceMethod(dialer, update) : NULL;
        if (!callMethod || !updateMethod || method_getNumberOfArguments(callMethod) != 3 ||
            method_getNumberOfArguments(updateMethod) != 3) {
            umi_voice_diagnostic("UMI Phone: native call button selectors unavailable\n");
            return;
        }
        original_dial = (void *)method_getImplementation(callMethod);
        original_update = (void *)method_getImplementation(updateMethod);
        class_replaceMethod(dialer, call, (IMP)keypad_dial, method_getTypeEncoding(callMethod));
        class_replaceMethod(dialer, update, (IMP)keypad_update, method_getTypeEncoding(updateMethod));
        umi_voice_diagnostic("UMI Phone: native call button installed\n");
    });
}
