// The iPhone app around the game: a full-screen landscape view that the Metal layer draws into (rt_ios_game_view), sound set up the way a
// game wants it, and the game itself started on a thread with a large stack (rt_main), since translated code mirrors the game's calls on
// the native stack and iOS gives the main thread 1 MB. Everything else is the runtime in build/ios/libarkham.a, built by tools/build.py --ios.
//
// What the game needs in the app's Documents folder, put there by tools/ios_install.sh (docs/IOS_PORT.md):
//   Batman Arkham Asylum GOTY/   the Steam folder of the game, as on the Mac
//   Steam/answer.txt, Steam/cloud/   what the Mac's Steam said when the game was installed, and the saves
//   arkham.env (optional)        lines of NAME=value, the same settings as config.local on the Mac, ARKHAM_METAL_HUD=1 among them
// The log of each run is Documents/Logs/run.txt, the previous one run.previous.txt.
#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>
#include <fcntl.h>
#include <os/proc.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

extern UIView *rt_ios_game_view;
extern double rt_ios_screen_w, rt_ios_screen_h, rt_ios_screen_scale;
int rt_main(int argc, char **argv);

static NSString *documents(void) { return NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject; }

// ---------- settings ----------
// The phone's defaults, then Documents/arkham.env over them.
static void settings(void) {
    setenv("ARKHAM_STUB_ALL", "1", 0);   // as tools/run.sh: imports without a shim answer 0, listed in the log as [stub]
    setenv("ARKHAM_LANGUAGE", "ru", 0);
    setenv("ARKHAM_FXAA", "1", 0);
    NSString *env = [NSString stringWithContentsOfFile:[documents() stringByAppendingPathComponent:@"arkham.env"] encoding:NSUTF8StringEncoding error:nil];
    for (NSString *line in [env componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]) {
        NSString *l = [line stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        if (l.length == 0 || [l hasPrefix:@"#"]) continue;
        if ([l hasPrefix:@"export "]) l = [l substringFromIndex:7];
        NSRange eq = [l rangeOfString:@"="];
        if (eq.location == NSNotFound) continue;
        NSString *k = [l substringToIndex:eq.location], *v = [l substringFromIndex:eq.location + 1];
        v = [v stringByTrimmingCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@"\"' "]];
        setenv(k.UTF8String, v.UTF8String, 1);
    }
    const char *hud = getenv("ARKHAM_METAL_HUD");
    if (hud && *hud == '1') setenv("MTL_HUD_ENABLED", "1", 1);  // and the layer's own switch, in mtl_device.m
}

// ---------- the log ----------
// The game's stderr goes both where it went (the Mac's console when started from there) and to Documents/Logs/run.txt, for the runs
// nobody is watching.
static int log_fd = -1, console_fd = -1;
static dispatch_semaphore_t log_drained;
static void *log_pump(void *arg) {
    int in = (int)(intptr_t)arg;
    char buf[16384];
    for (ssize_t n; (n = read(in, buf, sizeof buf)) > 0;) {
        if (console_fd >= 0) write(console_fd, buf, (size_t)n);
        if (log_fd >= 0) write(log_fd, buf, (size_t)n);
    }
    dispatch_semaphore_signal(log_drained);
    return NULL;
}
// At exit the pump gets what's still in the pipe out before the process ends; otherwise the last lines, the ones that say why, are lost.
static void finish_log(void) {
    fflush(stdout);
    fflush(stderr);
    int devnull = open("/dev/null", O_WRONLY);
    dup2(devnull, STDOUT_FILENO);
    dup2(devnull, STDERR_FILENO);  // the pipe's last writer goes, so the pump reads to its end
    dispatch_semaphore_wait(log_drained, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC));
}
static void start_log(void) {
    NSString *dir = [documents() stringByAppendingPathComponent:@"Logs"];
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *run = [dir stringByAppendingPathComponent:@"run.txt"], *prev = [dir stringByAppendingPathComponent:@"run.previous.txt"];
    [NSFileManager.defaultManager removeItemAtPath:prev error:nil];
    [NSFileManager.defaultManager moveItemAtPath:run toPath:prev error:nil];
    log_fd = open(run.fileSystemRepresentation, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    console_fd = dup(STDERR_FILENO);
    int p[2];
    if (pipe(p)) return;
    dup2(p[1], STDERR_FILENO);
    dup2(p[1], STDOUT_FILENO);
    close(p[1]);
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    log_drained = dispatch_semaphore_create(0);
    pthread_t t;
    pthread_create(&t, NULL, log_pump, (void *)(intptr_t)p[0]);
    pthread_detach(t);
    atexit(finish_log);
}

// ---------- the game ----------
static NSString *game_binary(void) {
    return [documents() stringByAppendingPathComponent:@"Batman Arkham Asylum GOTY/Batman Arkham Asylum GOTY.app/Contents/MacOS/Batman Arkham Asylum GOTY"];
}
static void *game_main(void *arg) {
    (void)arg;
    static char *argv[2];
    argv[0] = strdup(game_binary().fileSystemRepresentation);
    int r = rt_main(1, argv);
    fprintf(stderr, "[ios] the game ended with %d\n", r);
    return NULL;
}
static void start_game(void) {
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 256u << 20);  // the Mac's main thread has the same (tools/build.py); logs/probe_*.txt shows the phone allows it
    pthread_attr_set_qos_class_np(&a, QOS_CLASS_USER_INTERACTIVE, 0);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    int e = pthread_create(&t, &a, game_main, NULL);
    if (e) fprintf(stderr, "[ios] could not start the game's thread: %s\n", strerror(e));
}

// ---------- the screen ----------
@interface GameViewController : UIViewController
@end
@implementation GameViewController
- (void)loadView {
    UIView *v = [UIView new];
    v.backgroundColor = UIColor.blackColor;
    v.multipleTouchEnabled = YES;
    self.view = v;
}
- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures { return UIRectEdgeAll; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
// The Metal layer (mtl_device.m) is a sublayer of this view's: it follows the view when the phone turns from one landscape side to the other.
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    for (CALayer *l in self.view.layer.sublayers)
        if ([l isKindOfClass:CAMetalLayer.class]) l.frame = self.view.bounds;
    [CATransaction commit];
}
@end

// ---------- ARKHAM_FPS=1: a line over the game ----------
// The frame rate, the GPU's time a frame, how many frames reach the screen, and how warm the phone is: iOS tells apps a thermal state, not degrees.
static UILabel *overlay;
static NSString *thermal_name(NSProcessInfoThermalState s) {
    switch (s) {
        case NSProcessInfoThermalStateNominal: return @"норма";
        case NSProcessInfoThermalStateFair: return @"тёплый";
        case NSProcessInfoThermalStateSerious: return @"горячий";
        case NSProcessInfoThermalStateCritical: return @"критический";
    }
    return @"?";
}
void rt_ios_overlay(double fps, double gpu_ms, double shown_a_second) {
    static int on = -1;
    if (on < 0) on = getenv("ARKHAM_FPS") && *getenv("ARKHAM_FPS") == '1';
    if (!on) return;
    NSString *text = [NSString stringWithFormat:@"%.0f FPS · GPU %.1f мс · на экране %.0f/с · нагрев: %@", fps, gpu_ms, shown_a_second,
                                                thermal_name(NSProcessInfo.processInfo.thermalState)];
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!overlay) {
            UIView *v = rt_ios_game_view;
            if (!v) return;
            overlay = [UILabel new];
            overlay.font = [UIFont monospacedDigitSystemFontOfSize:12 weight:UIFontWeightMedium];
            overlay.textColor = UIColor.whiteColor;
            overlay.backgroundColor = [UIColor colorWithWhite:0 alpha:0.55];
            overlay.layer.cornerRadius = 6;
            overlay.clipsToBounds = YES;
            overlay.textAlignment = NSTextAlignmentCenter;
            [v addSubview:overlay];
        }
        overlay.text = text;
        [overlay sizeToFit];
        UIEdgeInsets safe = overlay.superview.safeAreaInsets;
        CGRect b = overlay.superview.bounds;
        overlay.frame = CGRectMake((b.size.width - overlay.bounds.size.width - 16) / 2, b.size.height - safe.bottom - overlay.bounds.size.height - 10,
                                   overlay.bounds.size.width + 16, overlay.bounds.size.height + 6);
    });
}

static void show_text(UIViewController *vc, NSString *text) {
    UILabel *l = [[UILabel alloc] initWithFrame:CGRectInset(vc.view.bounds, 40, 30)];
    l.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    l.numberOfLines = 0;
    l.textColor = UIColor.whiteColor;
    l.font = [UIFont systemFontOfSize:15];
    l.text = text;
    [vc.view addSubview:l];
}

@interface GameSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end
@implementation GameSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    UIWindowScene *ws = (UIWindowScene *)scene;
    self.window = [[UIWindow alloc] initWithWindowScene:ws];
    GameViewController *vc = [GameViewController new];
    self.window.rootViewController = vc;
    [self.window makeKeyAndVisible];
    UIApplication.sharedApplication.idleTimerDisabled = YES;

    // The screen as the game will see it: landscape, in points, and its scale (shims_cg.c, ios_appkit.m).
    CGRect b = ws.screen.bounds;
    rt_ios_screen_w = MAX(b.size.width, b.size.height);
    rt_ios_screen_h = MIN(b.size.width, b.size.height);
    rt_ios_screen_scale = ws.screen.nativeScale;
    NSDictionary *fs = [NSFileManager.defaultManager attributesOfFileSystemForPath:documents() error:nil];
    fprintf(stderr, "[ios] free space on the device: %.1f GB, memory the app may use: %.0f MB\n", [fs[NSFileSystemFreeSize] doubleValue] / 1e9,
            (double)os_proc_available_memory() / 1048576.0);
    fprintf(stderr, "[ios] screen %.0f x %.0f points at %.2fx, %.0f x %.0f pixels\n", rt_ios_screen_w, rt_ios_screen_h, rt_ios_screen_scale, rt_ios_screen_w * rt_ios_screen_scale,
            rt_ios_screen_h * rt_ios_screen_scale);

    if (![NSFileManager.defaultManager fileExistsAtPath:game_binary()]) {
        fprintf(stderr, "[ios] no game files: %s is missing\n", game_binary().UTF8String);
        show_text(vc, @"The game's files aren't on this iPhone yet.\n\nOn the Mac, with the iPhone connected: tools/ios_install.sh copies them (about 9 GB) and Steam's answer for the game into this app's Documents folder.");
        return;
    }
    rt_ios_game_view = vc.view;
    // Where Feral's code keeps its settings and caches, as on a Mac; a new app's container may not have them yet.
    NSString *lib = [NSHomeDirectory() stringByAppendingPathComponent:@"Library"];
    for (NSString *d in @[@"Application Support", @"Preferences", @"Caches"])
        [NSFileManager.defaultManager createDirectoryAtPath:[lib stringByAppendingPathComponent:d] withIntermediateDirectories:YES attributes:nil error:nil];

    NSError *err = nil;
    AVAudioSession *audio = AVAudioSession.sharedInstance;
    if (![audio setCategory:AVAudioSessionCategoryPlayback mode:AVAudioSessionModeDefault options:0 error:&err] || ![audio setActive:YES error:&err])
        fprintf(stderr, "[ios] sound session: %s\n", err.localizedDescription.UTF8String);
    // How warm the phone gets, in the log as it changes.
    [NSNotificationCenter.defaultCenter addObserverForName:NSProcessInfoThermalStateDidChangeNotification object:nil queue:nil usingBlock:^(NSNotification *n) {
        fprintf(stderr, "[ios] thermal state: %s\n", thermal_name(NSProcessInfo.processInfo.thermalState).UTF8String);
    }];
    fprintf(stderr, "[ios] thermal state: %s\n", thermal_name(NSProcessInfo.processInfo.thermalState).UTF8String);
    start_game();
}
@end

@interface GameAppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation GameAppDelegate
- (UISceneConfiguration *)application:(UIApplication *)app configurationForConnectingSceneSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    UISceneConfiguration *c = [[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:session.role];
    c.delegateClass = GameSceneDelegate.class;
    return c;
}
@end

int rt_reserve_guest(void);
int main(int argc, char **argv) {
    rt_reserve_guest();  // first, while the address space is still empty (rt.c)
    start_log();
    settings();
    fprintf(stderr, "[ios] Arkham on %s\n", UIDevice.currentDevice.systemVersion.UTF8String);
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(GameAppDelegate.class));
    }
}
