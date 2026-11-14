/* iOS 16.5 Phone keypad entry for the UMI SDX55 outbound call test. */
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/sysctl.h>
#include <sys/umi_sdx55_voice.h>
#include <unistd.h>

static NSString *voice_text(NSString *english, NSString *chinese)
{
    return [NSLocale.preferredLanguages.firstObject hasPrefix:@"zh"] ? chinese : english;
}

static NSString *test_number(NSString *text);

static void voice_diagnostic(const char *message)
{
    int fd = open("/dev/console", O_WRONLY);
    if (fd < 0) return;
    (void)write(fd, message, strlen(message));
    close(fd);
}

static int voice_submit(unsigned action, NSString *number)
{
    struct umi_voice_request request = {.size=sizeof(request), .version=UMI_VOICE_VERSION, .action=action};
    if (number && ![number getCString:request.number maxLength:sizeof(request.number) encoding:NSASCIIStringEncoding]) return EINVAL;
    if (action == UMI_VOICE_DIAL && !umi_voice_number_length(request.number)) return EINVAL;
    int error = sysctlbyname(UMI_VOICE_SYSCTL, NULL, NULL, &request, sizeof(request)) ? errno : 0;
    char message[80];
    snprintf(message, sizeof(message), "UMI Phone: voice request=%u errno=%d\n", action, error);
    voice_diagnostic(message);
    return error;
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
@end

@implementation UMIVoiceCallPage {
    UILabel *_stateLabel, *_detailsLabel;
    UITextField *_numberInput;
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
    _numberInput = [[UITextField alloc] init];
    _numberInput.text = self.number;
    _numberInput.placeholder = voice_text(@"Phone number or 10010", @"测试号码或 10010");
    _numberInput.font = [UIFont preferredFontForTextStyle:UIFontTextStyleTitle2];
    _numberInput.keyboardType = UIKeyboardTypePhonePad;
    _numberInput.textAlignment = NSTextAlignmentCenter;
    _numberInput.borderStyle = UITextBorderStyleRoundedRect;
    _numberInput.enabled = !self.number;
    _stateLabel = [[UILabel alloc] init];
    _stateLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
    _detailsLabel = [[UILabel alloc] init];
    _detailsLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    _detailsLabel.textColor = UIColor.secondaryLabelColor;
    for (UILabel *label in @[_stateLabel, _detailsLabel]) {
        label.textAlignment = NSTextAlignmentCenter;
        label.numberOfLines = 0;
        label.adjustsFontForContentSizeCategory = YES;
    }
    _action = [UIButton buttonWithType:UIButtonTypeSystem];
    [_action addTarget:self action:@selector(endOrDismiss) forControlEvents:UIControlEventTouchUpInside];
    [_action setTitle:voice_text(@"Call", @"拨打") forState:UIControlStateNormal];
    _speaker = [UIButton buttonWithType:UIButtonTypeSystem];
    _speaker.hidden = YES;
    [_speaker addTarget:self action:@selector(switchAudioRoute) forControlEvents:UIControlEventTouchUpInside];
    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[_numberInput, _stateLabel, _detailsLabel, _speaker, _action]];
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
    _numberInput.enabled = NO;
    [_numberInput resignFirstResponder];
    _numberInput.text = self.number;
    _requestError = voice_submit(UMI_VOICE_DIAL, self.number);
    [self refreshCall];
}

- (void)viewDidAppear:(BOOL)animated
{
    [super viewDidAppear:animated];
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
    size_t length = sizeof(status);
    int readError = sysctlbyname(UMI_VOICE_SYSCTL, &status, &length, NULL, 0) ? errno : 0;
    if (!readError && (length != sizeof(status) || status.size != sizeof(status) || status.version != UMI_VOICE_VERSION)) readError = EPROTO;
    if (!readError && !_requestError)
        _callLive = (status.state >= UMI_VOICE_QUEUED && status.state <= UMI_VOICE_ENDING) || status.state == UMI_VOICE_UNCERTAIN;
    self.modalInPresentation = _callLive;
    _stateLabel.text = _requestError ? voice_text(@"Dial request unavailable", @"拨号请求未受理") :
        readError ? voice_text(@"Modem status unavailable", @"无法读取基带状态") : voice_state(&status);
    unsigned error = _requestError ? (unsigned)_requestError : readError ? (unsigned)readError : status.error;
    _detailsLabel.text = error || status.qmi_error || status.end_reason ?
        [NSString stringWithFormat:@"errno %u · QMI %u · cause %u", error, status.qmi_error, status.end_reason] : nil;
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
        _speaker.selected ? voice_text(@"Speaker On", @"扬声器已开") : voice_text(@"Speaker Off", @"扬声器已关")
        forState:UIControlStateNormal];
    _speaker.accessibilityLabel = voice_text(@"Speaker", @"扬声器");
    _speaker.accessibilityValue = _speaker.selected ? voice_text(@"On", @"打开") : voice_text(@"Off", @"关闭");
    if (audioReady && audio.error)
        _detailsLabel.text = voice_text(@"Could not switch audio output.", @"无法切换音频输出。");
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
    if (error) _detailsLabel.text = voice_text(@"Could not switch audio output.", @"无法切换音频输出。");
}

- (void)endOrDismiss
{
    if (!_submitted) {
        self.number = test_number(_numberInput.text);
        if (self.number) [self submitCall];
        else _stateLabel.text = voice_text(@"Enter a test number.", @"请输入有效的测试号码。");
        return;
    }
    if (!_callLive) { [self dismissViewControllerAnimated:YES completion:nil]; return; }
    int error = voice_submit(UMI_VOICE_END, nil);
    if (!error) { [self refreshCall]; return; }
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:voice_text(@"End Call", @"挂断")
        message:[NSString stringWithFormat:@"errno %d", error] preferredStyle:UIAlertControllerStyleAlert];
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

static void (*original_appear)(UIViewController *, SEL, BOOL);
static Class keypad_class;
static char voice_button_key;

static NSString *test_number(NSString *text)
{
    if (![text isKindOfClass:NSString.class]) return nil;
    NSCharacterSet *formatting = [NSCharacterSet characterSetWithCharactersInString:
        @" ()-\u00a0\u2007\u202f\u200e\u200f\u202a\u202b\u202c\u202d\u202e\u2066\u2067\u2068\u2069"];
    NSString *number = [[text componentsSeparatedByCharactersInSet:formatting] componentsJoinedByString:@""];
    char wire[UMI_VOICE_NUMBER_SIZE] = {0};
    return [number getCString:wire maxLength:sizeof(wire) encoding:NSASCIIStringEncoding] &&
        umi_voice_number_length(wire) ? number : nil;
}

static void keypad_call(UIViewController *controller, __unused SEL cmd, __unused id sender)
{
    voice_diagnostic("UMI Phone: voice keypad tapped\n");
    if (controller.presentedViewController) return;
    UIView *lcd = [[controller dialerView] lcdView];
    NSString *number = [lcd respondsToSelector:@selector(text)] ? test_number([lcd text]) : nil;
    if (!number) voice_diagnostic("UMI Phone: voice keypad needs number input\n");
    UMIVoiceCallPage *page = [[UMIVoiceCallPage alloc] init];
    page.number = number;
    page.modalInPresentation = number != nil;
    [controller presentViewController:page animated:YES completion:nil];
}

static void keypad_appear(UIViewController *controller, SEL cmd, BOOL animated)
{
    original_appear(controller, cmd, animated);
    if (objc_getAssociatedObject(controller, &voice_button_key)) return;
    UIControl *native = [[controller dialerView] callButton];
    UIView *container = controller.view;
    if (!native.superview || ![native isDescendantOfView:container]) return;
    UIButtonConfiguration *appearance = UIButtonConfiguration.filledButtonConfiguration;
    appearance.baseBackgroundColor = UIColor.systemGreenColor;
    appearance.baseForegroundColor = UIColor.whiteColor;
    appearance.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
    appearance.title = voice_text(@"Cellular Call", @"蜂窝拨号");
    UIButton *button = [UIButton buttonWithConfiguration:appearance primaryAction:nil];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    [button addTarget:controller action:sel_registerName("umi_sdx55PlaceCall:") forControlEvents:UIControlEventTouchUpInside];
    [container addSubview:button];
    [NSLayoutConstraint activateConstraints:@[
        [button.trailingAnchor constraintEqualToAnchor:container.safeAreaLayoutGuide.trailingAnchor constant:-12],
        [button.centerYAnchor constraintEqualToAnchor:native.centerYAnchor],
        [button.heightAnchor constraintGreaterThanOrEqualToConstant:44],
    ]];
    objc_setAssociatedObject(controller, &voice_button_key, button, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    voice_diagnostic("UMI Phone: voice keypad button installed\n");
}

__attribute__((constructor)) static void install_voice_keypad(void)
{
    if (strcmp(getprogname(), "MobilePhone")) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        char args[1024];
        size_t length = sizeof(args);
        if (sysctlbyname("kern.bootargs", args, &length, NULL, 0) || length > sizeof(args) || !memchr(args, 0, length)) return;
        NSArray *words = [[NSString stringWithUTF8String:args]
            componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        if (![words containsObject:@"sdx55_voice=1"]) return;
        keypad_class = NSClassFromString(@"MPKeypadViewController");
        Class dialer = NSClassFromString(@"DialerController");
        Method appear = class_getInstanceMethod(keypad_class, @selector(viewWillAppear:));
        if (!keypad_class || !class_getInstanceMethod(dialer, @selector(dialerView)) || !appear ||
            method_getNumberOfArguments(appear) != 3 ||
            !class_addMethod(keypad_class, sel_registerName("umi_sdx55PlaceCall:"), (IMP)keypad_call, "v@:@")) {
            voice_diagnostic("UMI Phone: voice keypad selectors unavailable\n");
            return;
        }
        original_appear = (void *)method_getImplementation(appear);
        method_setImplementation(appear, (IMP)keypad_appear);
        voice_diagnostic("UMI Phone: voice keypad active\n");
    });
}
