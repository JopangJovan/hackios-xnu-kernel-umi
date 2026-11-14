/* SDX55 controls in iOS 16.5 Settings. */
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/sysctl.h>
#include <sys/umi_sdx55_cell_status.h>
#include <sys/umi_sdx55_voice.h>
#include <unistd.h>

static NSString *localized(NSString *english, NSString *chinese)
{
    return [NSLocale.preferredLanguages.firstObject hasPrefix:@"zh"] ? chinese : english;
}

static void diagnostic(const char *message)
{
    int fd = open("/dev/console", O_WRONLY);
    if (fd < 0) return;
    (void)write(fd, message, strlen(message));
    close(fd);
}

static struct umi_sdx55_radio_snapshot read_radio(void)
{
    struct umi_sdx55_radio_snapshot radio = {0};
    size_t size = sizeof(radio);
    if (sysctlbyname(UMI_SDX55_RADIO_SYSCTL, &radio, &size, NULL, 0) ||
        size != sizeof(radio) || radio.size != size || radio.version != UMI_SDX55_RADIO_VERSION)
        return (struct umi_sdx55_radio_snapshot){.rsrp = INT32_MIN};
    return radio;
}

static NSString *data_state(unsigned state)
{
    switch (state) {
    case UMI_DATA_OFF: return localized(@"Off", @"已关闭");
    case UMI_DATA_CONNECTING: return localized(@"Connecting", @"连接中");
    case UMI_DATA_ON: return localized(@"Connected", @"已连接");
    case UMI_DATA_DISCONNECTING: return localized(@"Disconnecting", @"断开中");
    default: return localized(@"Unavailable", @"不可用");
    }
}

static NSString *voice_service(void)
{
    struct umi_voice_snapshot voice;
    size_t length = sizeof(voice);
    if (sysctlbyname(UMI_VOICE_SYSCTL, &voice, &length, NULL, 0) ||
        length != sizeof(voice) || voice.size != length || voice.version != UMI_VOICE_VERSION ||
        voice.state == UMI_VOICE_UNAVAILABLE)
        return localized(@"Unavailable", @"不可用");
    switch (voice.ims_registration) {
    case 0: return localized(@"Not registered", @"未注册");
    case 1: return localized(@"Registering…", @"注册中…");
    case 2:
        if (voice.ims_voice == 2) return localized(@"Registered (IMS)", @"已注册（IMS）");
        if (voice.ims_voice == 1) return localized(@"Limited service", @"服务受限");
        if (!voice.ims_voice) return localized(@"Voice unavailable", @"语音不可用");
        break;
    }
    return localized(@"Checking…", @"正在查询…");
}

static BOOL showVoiceService;
@interface UMICellularPage : UITableViewController
@end

@implementation UMICellularPage {
    NSTimer *_refreshTimer;
    UISwitch *_dataSwitch;
    NSArray<UITableViewCell *> *_rows;
}

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.title = localized(@"Cellular", @"蜂窝网络");
    _dataSwitch = [[UISwitch alloc] init];
    [_dataSwitch addTarget:self action:@selector(dataChanged:) forControlEvents:UIControlEventValueChanged];
    NSArray<NSString *> *labels = @[localized(@"Cellular Data", @"蜂窝数据"), localized(@"Data Connection", @"数据连接"),
        localized(@"Carrier", @"运营商"), localized(@"Signal", @"信号")];
    if (showVoiceService) labels = [labels arrayByAddingObject:localized(@"Voice Service", @"语音服务")];
    NSMutableArray *rows = [NSMutableArray arrayWithCapacity:labels.count];
    for (NSString *label in labels) {
        UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1 reuseIdentifier:nil];
        cell.textLabel.text = label;
        cell.selectionStyle = UITableViewCellSelectionStyleNone;
        [rows addObject:cell];
    }
    _rows = rows;
    _rows[0].accessoryView = _dataSwitch;
    [self refreshRadio];
}

- (void)viewDidAppear:(BOOL)animated
{
    [super viewDidAppear:animated];
    diagnostic("UMI Cellular Settings: opened SDX55 page\n");
    [self refreshRadio];
    _refreshTimer = [NSTimer scheduledTimerWithTimeInterval:1 target:self selector:@selector(refreshRadio)
        userInfo:nil repeats:YES];
}

- (void)viewDidDisappear:(BOOL)animated
{
    [_refreshTimer invalidate];
    _refreshTimer = nil;
    [super viewDidDisappear:animated];
}

- (void)refreshRadio
{
    struct umi_sdx55_radio_snapshot radio = read_radio();
    NSString *carrier = [NSString stringWithUTF8String:radio.operator_name];
    if (!carrier.length) carrier = [NSString stringWithUTF8String:radio.plmn];
    _dataSwitch.enabled = radio.state != UMI_DATA_UNAVAILABLE;
    [_dataSwitch setOn:radio.enabled != 0 animated:NO];
    _rows[1].detailTextLabel.text = data_state(radio.state);
    _rows[2].detailTextLabel.text = carrier.length ? carrier : @"—";
    _rows[3].detailTextLabel.text = radio.rsrp == INT32_MIN ? @"—" :
        [NSString stringWithFormat:@"%uG · %d dBm", radio.rat, radio.rsrp];
    if (showVoiceService) {
        NSString *status = voice_service();
        if (![_rows[4].detailTextLabel.text isEqualToString:status]) {
            _rows[4].detailTextLabel.text = status;
            char message[112];
            snprintf(message, sizeof(message), "UMI Cellular Settings: voice service status=%s\n", status.UTF8String);
            diagnostic(message);
        }
    }
    for (UITableViewCell *cell in _rows) [cell setNeedsLayout];
}

- (void)dataChanged:(UISwitch *)sender
{
    int value = sender.on;
    int result = sysctlbyname(UMI_SDX55_DATA_SYSCTL, NULL, NULL, &value, sizeof(value));
    int error = result ? errno : 0;
    char message[100];
    snprintf(message, sizeof(message), "UMI Cellular Settings: switch=%d errno=%d\n", value, error);
    diagnostic(message);
    [self refreshRadio];
    if (error) {
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:localized(@"Cellular Data", @"蜂窝数据")
            message:localized(@"The modem could not change the data setting.", @"基带无法更改蜂窝数据设置。")
            preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:localized(@"OK", @"好") style:UIAlertActionStyleDefault handler:nil]];
        [self presentViewController:alert animated:YES completion:nil];
    }
}

- (NSInteger)numberOfSectionsInTableView:(__unused UITableView *)table { return 2; }
- (NSInteger)tableView:(__unused UITableView *)table numberOfRowsInSection:(NSInteger)section
{
    return section == 0 ? 2 : _rows.count - 2;
}
- (UITableViewCell *)tableView:(__unused UITableView *)table cellForRowAtIndexPath:(NSIndexPath *)indexPath
{
    return _rows[2 * indexPath.section + indexPath.row];
}
@end

static void (*original_select)(UIViewController *, SEL, UITableView *, NSIndexPath *);

static void select_settings_row(UIViewController *controller, SEL cmd, UITableView *table, NSIndexPath *path)
{
    UITableViewCell *cell = [table cellForRowAtIndexPath:path];
    NSString *name = cell.textLabel.text ?: cell.accessibilityLabel;
    if (controller.navigationController && ([name isEqualToString:localized(@"Cellular", @"蜂窝网络")] ||
        [name isEqualToString:@"Mobile Data"] || [name isEqualToString:@"Mobile Service"])) {
        [table deselectRowAtIndexPath:path animated:YES];
        [controller.navigationController pushViewController:[[UMICellularPage alloc] initWithStyle:UITableViewStyleInsetGrouped]
            animated:YES];
        return;
    }
    original_select(controller, cmd, table, path);
}

@interface UIViewController (UMICellControlCenter)
- (void)setEnabled:(BOOL)enabled;
- (void)setInoperative:(BOOL)inoperative;
- (void)setSubtitle:(NSString *)subtitle;
- (BOOL)_isCellularDataRestricted;
@end

static void (*cc_appear)(UIViewController *, SEL, BOOL);
static void (*cc_disappear)(UIViewController *, SEL, BOOL);
static char cc_timer;

static void update_control(UIViewController *controller, __unused SEL cmd)
{
    struct umi_sdx55_radio_snapshot radio = read_radio();
    [controller setInoperative:radio.state == UMI_DATA_UNAVAILABLE || [controller _isCellularDataRestricted]];
    [controller setEnabled:radio.enabled != 0];
    [controller setSubtitle:data_state(radio.state)];
}

static void tap_control(UIViewController *controller, __unused SEL cmd, __unused id sender)
{
    struct umi_sdx55_radio_snapshot radio = read_radio();
    if (radio.state == UMI_DATA_UNAVAILABLE || [controller _isCellularDataRestricted]) return;
    int value = !radio.enabled;
    int result = sysctlbyname(UMI_SDX55_DATA_SYSCTL, NULL, NULL, &value, sizeof(value));
    int error = result ? errno : 0;
    char message[100];
    snprintf(message, sizeof(message), "UMI Cellular Control Center: switch=%d errno=%d\n", value, error);
    diagnostic(message);
    update_control(controller, NULL);
}

static void show_control(UIViewController *controller, SEL cmd, BOOL animated)
{
    cc_appear(controller, cmd, animated);
    update_control(controller, NULL);
    if (objc_getAssociatedObject(controller, &cc_timer)) return;
    __weak UIViewController *weak = controller;
    NSTimer *timer = [NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer *tick) {
        UIViewController *visible = weak;
        if (visible) update_control(visible, NULL);
        else [tick invalidate];
    }];
    objc_setAssociatedObject(controller, &cc_timer, timer, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

static void hide_control(UIViewController *controller, SEL cmd, BOOL animated)
{
    [objc_getAssociatedObject(controller, &cc_timer) invalidate];
    objc_setAssociatedObject(controller, &cc_timer, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    cc_disappear(controller, cmd, animated);
}

static void install_control_center(void)
{
    if (!dlopen("/System/Library/ControlCenter/Bundles/ConnectivityModule.bundle/ConnectivityModule",
        RTLD_LAZY | RTLD_LOCAL)) return;
    Class cls = NSClassFromString(@"CCUIConnectivityCellularDataViewController");
    Method update = class_getInstanceMethod(cls, sel_registerName("_updateState"));
    Method tap = class_getInstanceMethod(cls, sel_registerName("buttonTapped:"));
    Method appear = class_getInstanceMethod(cls, @selector(viewWillAppear:));
    Method disappear = class_getInstanceMethod(cls, @selector(viewWillDisappear:));
    if (!update || !tap || !appear || !disappear ||
        !class_getInstanceMethod(cls, @selector(setInoperative:)) ||
        !class_getInstanceMethod(cls, @selector(setEnabled:)) ||
        !class_getInstanceMethod(cls, @selector(setSubtitle:))) return;
    cc_appear = (void *)method_getImplementation(appear);
    cc_disappear = (void *)method_getImplementation(disappear);
    class_replaceMethod(cls, method_getName(update), (IMP)update_control, method_getTypeEncoding(update));
    class_replaceMethod(cls, method_getName(tap), (IMP)tap_control, method_getTypeEncoding(tap));
    class_replaceMethod(cls, method_getName(appear), (IMP)show_control, method_getTypeEncoding(appear));
    class_replaceMethod(cls, method_getName(disappear), (IMP)hide_control, method_getTypeEncoding(disappear));
    diagnostic("UMI Cellular Control Center: installed SDX55 control\n");
}

/* UIKitCore 20F66's publisher copies 0xf28 bytes. The cellular converter
 * reads these primary-line offsets for enable flags, strings, and strength. */
enum { STATUS_BYTES = 0xf28, STATUS_SIGNAL_ENABLED = 4, STATUS_SERVICE_ENABLED = 6,
    STATUS_SIGNAL_DBM = 0x1b0, STATUS_SIGNAL_BARS = 0x1b8, STATUS_SERVICE = 0x1c0,
    STATUS_SERVICE_CROSSFADE = 0x288, STATUS_SERVICE_TYPE = 0x818, STATUS_SERVICE_LENGTH = 100 };
static void (*original_post)(id, SEL, const uint8_t *, int);
static NSData *last_status;
static struct umi_sdx55_radio_snapshot last_radio;

static unsigned signal_bars(int32_t rsrp)
{
    return (rsrp >= -125) + (rsrp >= -115) + (rsrp >= -105) + (rsrp >= -90);
}

static void publish_radio(id server, SEL cmd, const uint8_t *status, int actions,
    struct umi_sdx55_radio_snapshot radio)
{
    if (!radio.rat || radio.rsrp == INT32_MIN) {
        original_post(server, cmd, status, actions);
        return;
    }
    uint8_t updated[STATUS_BYTES];
    memcpy(updated, status, sizeof(updated));
    updated[STATUS_SIGNAL_ENABLED] = 1;
    unsigned bars = signal_bars(radio.rsrp);
    memcpy(updated + STATUS_SIGNAL_DBM, &radio.rsrp, sizeof(radio.rsrp));
    memcpy(updated + STATUS_SIGNAL_BARS, &bars, sizeof(bars));
    if (radio.operator_name[0] || radio.plmn[0]) {
        const char *name = radio.operator_name[0] ? radio.operator_name : radio.plmn;
        updated[STATUS_SERVICE_ENABLED] = 1;
        memset(updated + STATUS_SERVICE_TYPE, 0, sizeof(uint32_t));
        strlcpy((char *)updated + STATUS_SERVICE, name, STATUS_SERVICE_LENGTH);
        strlcpy((char *)updated + STATUS_SERVICE_CROSSFADE, name, STATUS_SERVICE_LENGTH);
    }
    original_post(server, cmd, updated, actions);
}

static void post_status(id server, SEL cmd, const uint8_t *status, int actions)
{
    if (!status || !NSThread.isMainThread) {
        original_post(server, cmd, status, actions);
        return;
    }
    last_status = [NSData dataWithBytes:status length:STATUS_BYTES];
    last_radio = read_radio();
    publish_radio(server, cmd, status, actions, last_radio);
}

static void install_status_bar(void)
{
    Class server = NSClassFromString(@"UIStatusBarServer");
    SEL post = sel_registerName("postStatusBarData:withActions:");
    Method method = class_getClassMethod(server, post);
    if (!method || method_getNumberOfArguments(method) != 4) return;
    char data_type[512];
    method_getArgumentType(method, 2, data_type, sizeof(data_type));
    if (!strstr(data_type, "[46B][64c][64c][256c]iiii[100c][100c]")) return;
    original_post = (void *)method_getImplementation(method);
    class_replaceMethod(object_getClass(server), post, (IMP)post_status, method_getTypeEncoding(method));
    [NSTimer scheduledTimerWithTimeInterval:2 repeats:YES block:^(__unused NSTimer *timer) {
        if (!last_status) return;
        struct umi_sdx55_radio_snapshot next = read_radio();
        if (memcmp(&next, &last_radio, sizeof(next)) == 0) return;
        last_radio = next;
        publish_radio(server, post, last_status.bytes, 0, next);
    }];
    diagnostic("UMI Cellular Status Bar: installed SDX55 publisher\n");
}

__attribute__((constructor)) static void start_cellular_settings(void)
{
    bool springboard = !strcmp(getprogname(), "SpringBoard");
    if (!springboard && strcmp(getprogname(), "Preferences")) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        char args[1024] = {0};
        size_t size = sizeof(args);
        if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) ||
            size > sizeof(args) || !memchr(args, 0, size)) return;
        NSArray *words = [[NSString stringWithUTF8String:args]
            componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        if (![words containsObject:@"sdx55_cell=1"] || ![words containsObject:@"sdx55_data_call=1"]) return;
        if (springboard) {
            install_status_bar();
            install_control_center();
            return;
        }
        showVoiceService = [words containsObject:@"sdx55_voice=1"];
        Class root = NSClassFromString(@"PSUIPrefsListController");
        SEL select = @selector(tableView:didSelectRowAtIndexPath:);
        Method method = class_getInstanceMethod(root, select);
        if (!method || method_getNumberOfArguments(method) != 4) return;
        original_select = (void *)method_getImplementation(method);
        class_replaceMethod(root, select, (IMP)select_settings_row, method_getTypeEncoding(method));
        diagnostic("UMI Cellular Settings: installed SDX55 selection handler\n");
    });
}
