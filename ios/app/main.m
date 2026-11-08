// The iPhone app around the game: a full-screen view that the Metal layer draws into (rt_ios_game_view), sound set up the way a game wants it,
// and the game itself started on a thread with a large stack (rt_main), since translated code mirrors the game's calls on the native stack and
// iOS gives the main thread 1 MB. Everything else is the runtime in build/ios/libarkham.a, built by tools/build.py --ios.
//
// Held sideways the game fills the screen; held upright, it takes the top in its own proportions and the touch controls the rest
// (TouchControls.m). On a screen of other proportions than the one the game started on, such as a foldable's outer one, it keeps its own.
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
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

extern UIView *rt_ios_game_view;
extern double rt_ios_screen_w, rt_ios_screen_h, rt_ios_screen_scale;
int rt_main(int argc, char **argv);

static NSString *documents(void) { return NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject; }

// ---------- settings ----------
int rt_ios_hud_at_launch;  // Apple's Metal HUD on for this process from its start (see the end of settings)
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
    // Apple's Metal HUD shows only in a process it was on for from the launch; the layer's own properties then show and hide it (mtl_device.m), and
    // the settings' switch does that (TouchControls.m). So the switch's state, or ARKHAM_METAL_HUD before it was ever touched, decides here whether
    // this process gets it, and Metal's own key in the defaults keeps that for the next launch too.
    NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
    const char *env_hud = getenv("ARKHAM_METAL_HUD");
    rt_ios_hud_at_launch = [d objectForKey:@"metalHUD"] ? [d doubleForKey:@"metalHUD"] > 0 : env_hud && *env_hud == '1';
    [d setBool:rt_ios_hud_at_launch forKey:@"MetalForceHudEnabled"];
    if (rt_ios_hud_at_launch) setenv("MTL_HUD_ENABLED", "1", 1);
    // The first launch of each new build samples every thread through the startup (hitch.c), so that its log shows where the black seconds go
    // without a launch from the Mac. Then again from 90 to 110 s, by then most likely in play, once every 2 s: what the game's own threads do there,
    // for making them faster (the Mac's profile doesn't name the translated functions). A build is known by its executable's time.
    NSDate *built = [NSFileManager.defaultManager attributesOfItemAtPath:NSBundle.mainBundle.executablePath error:nil].fileModificationDate;
    if (built && [d doubleForKey:@"profiledBuild"] != built.timeIntervalSince1970 && !getenv("ARKHAM_TRACE_PROFILE")) {
        [d setDouble:built.timeIntervalSince1970 forKey:@"profiledBuild"];
        setenv("ARKHAM_TRACE_PROFILE", "0.5", 1);
        setenv("ARKHAM_TRACE_PROFILE_FOR", "10", 1);
        setenv("ARKHAM_TRACE_PROFILE_PLAY", "90:20:2", 1);
        fprintf(stderr, "[ios] the first launch of this build: where every thread is, every half second for the first 10 s, and every 2 s from 90 to 110 s\n");
    }
}

// ---------- the log ----------
// The game's stderr goes both where it went (the Mac's console when started from there) and to Documents/Logs/run.txt, for the runs
// nobody is watching.
// In the file each line starts with the seconds since the process started, for seeing where time goes, at start above all.
static int log_fd = -1, console_fd = -1;
static dispatch_semaphore_t log_drained;
static double process_started;  // seconds, on the clock below
static double now_seconds(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + tv.tv_usec / 1e6;
}
static void *log_pump(void *arg) {
    int in = (int)(intptr_t)arg;
    char buf[16384], out[sizeof buf * 2];
    int line_start = 1;
    for (ssize_t n; (n = read(in, buf, sizeof buf)) > 0;) {
        if (console_fd >= 0) write(console_fd, buf, (size_t)n);
        if (log_fd < 0) continue;
        size_t o = 0;
        double t = now_seconds() - process_started;
        for (ssize_t i = 0; i < n; i++) {
            if (line_start) {
                o += (size_t)snprintf(out + o, 16, "%7.2f ", t);
                line_start = 0;
            }
            out[o++] = buf[i];
            if (buf[i] == '\n') line_start = 1;
            if (o > sizeof out - 32) {
                write(log_fd, out, o);
                o = 0;
            }
        }
        write(log_fd, out, o);
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
    // When the process started, from the kernel: what came before main() (loading the app, its 300 MB of code) counts too.
    struct kinfo_proc kp;
    size_t len = sizeof kp;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    process_started = sysctl(mib, 4, &kp, &len, NULL, 0) == 0 ? (double)kp.kp_proc.p_starttime.tv_sec + kp.kp_proc.p_starttime.tv_usec / 1e6 : now_seconds();
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
static UIView *root_view;
// Where the game's picture is, in the view's points: across the top under the camera when the phone is upright, otherwise the middle of the screen,
// always in the proportions of the screen the game started on (what it was told its display is: rt_ios_screen_w and _h).
CGRect rt_ios_game_rect(void) {
    UIView *v = root_view;
    if (!v) return CGRectZero;
    CGSize s = v.bounds.size;
    double aspect = rt_ios_screen_h > 0 ? rt_ios_screen_w / rt_ios_screen_h : 1.42;
    if (s.height > s.width) return CGRectMake(0, v.safeAreaInsets.top, s.width, round(s.width / aspect));
    CGFloat w = s.width, h = round(s.width / aspect);
    if (h > s.height) {
        h = s.height;
        w = round(h * aspect);
    }
    return CGRectMake(round((s.width - w) / 2), round((s.height - h) / 2), w, h);
}

@interface GameViewController : UIViewController
@end
@implementation GameViewController {
    CGSize laid_out;
}
- (void)loadView {
    UIView *v = [UIView new];
    v.backgroundColor = UIColor.blackColor;
    v.multipleTouchEnabled = YES;
    self.view = root_view = v;
}
- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures { return UIRectEdgeAll; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskAllButUpsideDown; }
// The Metal layer (mtl_device.m) is a sublayer of this view's: it goes where the picture goes whenever the view changes, as the phone turns or folds.
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    CGRect g = rt_ios_game_rect();
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    for (CALayer *l in self.view.layer.sublayers)
        if ([l isKindOfClass:CAMetalLayer.class]) l.frame = g;
    [CATransaction commit];
    CGSize s = self.view.bounds.size;
    if (!CGSizeEqualToSize(s, laid_out)) {
        BOOL first = CGSizeEqualToSize(laid_out, CGSizeZero);
        laid_out = s;
        UIEdgeInsets in = self.view.safeAreaInsets;
        fprintf(stderr, "[ios] the view is %.0f x %.0f points (safe area insets top %.0f left %.0f bottom %.0f right %.0f), the game's picture %.0f x %.0f at %.0f,%.0f\n",
                s.width, s.height, in.top, in.left, in.bottom, in.right, g.size.width, g.size.height, g.origin.x, g.origin.y);
        // Turned or folded: once it has settled, a new layer for the game (mtl_device.m says why). A turn and back within the wait makes one.
        if (!first) {
            static unsigned changes;
            unsigned mine = ++changes;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 700 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
                void rt_mtl_new_layer(void);
                if (mine == changes) rt_mtl_new_layer();
            });
        }
    }
}
@end

// ---------- ARKHAM_FPS=1: a line over the game ----------
// The frame rate, the GPU's time a frame, how many frames reach the screen, and how warm the phone is: iOS tells apps a thermal state, not degrees.
static UILabel *overlay;
static int overlay_on = -1;  // the settings' switch (TouchControls.m), or ARKHAM_FPS=1 until they say
static int overlay_hidden;   // everything hidden for a picture (TouchControls.m)
void rt_ios_overlay_enable(int on) {
    overlay_on = on;
    if (!on) dispatch_async(dispatch_get_main_queue(), ^{
            overlay.hidden = YES;
        });
}
void rt_ios_overlay_hidden(int hidden) {
    overlay_hidden = hidden;
    overlay.hidden = hidden || overlay_on <= 0;
}
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
    if (overlay_on < 0) overlay_on = getenv("ARKHAM_FPS") && *getenv("ARKHAM_FPS") == '1';
    if (!overlay_on) return;
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
        overlay.hidden = overlay_hidden;
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
    void rt_ios_touch_controls(UIView *over), rt_ios_apply_settings(void);  // TouchControls.m
    rt_ios_apply_settings();
    rt_ios_touch_controls(vc.view);
    // Where Feral's code keeps its settings and caches, as on a Mac; a new app's container may not have them yet.
    NSString *lib = [NSHomeDirectory() stringByAppendingPathComponent:@"Library"];
    for (NSString *d in @[@"Application Support", @"Preferences", @"Caches"])
        [NSFileManager.defaultManager createDirectoryAtPath:[lib stringByAppendingPathComponent:d] withIntermediateDirectories:YES attributes:nil error:nil];

    NSError *err = nil;
    AVAudioSession *audio = AVAudioSession.sharedInstance;
    if (![audio setCategory:AVAudioSessionCategoryPlayback mode:AVAudioSessionModeDefault options:0 error:&err] || ![audio setActive:YES error:&err])
        fprintf(stderr, "[ios] sound session: %s\n", err.localizedDescription.UTF8String);
    // iOS stops the app's sound while it's away, and after an interruption (a call, another app's sound); nothing starts it again by itself.
    // So on coming back, and when an interruption ends, the session is made active again and the output unit started (shims_audio.c).
    void rt_audio_resume(const char *why);
    void (^resume)(const char *) = ^(const char *why) {
        NSError *e = nil;
        if (![AVAudioSession.sharedInstance setActive:YES error:&e]) fprintf(stderr, "[audio] the sound session again (%s): %s\n", why, e.localizedDescription.UTF8String);
        rt_audio_resume(why);
    };
    [NSNotificationCenter.defaultCenter addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:nil usingBlock:^(NSNotification *n) {
        resume("the app is back");
    }];
    [NSNotificationCenter.defaultCenter addObserverForName:AVAudioSessionInterruptionNotification object:nil queue:nil usingBlock:^(NSNotification *n) {
        NSUInteger type = [n.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue];
        fprintf(stderr, "[audio] interruption %s\n", type == AVAudioSessionInterruptionTypeBegan ? "began" : "ended");
        if (type == AVAudioSessionInterruptionTypeEnded) resume("an interruption ended");
    }];
    [NSNotificationCenter.defaultCenter addObserverForName:UIApplicationDidEnterBackgroundNotification object:nil queue:nil usingBlock:^(NSNotification *n) {
        fprintf(stderr, "[ios] the app went to the background\n");
    }];
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
