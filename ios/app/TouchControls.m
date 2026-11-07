// Touch controls and the phone's settings, after Feral's Tomb Raider for iOS.
//
// In play: a stick wherever the left thumb lands (pushed past its edge it also holds the run button), swipes on the right that turn the camera, and
// the pad's buttons where the thumbs rest: the main four in an arc under the right thumb, detective mode and aiming above the left one, and the
// gadgets as a wheel, which opens while held and chooses the side the thumb slides to (let go without sliding, it stays open for a tap on a side).
// The buttons draw as a PlayStation pad, an Xbox pad, or as icons of what each does in the game (from its bindings in DefaultInput.ini). What they do
// goes to the runtime as a pad of its own (rt_touch_pad in shims_hid.m), shown to the game as the pad drawn so that its prompts match. The camera is
// that pad's right stick, since the game ignores the mouse while a pad moves: its push is the swipe's speed, so the camera turns by how far the thumb
// goes, the same however fast (the game reads that stick ten times faster than it was made to, shims_libc.c, so that it can turn far enough).
//
// Outside play, wherever the game lets the cursor go while its frames go on (menus, the map, cutscenes), the pad hides but for a button that backs
// out, the pad's ○ or B. A tap there is a click where it lands, after which the cursor goes off into a corner, and a drag is the left stick, the other
// way from the finger, so that the map moves with it.
//
// Held upright, the phone shows the game across the top and the controls in the space below it, with a layout of their own. A moved button keeps its
// place in proportion to the space for the controls and its size in proportion to that space's height, so a layout made on a foldable's outer screen
// comes back in the same places on the inner one, larger, and the other way round. The controls hide while a real controller is connected.
//
// The settings, behind the gear at the top right: the frame rate limit, the controls' style, size, opacity and camera speed, the run push, hiding
// outside play, the frame rate line and Apple's Metal HUD, and the layout: moving, resizing, hiding and resetting each button. They're kept in the
// app's own defaults. ARKHAM_TOUCH=0 turns the controls off.
#import <GameController/GameController.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>

void rt_touch_pad(float lx, float ly, float rx, float ry, float lt, float rt, unsigned buttons);
void rt_touch_pad_on(int on);
void rt_touch_pad_playstation(int ps);
int rt_controller_connected(void);
int rt_game_holds_cursor(void);
unsigned rt_frames_presented(void);
void rt_ios_click(double share_x, double share_y);
void rt_ios_park_cursor(void);
void rt_set_fps_cap(double n);
void rt_set_metal_hud(int on);
void rt_ios_overlay_enable(int on);  // main.m: the frame rate line
CGRect rt_ios_game_rect(void);       // main.m: where the game's picture is

// The runtime's numbers for the controls (the enum in shims_hid.m), and the wheel's own.
enum { C_LT = 4, C_RT = 5, B_SOUTH = 7, B_EAST, B_WEST, B_NORTH, B_LB, B_RB, B_START, B_BACK, B_L3, B_R3, B_HOME, D_UP, D_DOWN, D_LEFT, D_RIGHT, C_WHEEL = 100 };
enum { STYLE_PS, STYLE_XBOX, STYLE_ICONS };
enum { SH_NONE, SH_CROSS, SH_CIRCLE, SH_SQUARE, SH_TRIANGLE };
enum { SIDE_UP, SIDE_RIGHT, SIDE_DOWN, SIDE_LEFT };

// ---------- settings ----------
static NSUserDefaults *prefs(void) { return NSUserDefaults.standardUserDefaults; }
static double pref_double(NSString *key, double fallback) {
    id v = [prefs() objectForKey:key];
    return v ? [v doubleValue] : fallback;
}
static double env_double(const char *name, double fallback) {
    const char *v = getenv(name);
    return v && *v ? atof(v) : fallback;
}
// First start: what arkham.env says, if anything; after that, what was chosen here.
static double setting(NSString *key, const char *env, double fallback) { return pref_double(key, env_double(env, fallback)); }
// Written through at once: a game that's stopped from the Mac, or by iOS, gives the defaults no time to be saved later.
static void set_setting(NSString *key, double v) {
    [prefs() setDouble:v forKey:key];
    [prefs() synchronize];
}

// ---------- the buttons ----------
// A default place: a point of the space for the controls (ax, ay: 0 for its left or top, 1 for its right or bottom), and the button's offset from it
// and radius, in points as on a space REF_H high. They're scaled to the actual space's height, and by the size setting.
typedef struct {
    float ax, ay, dx, dy, r;
} Place;
#define REF_H 493.0f  // the unfolded phone's space held sideways, where these places were made (the scale of 1.3 they had before)
typedef struct {
    const char *ident;
    int control;
    Place land, port;  // held sideways, and upright, where the space is below the game's picture
    const char *ps, *xbox, *icon, *hint;
    int shape, ps_color, xbox_color;
} Def;
#define BR 1, 1
#define BL 0, 1
#define BC 0.5f, 1
#define TL 0, 0
#define TR 1, 0
#define TC 0.5f, 0
static const Def defs[] = {
    // Under the right thumb: the arc of four, then grapple and crouch further along it. Upright, the four are a diamond and the shoulders on top.
    {"cross", B_SOUTH, {BR, -86, -66, 34}, {BR, -174, -77, 44}, "", "A", "figure.run", "Бег, действие, планирование, уклонение", SH_CROSS, 0x7FB2E5, 0x6DB33F},
    {"square", B_WEST, {BR, -184, -60, 36}, {BR, -273, -175, 44}, "", "X", "hand.raised.fill", "Удар, тихое добивание", SH_SQUARE, 0xE48AD6, 0x3C8FE0},
    {"triangle", B_NORTH, {BR, -140, -154, 34}, {BR, -174, -274, 42}, "", "Y", "shield.fill", "Захват, контратака", SH_TRIANGLE, 0x5CC49A, 0xF5C518},
    {"circle", B_EAST, {BR, -50, -164, 30}, {BR, -76, -175, 42}, "", "B", "wind", "Плащ, отмена, пропуск сцены", SH_CIRCLE, 0xE86A6A, 0xE03A3E},
    {"r1", B_RB, {BR, -86, -258, 27}, {TR, -76, 55, 33}, "R1", "RB", "arrow.up.right", "Крюк", SH_NONE, 0xFFFFFF, 0xFFFFFF},
    {"r2", C_RT, {BR, -236, -158, 28}, {TR, -185, 55, 33}, "R2", "RT", "arrow.down.to.line", "Присесть, гаджет, бросок", SH_NONE, 0xFFFFFF, 0xFFFFFF},
    {"r3", B_R3, {BR, -200, -250, 21}, {BR, -49, -49, 26}, "R3", "RS", "plus.magnifyingglass", "Зум камеры", SH_NONE, 0xFFFFFF, 0xFFFFFF},
    // Above the left thumb's stick.
    {"l1", B_LB, {BL, 68, -262, 28}, {TL, 76, 55, 33}, "L1", "LB", "eye.fill", "Режим детектива", SH_NONE, 0xFFFFFF, 0xFFFFFF},
    {"l2", C_LT, {BL, 166, -276, 28}, {TL, 185, 55, 33}, "L2", "LT", "scope", "Прицел гаджета", SH_NONE, 0xFFFFFF, 0xFFFFFF},
    {"l3", B_L3, {BL, 60, -350, 20}, {TL, 60, 153, 26}, "L3", "LS", "dot.viewfinder", "Камера за спину", SH_NONE, 0xFFFFFF, 0xFFFFFF},
    // The gadgets: one button for the pad's cross, a wheel of its four sides.
    {"gadgets", C_WHEEL, {TL, 124, 144, 30}, {BC, 0, -77, 37}, "", "", "dpad.fill", "Гаджеты: зажмите и ведите в сторону", SH_NONE, 0xFFFFFF, 0xFFFFFF},
    // The top.
    {"map", B_BACK, {TC, -64, 28, 20}, {TC, -44, 55, 24}, "", "", "map.fill", "Карта", SH_NONE, 0xFFFFFF, 0xFFFFFF},
    {"pause", B_START, {TC, 64, 28, 20}, {TC, 44, 55, 24}, "", "", "pause.fill", "Пауза", SH_NONE, 0xFFFFFF, 0xFFFFFF},
};
#define N_DEFS (sizeof defs / sizeof defs[0])
// Outside play the only one: backs out of menus, closes the map, skips a cutscene. It stays at the top left, as the gear does at the right.
static const Def close_def = {"close", B_EAST, {TL, 0, 0, 22}, {TL, 0, 0, 22}, "", "", "xmark", "Назад", SH_NONE, 0xFFFFFF, 0xFFFFFF};
#undef BR
#undef BL
#undef BC
#undef TL
#undef TR
#undef TC

// Where the controls' first layouts measured each button from ("layout": offsets from a corner or the top's middle, in points divided by a scale of
// the screen's height clamped to 0.8...1.3), for bringing a layout kept then over.
enum { OLD_BR, OLD_BL, OLD_TL, OLD_TC, OLD_NONE };
static int old_anchor(NSString *ident) {
    for (NSString *s in @[@"cross", @"square", @"triangle", @"circle", @"r1", @"r2", @"r3"])
        if ([ident isEqualToString:s]) return OLD_BR;
    for (NSString *s in @[@"l1", @"l2", @"l3"])
        if ([ident isEqualToString:s]) return OLD_BL;
    for (NSString *s in @[@"up", @"down", @"left", @"right"])
        if ([ident isEqualToString:s]) return OLD_TL;
    for (NSString *s in @[@"map", @"pause"])
        if ([ident isEqualToString:s]) return OLD_TC;
    return OLD_NONE;
}

static UIColor *rgb(int c, CGFloat a) { return [UIColor colorWithRed:((c >> 16) & 255) / 255.0 green:((c >> 8) & 255) / 255.0 blue:(c & 255) / 255.0 alpha:a]; }

// The PlayStation symbols, in a box of the given size around a centre.
static UIBezierPath *ps_shape(int shape, CGPoint c, CGFloat s) {
    UIBezierPath *p = [UIBezierPath bezierPath];
    switch (shape) {
        case SH_CROSS:
            [p moveToPoint:CGPointMake(c.x - s, c.y - s)];
            [p addLineToPoint:CGPointMake(c.x + s, c.y + s)];
            [p moveToPoint:CGPointMake(c.x + s, c.y - s)];
            [p addLineToPoint:CGPointMake(c.x - s, c.y + s)];
            break;
        case SH_CIRCLE: [p appendPath:[UIBezierPath bezierPathWithOvalInRect:CGRectMake(c.x - s, c.y - s, s * 2, s * 2)]]; break;
        case SH_SQUARE: [p appendPath:[UIBezierPath bezierPathWithRect:CGRectMake(c.x - s * 0.9, c.y - s * 0.9, s * 1.8, s * 1.8)]]; break;
        case SH_TRIANGLE:
            [p moveToPoint:CGPointMake(c.x, c.y - s * 1.05)];
            [p addLineToPoint:CGPointMake(c.x + s * 1.05, c.y + s * 0.8)];
            [p addLineToPoint:CGPointMake(c.x - s * 1.05, c.y + s * 0.8)];
            [p closePath];
            break;
    }
    return p;
}
static UIImage *symbol(NSString *name, CGFloat size, UIColor *color) {
    UIImageSymbolConfiguration *cfg = [UIImageSymbolConfiguration configurationWithPointSize:size weight:UIImageSymbolWeightSemibold];
    UIImage *img = [UIImage systemImageNamed:name withConfiguration:cfg];
    if (!img && [name isEqualToString:@"dpad.fill"]) img = [UIImage systemImageNamed:@"plus.circle.fill" withConfiguration:cfg];
    return [img imageWithTintColor:color renderingMode:UIImageRenderingModeAlwaysOriginal];
}

// The camera's push after the PlayStation pad's 255 steps a stick has (shims_hid.m stick()): a slow swipe's push falls between two of them, so what's
// left over goes on to the next frame and the camera still turns, on average, as far as the thumb moved. A quarter of a step more lands each value
// on its step for sure; the Xbox pad's finer steps take it as it is.
static float in_steps(float v, float *carry) {
    if (v == 0) {
        *carry = 0;
        return 0;
    }
    float want = v * 127.5f + *carry, steps = roundf(want);
    *carry = want - steps;
    return steps == 0 ? 0 : (steps + 0.25f) / 127.5f;
}

@interface PadButton : NSObject
@property(nonatomic) const Def *def;
@property(nonatomic) CGPoint center;
@property(nonatomic) CGFloat radius;
@property(nonatomic, strong) CALayer *group;
@property(nonatomic, strong) CAShapeLayer *back, *symbol, *badge;
@property(nonatomic, strong) CATextLayer *text, *badge_text;
@property(nonatomic, strong) CALayer *icon;
@property(nonatomic) int holders;
@property(nonatomic) BOOL shown;           // drawn and taking touches now
@property(nonatomic) BOOL hiddenByPlayer;  // hidden in the layout editor
@end
@implementation PadButton
@end

@interface TouchControlsView : UIView
@property(nonatomic) BOOL editing;
- (void)restyle;
- (void)releaseAll;
- (void)startEditing;
@end
static TouchControlsView *controls;
static void open_settings(void);

@implementation TouchControlsView {
    NSMutableArray<PadButton *> *buttons;
    PadButton *close_button;
    NSMapTable<UITouch *, id> *owners;
    NSMapTable<UITouch *, NSValue *> *tap_start;
    NSMapTable<UITouch *, NSNumber *> *tap_time;
    UITouch *stick_touch, *look_touch, *pan_touch, *wheel_touch;
    CGPoint stick_center, look_last, look_sum, pan_last, pan_sum;
    float lx, ly, rx, ry;
    float look_prev_x, look_prev_y, carry_x, carry_y;
    BOOL running;  // the stick pushed past its edge
    BOOL panning;  // the touch outside play has moved far enough to be a drag
    BOOL in_menu;  // outside play
    CFTimeInterval last_frame, last_held;
    unsigned held_frame;
    CGSize laid_out;
    unsigned pulse_bits;  // the wheel's choice, held for a moment
    CFTimeInterval pulse_until;
    CAShapeLayer *stick_base, *stick_knob;
    UIImpactFeedbackGenerator *tap;
    UISelectionFeedbackGenerator *tick;
    CADisplayLink *link;
    // the gadgets' wheel
    CAShapeLayer *wheel_disc, *wheel_sides[4];
    CALayer *wheel_icons[4];
    CGPoint wheel_center, wheel_origin;
    CGFloat wheel_R;
    int wheel_side;
    BOOL wheel_open;
    // editing
    PadButton *dragged, *selected;
    CGPoint drag_offset;
    UIView *toolbar;
    UISlider *size_slider;
    UIButton *hide_button, *reset_one_button;
    UILabel *what_label;
}

- (PadButton *)makeButton:(const Def *)d {
    PadButton *b = [PadButton new];
    b.def = d;
    b.group = [CALayer layer];
    b.back = [CAShapeLayer layer];
    b.symbol = [CAShapeLayer layer];
    b.badge = [CAShapeLayer layer];
    b.text = [CATextLayer layer];
    b.badge_text = [CATextLayer layer];
    b.icon = [CALayer layer];
    for (CATextLayer *t in @[b.text, b.badge_text]) {
        t.alignmentMode = kCAAlignmentCenter;
        t.contentsScale = UIScreen.mainScreen.scale;
    }
    b.icon.contentsGravity = kCAGravityResizeAspect;
    for (CALayer *l in @[b.back, b.symbol, b.text, b.icon, b.badge, b.badge_text]) [b.group addSublayer:l];
    [self.layer addSublayer:b.group];
    return b;
}

- (instancetype)initWithFrame:(CGRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    self.multipleTouchEnabled = YES;
    self.backgroundColor = UIColor.clearColor;
    owners = [NSMapTable strongToStrongObjectsMapTable];
    tap_start = [NSMapTable strongToStrongObjectsMapTable];
    tap_time = [NSMapTable strongToStrongObjectsMapTable];
    tap = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];
    tick = [UISelectionFeedbackGenerator new];
    buttons = [NSMutableArray array];
    for (size_t i = 0; i < N_DEFS; i++) [buttons addObject:[self makeButton:&defs[i]]];
    close_button = [self makeButton:&close_def];
    stick_base = [CAShapeLayer layer];
    stick_knob = [CAShapeLayer layer];
    for (CAShapeLayer *l in @[stick_base, stick_knob]) {
        l.hidden = YES;
        [self.layer addSublayer:l];
    }
    stick_base.lineWidth = 2;
    wheel_disc = [CAShapeLayer layer];
    wheel_disc.fillColor = [UIColor colorWithWhite:0 alpha:0.35].CGColor;
    wheel_disc.strokeColor = [UIColor colorWithWhite:1 alpha:0.5].CGColor;
    wheel_disc.lineWidth = 1.5;
    wheel_disc.zPosition = 10;
    wheel_disc.hidden = YES;
    [self.layer addSublayer:wheel_disc];
    NSArray *chevrons = @[@"chevron.up", @"chevron.right", @"chevron.down", @"chevron.left"];
    for (int i = 0; i < 4; i++) {
        wheel_sides[i] = [CAShapeLayer layer];
        wheel_sides[i].zPosition = 11;
        wheel_sides[i].strokeColor = [UIColor colorWithWhite:1 alpha:0.35].CGColor;
        wheel_sides[i].lineWidth = 1;
        wheel_icons[i] = [CALayer layer];
        wheel_icons[i].zPosition = 12;
        wheel_icons[i].contentsGravity = kCAGravityResizeAspect;
        wheel_icons[i].contents = (__bridge id)symbol(chevrons[i], 40, UIColor.whiteColor).CGImage;
        wheel_sides[i].hidden = wheel_icons[i].hidden = YES;
        [self.layer addSublayer:wheel_sides[i]];
        [self.layer addSublayer:wheel_icons[i]];
    }
    wheel_side = -1;
    link = [CADisplayLink displayLinkWithTarget:self selector:@selector(frame:)];
    if (@available(iOS 15.0, *)) link.preferredFrameRateRange = CAFrameRateRangeMake(60, 120, 120);
    [link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    fprintf(stderr, "[touch] camera speed %.2f (a swipe of %.0f points a second is the stick's full push)\n", setting(@"lookSpeed2", "ARKHAM_TOUCH_LOOK", 1.0),
            3000.0 / setting(@"lookSpeed2", "ARKHAM_TOUCH_LOOK", 1.0));
    return self;
}

// ---------- where things go ----------
- (BOOL)portrait { return self.bounds.size.height > self.bounds.size.width; }
// The space for the controls: the screen inside its rounded corners and camera, and held upright only what's below the game's picture.
- (CGRect)padArea {
    CGSize s = self.bounds.size;
    UIEdgeInsets in = self.safeAreaInsets;
    CGFloat left = MAX(in.left, 8), right = s.width - MAX(in.right, 8), top = MAX(in.top, 8), bottom = s.height - MAX(in.bottom, 8);
    if ([self portrait]) top = MIN(MAX(top, CGRectGetMaxY(rt_ios_game_rect()) + 8), bottom - 120);
    return CGRectMake(left, top, MAX(right - left, 1), MAX(bottom - top, 1));
}
- (CGFloat)sizeSetting { return setting(@"buttonScale", "ARKHAM_TOUCH_SIZE", 1.0); }
// Points for each of the default places' and the kept sizes': in proportion to the space's height, and the size setting.
- (CGFloat)unit { return [self padArea].size.height / REF_H * [self sizeSetting]; }
- (NSString *)layoutKey { return [self portrait] ? @"layout.portrait" : @"layout.landscape"; }
- (NSDictionary *)layout { return [prefs() dictionaryForKey:[self layoutKey]] ?: @{}; }

// A button's place: the default, or where the player put it, kept as a share of the space's width and height, and its size in default points.
- (void)placeButton:(PadButton *)b {
    CGSize vs = self.bounds.size;
    UIEdgeInsets in = self.safeAreaInsets;
    if (b == close_button) {
        b.center = CGPointMake(MAX(in.left, 8) + 34, MAX(in.top, 8) + 28);
        b.radius = 22;
        return;
    }
    CGRect a = [self padArea];
    CGFloat u = [self unit];
    NSDictionary *mine = [self layout][@(b.def->ident)];
    const Place *d = [self portrait] ? &b.def->port : &b.def->land;
    CGFloat r = (mine[@"r"] ? [mine[@"r"] doubleValue] : d->r) * u;
    CGPoint c;
    if (mine[@"x"])
        c = CGPointMake(a.origin.x + [mine[@"x"] doubleValue] * a.size.width, a.origin.y + [mine[@"y"] doubleValue] * a.size.height);
    else
        c = CGPointMake(a.origin.x + d->ax * a.size.width + d->dx * u, a.origin.y + d->ay * a.size.height + d->dy * u);
    b.hiddenByPlayer = [mine[@"hidden"] boolValue];
    b.center = CGPointMake(MIN(MAX(c.x, r + 2), vs.width - r - 2), MIN(MAX(c.y, r + 2), vs.height - r - 2));  // never off the screen
    b.radius = r;
}
- (void)changeLayout:(PadButton *)b with:(void (^)(NSMutableDictionary *mine))change {
    NSMutableDictionary *layout = [[self layout] mutableCopy];
    NSMutableDictionary *mine = [layout[@(b.def->ident)] mutableCopy] ?: [NSMutableDictionary dictionary];
    change(mine);
    if (mine.count)
        layout[@(b.def->ident)] = mine;
    else
        [layout removeObjectForKey:@(b.def->ident)];
    [prefs() setObject:layout forKey:[self layoutKey]];
}
- (void)storeButton:(PadButton *)b {
    CGRect a = [self padArea];
    CGFloat u = [self unit];
    [self changeLayout:b with:^(NSMutableDictionary *mine) {
        mine[@"x"] = @((b.center.x - a.origin.x) / a.size.width);
        mine[@"y"] = @((b.center.y - a.origin.y) / a.size.height);
        mine[@"r"] = @(b.radius / u);
    }];
}

// A layout kept by the controls' first version ("layout"), brought over the first time the phone is held sideways: each button where it was on the
// screen it's on now, the four arrows' middle for the wheel that replaced them. The old one stays as "layout.v2".
- (void)bringOldLayoutOver {
    NSDictionary *old = [prefs() dictionaryForKey:@"layout"];
    CGSize s = self.bounds.size;
    if (!old || [self portrait] || s.width < 1 || s.height < 1) return;
    CGRect a = [self padArea];
    CGFloat u = [self unit];
    CGFloat k = MIN(MAX(MIN(s.width, s.height) / 480.0, 0.8), 1.3) * [self sizeSetting];
    NSMutableDictionary *layout = [NSMutableDictionary dictionary];
    CGPoint arrows = CGPointZero;
    int n_arrows = 0;
    for (NSString *ident in old) {
        NSDictionary *e = old[ident];
        CGPoint from;
        switch (old_anchor(ident)) {
            case OLD_BR: from = CGPointMake(CGRectGetMaxX(a), CGRectGetMaxY(a)); break;
            case OLD_BL: from = CGPointMake(a.origin.x, CGRectGetMaxY(a)); break;
            case OLD_TL: from = a.origin; break;
            case OLD_TC: from = CGPointMake(s.width / 2, a.origin.y); break;
            default: continue;
        }
        CGPoint c = CGPointMake(from.x + [e[@"dx"] doubleValue] * k, from.y + [e[@"dy"] doubleValue] * k);
        if (old_anchor(ident) == OLD_TL) {
            arrows.x += c.x;
            arrows.y += c.y;
            n_arrows++;
            continue;
        }
        layout[ident] = @{@"x": @((c.x - a.origin.x) / a.size.width), @"y": @((c.y - a.origin.y) / a.size.height), @"r": @([e[@"r"] doubleValue] * k / u)};
    }
    if (n_arrows)
        layout[@"gadgets"] = @{@"x": @((arrows.x / n_arrows - a.origin.x) / a.size.width), @"y": @((arrows.y / n_arrows - a.origin.y) / a.size.height), @"r": @(30)};
    [prefs() setObject:layout forKey:@"layout.landscape"];
    [prefs() setObject:old forKey:@"layout.v2"];
    [prefs() removeObjectForKey:@"layout"];
    [prefs() synchronize];
    fprintf(stderr, "[touch] the layout kept before brought over: %lu buttons, now in proportion to the screen\n", (unsigned long)layout.count);
}

- (void)layoutSubviews {
    [super layoutSubviews];
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    for (PadButton *b in buttons) b.group.frame = self.bounds;
    close_button.group.frame = self.bounds;
    [CATransaction commit];
    if (!CGSizeEqualToSize(self.bounds.size, laid_out)) {
        laid_out = self.bounds.size;
        if (wheel_open) [self closeWheel];
    }
    [self bringOldLayoutOver];
    [self restyle];
    [self placeToolbar];
}

// ---------- drawing ----------
// Draws every button for the style chosen; only the back button outside play, and every one, hidden ones faintly, while the layout is edited.
- (void)drawButton:(PadButton *)b style:(int)style alpha:(CGFloat)alpha {
    const Def *d = b.def;
    CGPoint c = b.center;
    CGFloat r = b.radius;
    BOOL down = b.holders > 0;
    b.back.path = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(c.x - r, c.y - r, r * 2, r * 2)].CGPath;
    b.back.fillColor = [UIColor colorWithWhite:down ? 0.35 : 0 alpha:(down ? 0.6 : 0.32) * (alpha / 0.6)].CGColor;
    b.back.strokeColor = (self.editing && b.hiddenByPlayer ? rgb(0xE86A6A, 0.9) : [UIColor colorWithWhite:1 alpha:(self.editing ? 0.9 : 0.5) * alpha]).CGColor;
    b.back.lineWidth = self.editing && b == selected ? 3 : 1.5;
    b.back.lineDashPattern = self.editing ? @[@6, @4] : nil;
    b.symbol.hidden = b.text.hidden = b.icon.hidden = b.badge.hidden = b.badge_text.hidden = YES;
    BOOL letters = style != STYLE_ICONS && (d->shape != SH_NONE || *(style == STYLE_XBOX ? d->xbox : d->ps));
    if (letters && style == STYLE_PS && d->shape != SH_NONE) {
        // The PlayStation symbol, drawn.
        b.symbol.hidden = NO;
        b.symbol.path = ps_shape(d->shape, c, r * 0.42).CGPath;
        b.symbol.fillColor = UIColor.clearColor.CGColor;
        b.symbol.strokeColor = rgb(d->ps_color, down ? 1 : alpha + 0.25).CGColor;
        b.symbol.lineWidth = MAX(2.5, r * 0.11);
    } else if (letters) {
        const char *label = style == STYLE_XBOX ? d->xbox : d->ps;
        int color = style == STYLE_XBOX ? d->xbox_color : d->ps_color;
        CGFloat font = strlen(label) > 1 ? r * 0.62 : r * 0.9;
        b.text.hidden = NO;
        b.text.string = @(label);
        b.text.fontSize = font;
        b.text.font = (__bridge CFTypeRef)[UIFont systemFontOfSize:font weight:UIFontWeightBold];
        b.text.foregroundColor = rgb(color, down ? 1 : alpha + 0.3).CGColor;
        b.text.frame = CGRectMake(c.x - r, c.y - font * 0.62, r * 2, font * 1.3);
    } else {
        // An icon of what the button does, with the PlayStation symbol or name it is in a corner, to match the game's prompts.
        UIImage *img = symbol(@(d->icon), r * 0.85, UIColor.whiteColor);
        if (img) {
            b.icon.hidden = NO;
            b.icon.contents = (__bridge id)img.CGImage;
            b.icon.opacity = down ? 1 : MIN(1, alpha + 0.3);
            CGFloat w = r * 1.15;
            b.icon.frame = CGRectMake(c.x - w / 2, c.y - w / 2, w, w);
        }
        if (style == STYLE_ICONS && (d->shape != SH_NONE || *d->ps)) {
            CGPoint bc = CGPointMake(c.x + r * 0.72, c.y + r * 0.72);
            CGFloat br = MAX(8, r * 0.3);
            b.badge.hidden = NO;
            b.badge.path = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(bc.x - br, bc.y - br, br * 2, br * 2)].CGPath;
            b.badge.fillColor = [UIColor colorWithWhite:0 alpha:0.75].CGColor;
            b.badge.strokeColor = UIColor.clearColor.CGColor;
            if (d->shape != SH_NONE) {
                b.symbol.hidden = NO;
                b.symbol.path = ps_shape(d->shape, bc, br * 0.48).CGPath;
                b.symbol.fillColor = UIColor.clearColor.CGColor;
                b.symbol.strokeColor = rgb(d->ps_color, 1).CGColor;
                b.symbol.lineWidth = 1.6;
            } else {
                b.badge_text.hidden = NO;
                b.badge_text.string = @(d->ps);
                b.badge_text.fontSize = br * 0.9;
                b.badge_text.font = (__bridge CFTypeRef)[UIFont systemFontOfSize:br * 0.9 weight:UIFontWeightBold];
                b.badge_text.foregroundColor = UIColor.whiteColor.CGColor;
                b.badge_text.frame = CGRectMake(bc.x - br, bc.y - br * 0.6, br * 2, br * 1.3);
            }
        }
    }
}
- (void)restyle {
    int style = (int)setting(@"buttonStyle", "ARKHAM_TOUCH_STYLE", STYLE_PS);
    CGFloat alpha = setting(@"opacity", "ARKHAM_TOUCH_OPACITY", 0.6);
    rt_touch_pad_playstation(style != STYLE_XBOX);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    for (PadButton *b in [buttons arrayByAddingObject:close_button]) {
        [self placeButton:b];
        BOOL show = b == close_button ? in_menu && !self.editing : self.editing || (!in_menu && !b.hiddenByPlayer);
        b.shown = show;
        b.group.hidden = !show;
        if (!show) {
            b.holders = 0;
            continue;
        }
        b.group.opacity = self.editing && b.hiddenByPlayer ? 0.35 : 1;
        [self drawButton:b style:style alpha:b == close_button ? MAX(alpha, 0.6) : alpha];
    }
    stick_base.fillColor = [UIColor colorWithWhite:0 alpha:0.25 * (alpha / 0.6)].CGColor;
    stick_base.strokeColor = [UIColor colorWithWhite:1 alpha:0.45 * (alpha / 0.6)].CGColor;
    [CATransaction commit];
}

- (PadButton *)buttonAt:(CGPoint)p {
    PadButton *best = nil;
    CGFloat best_d = 1e9;
    for (PadButton *b in [buttons arrayByAddingObject:close_button]) {
        if (!b.shown) continue;
        CGFloat d = hypot(p.x - b.center.x, p.y - b.center.y);
        if (d < b.radius * 1.25 && d < best_d) {  // a little beyond what's drawn: thumbs are wide
            best = b;
            best_d = d;
        }
    }
    return best;
}

- (void)send {
    unsigned bits = 0;
    float lt = 0, rt = 0;
    for (PadButton *b in [buttons arrayByAddingObject:close_button]) {
        if (!b.holders) continue;
        int k = b.def->control;
        if (k == C_LT) lt = 1;
        else if (k == C_RT) rt = 1;
        else if (k != C_WHEEL) bits |= 1u << k;
    }
    if (running) bits |= 1u << B_SOUTH;
    if (pulse_bits && CACurrentMediaTime() < pulse_until) bits |= pulse_bits;
    rt_touch_pad(lx, ly, rx, ry, lt, rt, bits);
}

- (CGFloat)stickRadius { return 62 * [self unit] / 1.15 * ([self portrait] ? 1.2 : 1); }
- (void)showStickAt:(CGPoint)c knob:(CGPoint)k {
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    CGFloat R = [self stickRadius], kr = R * 0.42;
    stick_base.path = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(c.x - R, c.y - R, R * 2, R * 2)].CGPath;
    stick_knob.path = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(k.x - kr, k.y - kr, kr * 2, kr * 2)].CGPath;
    stick_knob.fillColor = (running ? rgb(0x7FB2E5, 0.8) : [UIColor colorWithWhite:1 alpha:0.45]).CGColor;
    stick_base.hidden = stick_knob.hidden = NO;
    [CATransaction commit];
}

// ---------- the gadgets' wheel ----------
- (void)drawWheel {
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    BOOL on = wheel_open;
    wheel_disc.hidden = !on;
    for (int i = 0; i < 4; i++) wheel_sides[i].hidden = wheel_icons[i].hidden = !on;
    if (on) {
        CGPoint c = wheel_center;
        CGFloat R = wheel_R, ri = R * 0.3;
        wheel_disc.path = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(c.x - R, c.y - R, R * 2, R * 2)].CGPath;
        for (int i = 0; i < 4; i++) {
            CGFloat mid = (i - 1) * M_PI_2, a0 = mid - M_PI_4 + 0.06, a1 = mid + M_PI_4 - 0.06;  // up, right, down, left: -90, 0, 90, 180 degrees
            UIBezierPath *w = [UIBezierPath bezierPath];
            [w addArcWithCenter:c radius:R - 5 startAngle:a0 endAngle:a1 clockwise:YES];
            [w addArcWithCenter:c radius:ri startAngle:a1 endAngle:a0 clockwise:NO];
            [w closePath];
            wheel_sides[i].path = w.CGPath;
            wheel_sides[i].fillColor = (i == wheel_side ? rgb(0x7FB2E5, 0.8) : [UIColor colorWithWhite:0 alpha:0.55]).CGColor;
            CGFloat at = (R + ri) / 2, size = R * 0.3;
            wheel_icons[i].frame = CGRectMake(c.x + cos(mid) * at - size / 2, c.y + sin(mid) * at - size / 2, size, size);
        }
    }
    [CATransaction commit];
}
- (void)openWheel:(PadButton *)b at:(CGPoint)p {
    CGSize s = self.bounds.size;
    wheel_open = YES;
    wheel_origin = p;
    wheel_side = -1;
    wheel_R = MAX(b.radius * 3.0, 96);
    CGFloat m = wheel_R + 6;
    wheel_center = CGPointMake(MIN(MAX(b.center.x, m), s.width - m), MIN(MAX(b.center.y, m), s.height - m));
    [self drawWheel];
}
- (void)closeWheel {
    wheel_open = NO;
    wheel_side = -1;
    [self drawWheel];
}
// Which side a point is on from a centre: none near it, and, for a tap on the open wheel, none well outside it either.
- (int)sideAt:(CGPoint)p from:(CGPoint)c within:(BOOL)within {
    CGFloat dx = p.x - c.x, dy = p.y - c.y, d = hypot(dx, dy);
    if (d < MAX(wheel_R * 0.3, 18) || (within && d > wheel_R * 1.25)) return -1;
    if (fabs(dx) > fabs(dy)) return dx > 0 ? SIDE_RIGHT : SIDE_LEFT;
    return dy > 0 ? SIDE_DOWN : SIDE_UP;
}
- (void)wheelMovedTo:(CGPoint)p {
    int side = [self sideAt:p from:wheel_origin within:NO];
    if (side == wheel_side) return;
    wheel_side = side;
    if (side >= 0) [tick selectionChanged];
    [self drawWheel];
}
// The pad's cross pressed on that side for a moment, as the game wants a press to select a gadget.
- (void)choose:(int)side {
    static const int bits[] = {D_UP, D_RIGHT, D_DOWN, D_LEFT};
    pulse_bits = 1u << bits[side];
    pulse_until = CACurrentMediaTime() + 0.12;
    [tap impactOccurred];
    [self send];
}

// ---------- each display frame ----------
// Whether the game is in play: there it warps the cursor back every frame. A game whose frames go on without that is in a menu; one that has stopped
// both is only busy, and stays in play.
- (void)updatePlay:(CFTimeInterval)now {
    unsigned frames = rt_frames_presented();
    if (rt_game_holds_cursor() || self.editing || setting(@"hideInMenus", "ARKHAM_TOUCH_HIDE", 1) <= 0) {
        last_held = now;
        held_frame = frames;
    }
    BOOL menu = frames - held_frame >= 20 && now - last_held > 0.3;
    if (menu == in_menu) return;
    [self releaseAll];
    in_menu = menu;
    if (menu) rt_ios_park_cursor();
    [self restyle];
    fprintf(stderr, "[touch] %s\n", menu ? "outside play: the back button only; taps click, drags are the left stick" : "in play: the pad is back");
}
// The camera: the thumb's speed as the right stick's push, so that it turns as far as the thumb moves. Two display frames are averaged, as touches and
// frames don't come in step; that keeps the sum over a swipe.
- (void)frame:(CADisplayLink *)l {
    CFTimeInterval now = l.timestamp, dt = last_frame > 0 ? now - last_frame : 1.0 / 60;
    last_frame = now;
    if (dt <= 0 || dt > 0.1) dt = 1.0 / 60;
    [self updatePlay:now];
    float nx = 0, ny = 0;
    if (look_touch && !in_menu) {
        double full = 3000.0 / setting(@"lookSpeed2", "ARKHAM_TOUCH_LOOK", 1.0);  // points a second for the stick's full push
        nx = (float)(look_sum.x / dt / full);
        ny = (float)(-look_sum.y / dt / full);
        float m = hypotf(nx, ny);
        if (m > 1) {
            nx /= m;
            ny /= m;
        }
    }
    look_sum = CGPointZero;
    rx = in_steps((nx + look_prev_x) * 0.5f, &carry_x);
    ry = in_steps((ny + look_prev_y) * 0.5f, &carry_y);
    look_prev_x = nx;
    look_prev_y = ny;
    // Outside play a drag is the left stick, the other way from the finger: the map goes with it, as on a phone's map.
    if (in_menu) {
        float px = 0, py = 0;
        if (pan_touch && panning) {
            double vx = pan_sum.x / dt, vy = pan_sum.y / dt, v = hypot(vx, vy);
            if (v > 20) {
                double m = MIN(1.0, 0.35 + v / 1600.0);  // past the game's dead zone from the start
                px = (float)(-vx / v * m);
                py = (float)(vy / v * m);
            }
        }
        pan_sum = CGPointZero;
        lx = lx * 0.4f + px * 0.6f;
        ly = ly * 0.4f + py * 0.6f;
        if (px == 0 && py == 0 && fabsf(lx) < 0.3f && fabsf(ly) < 0.3f) lx = ly = 0;
    }
    if (pulse_bits && now > pulse_until) pulse_bits = 0;
    [self send];
}

// ---------- touches ----------
- (BOOL)inStickZone:(CGPoint)p {
    CGRect a = [self padArea];
    if ([self portrait] && p.y < a.origin.y - 8) return NO;  // on the picture: the camera
    return p.x < a.origin.x + a.size.width * 0.45;
}
// A click where a tap landed, as a share of the game's picture.
- (void)clickAt:(CGPoint)p {
    CGRect g = rt_ios_game_rect();
    if (!CGRectContainsPoint(g, p)) return;
    rt_ios_click((p.x - g.origin.x) / g.size.width, (p.y - g.origin.y) / g.size.height);
}
- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    for (UITouch *t in touches) {
        CGPoint p = [t locationInView:self];
        if (self.editing) {
            [self editTouchBegan:p];
            continue;
        }
        // The wheel left open: a touch on one of its sides chooses that, anywhere else only closes it.
        if (wheel_open && !wheel_touch) {
            int side = [self sideAt:p from:wheel_center within:YES];
            [self closeWheel];
            if (side >= 0) {
                [self choose:side];
                [owners setObject:@"wheel" forKey:t];
                continue;
            }
        }
        PadButton *b = [self buttonAt:p];
        if (b && b.def->control == C_WHEEL) {
            if (!wheel_touch) {
                wheel_touch = t;
                [owners setObject:b forKey:t];
                [self openWheel:b at:p];
                [tap impactOccurred];
            }
            continue;
        }
        if (b) {
            [owners setObject:b forKey:t];
            b.holders++;
            [self restyle];
            [tap impactOccurred];
            continue;
        }
        [tap_start setObject:[NSValue valueWithCGPoint:p] forKey:t];
        [tap_time setObject:@(t.timestamp) forKey:t];
        if (in_menu) {
            if (!pan_touch) {
                pan_touch = t;
                pan_last = p;
                pan_sum = CGPointZero;
                panning = NO;
                [owners setObject:@"pan" forKey:t];
            }
            continue;
        }
        if ([self inStickZone:p] && !stick_touch) {
            stick_touch = t;
            stick_center = p;
            lx = ly = 0;
            running = NO;
            [owners setObject:@"stick" forKey:t];
            [self showStickAt:p knob:p];
        } else if (!look_touch) {
            look_touch = t;
            look_last = p;
            [owners setObject:@"look" forKey:t];
        }
    }
    [self send];
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    for (UITouch *t in touches) {
        CGPoint p = [t locationInView:self];
        if (self.editing) {
            [self editTouchMoved:p];
            continue;
        }
        if (t == wheel_touch) {
            [self wheelMovedTo:p];
        } else if (t == pan_touch) {
            for (UITouch *c in [event coalescedTouchesForTouch:t] ?: @[t]) {
                CGPoint q = [c locationInView:self];
                pan_sum.x += q.x - pan_last.x;
                pan_sum.y += q.y - pan_last.y;
                pan_last = q;
            }
            CGPoint s0 = [tap_start objectForKey:t].CGPointValue;
            if (!panning && hypot(p.x - s0.x, p.y - s0.y) > 14) panning = YES;
        } else if (t == stick_touch) {
            CGFloat R = [self stickRadius], dx = p.x - stick_center.x, dy = p.y - stick_center.y, d = hypot(dx, dy);
            BOOL run = setting(@"autoRun", "ARKHAM_TOUCH_RUN", 1) > 0 && d > R * 1.45;
            if (run != running) {
                running = run;
                if (run) [tap impactOccurred];
            }
            CGFloat shown = MIN(d, R);
            float m = (float)(shown / R);
            m = m < 0.12f ? 0 : (m - 0.12f) / 0.88f;  // a dead zone, so a resting thumb doesn't walk
            lx = d > 0 ? (float)(dx / d) * m : 0;
            ly = d > 0 ? (float)(-dy / d) * m : 0;
            [self showStickAt:stick_center knob:CGPointMake(stick_center.x + (d > 0 ? dx / d * shown : 0), stick_center.y + (d > 0 ? dy / d * shown : 0))];
        } else if (t == look_touch) {
            for (UITouch *c in [event coalescedTouchesForTouch:t] ?: @[t]) {
                CGPoint q = [c locationInView:self];
                look_sum.x += q.x - look_last.x;
                look_sum.y += q.y - look_last.y;
                look_last = q;
            }
        }
    }
    [self send];
}

- (void)lift:(NSSet<UITouch *> *)touches tapped:(BOOL)ended {
    for (UITouch *t in touches) {
        if (self.editing) {
            [self editTouchEnded];
            continue;
        }
        id owner = [owners objectForKey:t];
        if (t == wheel_touch) {
            wheel_touch = nil;
            if (ended && wheel_side >= 0) {
                [self choose:wheel_side];
                [self closeWheel];
            } else if (!ended) {
                [self closeWheel];
            } else {
                wheel_side = -1;  // let go without choosing: it stays open for a tap on a side
                [self drawWheel];
            }
        } else if ([owner isKindOfClass:PadButton.class]) {
            PadButton *b = owner;
            if (b.holders > 0) b.holders--;
            [self restyle];
        }
        // A short touch that hardly moved, outside play, is a click there too.
        NSValue *start = [tap_start objectForKey:t];
        NSNumber *when = [tap_time objectForKey:t];
        if (ended && start && when) {
            CGPoint p = [t locationInView:self], s0 = start.CGPointValue;
            if (t.timestamp - when.doubleValue < 0.35 && hypot(p.x - s0.x, p.y - s0.y) < 14 && (in_menu || !rt_game_holds_cursor())) [self clickAt:p];
        }
        [tap_start removeObjectForKey:t];
        [tap_time removeObjectForKey:t];
        if (t == pan_touch) {
            pan_touch = nil;
            panning = NO;
            pan_sum = CGPointZero;
        }
        if (t == stick_touch) {
            stick_touch = nil;
            lx = ly = 0;
            running = NO;
            stick_base.hidden = stick_knob.hidden = YES;
        }
        if (t == look_touch) {
            look_touch = nil;
            look_sum = CGPointZero;
        }
        [owners removeObjectForKey:t];
    }
    [self send];
}
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { [self lift:touches tapped:YES]; }
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { [self lift:touches tapped:NO]; }

- (void)releaseAll {
    for (PadButton *b in buttons) b.holders = 0;
    close_button.holders = 0;
    [owners removeAllObjects];
    [tap_start removeAllObjects];
    [tap_time removeAllObjects];
    stick_touch = look_touch = pan_touch = wheel_touch = nil;
    lx = ly = rx = ry = 0;
    look_sum = pan_sum = CGPointZero;
    look_prev_x = look_prev_y = carry_x = carry_y = 0;
    running = panning = NO;
    pulse_bits = 0;
    stick_base.hidden = stick_knob.hidden = YES;
    if (wheel_open) [self closeWheel];
    [self restyle];
    [self send];
}

// ---------- the layout editor: moving, resizing, hiding and resetting each button ----------
- (UIButton *)toolbarButton:(NSString *)title frame:(CGRect)f action:(SEL)a bold:(BOOL)bold {
    UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
    [b setTitle:title forState:UIControlStateNormal];
    b.titleLabel.font = [UIFont systemFontOfSize:16 weight:bold ? UIFontWeightSemibold : UIFontWeightRegular];
    b.frame = f;
    [b addTarget:self action:a forControlEvents:UIControlEventTouchUpInside];
    [toolbar addSubview:b];
    return b;
}
- (void)buildToolbar {
    toolbar = [[UIView alloc] initWithFrame:CGRectMake(0, 0, 480, 134)];
    toolbar.backgroundColor = [UIColor colorWithWhite:0.08 alpha:0.9];
    toolbar.layer.cornerRadius = 14;
    UILabel *hint = [[UILabel alloc] initWithFrame:CGRectMake(14, 8, 452, 20)];
    hint.text = @"Перетаскивайте кнопки. Нажмите на кнопку, чтобы её изменить.";
    hint.textColor = UIColor.whiteColor;
    hint.font = [UIFont systemFontOfSize:13 weight:UIFontWeightMedium];
    [toolbar addSubview:hint];
    UILabel *size = [[UILabel alloc] initWithFrame:CGRectMake(14, 40, 60, 24)];
    size.textColor = UIColor.lightGrayColor;
    size.font = [UIFont systemFontOfSize:13];
    size.text = @"Размер";
    [toolbar addSubview:size];
    size_slider = [[UISlider alloc] initWithFrame:CGRectMake(72, 38, 140, 28)];
    size_slider.minimumValue = 14;
    size_slider.maximumValue = 64;
    [size_slider addTarget:self action:@selector(sizeChanged:) forControlEvents:UIControlEventValueChanged];
    [size_slider addTarget:self action:@selector(persist) forControlEvents:UIControlEventTouchUpInside | UIControlEventTouchUpOutside];
    [toolbar addSubview:size_slider];
    hide_button = [self toolbarButton:@"Скрыть" frame:CGRectMake(222, 36, 104, 32) action:@selector(hideSelected) bold:NO];
    reset_one_button = [self toolbarButton:@"Сбросить кнопку" frame:CGRectMake(330, 36, 140, 32) action:@selector(resetSelected) bold:NO];
    what_label = [[UILabel alloc] initWithFrame:CGRectMake(14, 70, 452, 20)];
    what_label.textColor = UIColor.lightGrayColor;
    what_label.font = [UIFont systemFontOfSize:12];
    [toolbar addSubview:what_label];
    UIButton *all = [self toolbarButton:@"Сбросить всё…" frame:CGRectMake(10, 96, 150, 32) action:@selector(askResetAll) bold:NO];
    [all setTitleColor:rgb(0xE86A6A, 1) forState:UIControlStateNormal];
    [self toolbarButton:@"Готово" frame:CGRectMake(376, 96, 94, 32) action:@selector(stopEditing) bold:YES];
}
// At the top held sideways; upright, over the game's picture, out of the controls' way. Smaller on a narrow screen.
- (void)placeToolbar {
    if (!toolbar) return;
    CGSize s = self.bounds.size;
    CGFloat k = MIN(1, (s.width - 16) / 480);
    toolbar.transform = CGAffineTransformMakeScale(k, k);
    CGRect g = rt_ios_game_rect();
    toolbar.center = [self portrait] ? CGPointMake(s.width / 2, CGRectGetMidY(g)) : CGPointMake(s.width / 2, MAX(self.safeAreaInsets.top, 8) + 72 * k);
}
- (void)startEditing {
    [self releaseAll];
    self.editing = YES;
    in_menu = NO;
    if (!toolbar) [self buildToolbar];
    toolbar.hidden = NO;
    [self addSubview:toolbar];
    [self select:nil];
    [self placeToolbar];
    [self restyle];
}
- (void)persist { [prefs() synchronize]; }
- (void)stopEditing {
    self.editing = NO;
    toolbar.hidden = YES;
    selected = dragged = nil;
    [self persist];
    [self restyle];
}
- (void)select:(PadButton *)b {
    selected = b;
    size_slider.enabled = hide_button.enabled = reset_one_button.enabled = b != nil;
    size_slider.value = b ? b.radius / [self unit] : 30;
    [hide_button setTitle:b.hiddenByPlayer ? @"Показать" : @"Скрыть" forState:UIControlStateNormal];
    what_label.text = b ? [@(b.def->hint) stringByAppendingString:b.hiddenByPlayer ? @" · скрыта в игре" : @""] : @"Скрытые кнопки видны здесь бледными.";
    [self restyle];
}
- (void)sizeChanged:(UISlider *)s {
    if (!selected) return;
    selected.radius = s.value * [self unit];
    [self storeButton:selected];
    [self restyle];
}
- (void)hideSelected {
    if (!selected) return;
    BOOL hide = !selected.hiddenByPlayer;
    [self changeLayout:selected with:^(NSMutableDictionary *mine) {
        if (hide)
            mine[@"hidden"] = @YES;
        else
            [mine removeObjectForKey:@"hidden"];
    }];
    [self persist];
    [self restyle];
    [self select:selected];
}
// Back where it was at first, and shown, this button only.
- (void)resetSelected {
    if (!selected) return;
    [self changeLayout:selected with:^(NSMutableDictionary *mine) { [mine removeAllObjects]; }];
    [self persist];
    [self restyle];
    [self select:selected];
}
// Every button back where it was at first, only once asked: a slip of the finger shouldn't undo a whole layout.
- (void)askResetAll {
    NSString *which = [self portrait] ? @"вертикального" : @"горизонтального";
    UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Сбросить все кнопки?"
                                                               message:[NSString stringWithFormat:@"Расположение, размер и скрытие всех кнопок для %@ режима вернутся к исходным.", which]
                                                        preferredStyle:UIAlertControllerStyleAlert];
    [a addAction:[UIAlertAction actionWithTitle:@"Отмена" style:UIAlertActionStyleCancel handler:nil]];
    __weak TouchControlsView *me = self;
    [a addAction:[UIAlertAction actionWithTitle:@"Сбросить всё" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *x) { [me resetLayout]; }]];
    [self.window.rootViewController presentViewController:a animated:YES completion:nil];
}
- (void)resetLayout {
    [prefs() removeObjectForKey:[self layoutKey]];
    [self persist];
    [self select:nil];
}
- (void)editTouchBegan:(CGPoint)p {
    PadButton *b = [self buttonAt:p];
    if (!b || b == close_button) return;
    dragged = b;
    drag_offset = CGPointMake(b.center.x - p.x, b.center.y - p.y);
    [self select:b];
}
- (void)editTouchMoved:(CGPoint)p {
    if (!dragged) return;
    dragged.center = CGPointMake(p.x + drag_offset.x, p.y + drag_offset.y);
    [self storeButton:dragged];
    [self restyle];
}
- (void)editTouchEnded {
    if (dragged) [self persist];
    dragged = nil;
}
- (UIView *)hitTest:(CGPoint)p withEvent:(UIEvent *)e {
    if (self.editing && toolbar && !toolbar.hidden && CGRectContainsPoint(toolbar.frame, p)) return [super hitTest:p withEvent:e];
    return self.hidden ? nil : self;
}
@end

// ---------- the settings ----------
@interface SettingsView : UIView
@end
@implementation SettingsView {
    UIScrollView *scroll;
    UIStackView *stack;
}
- (UISegmentedControl *)segments:(NSArray *)items value:(NSInteger)v action:(SEL)a {
    UISegmentedControl *s = [[UISegmentedControl alloc] initWithItems:items];
    s.selectedSegmentIndex = v;
    [s addTarget:self action:a forControlEvents:UIControlEventValueChanged];
    return s;
}
- (UISlider *)slider:(float)min max:(float)max value:(float)v action:(SEL)a {
    UISlider *s = [UISlider new];
    s.minimumValue = min;
    s.maximumValue = max;
    s.value = v;
    [s addTarget:self action:a forControlEvents:UIControlEventValueChanged];
    return s;
}
- (UISwitch *)toggle:(BOOL)on action:(SEL)a {
    UISwitch *s = [UISwitch new];
    s.on = on;
    [s addTarget:self action:a forControlEvents:UIControlEventValueChanged];
    return s;
}
- (void)row:(NSString *)title control:(UIView *)c {
    UILabel *l = [UILabel new];
    l.text = title;
    l.textColor = UIColor.whiteColor;
    l.font = [UIFont systemFontOfSize:15];
    [l setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    UIStackView *row = [[UIStackView alloc] initWithArrangedSubviews:@[l, c]];
    row.axis = UILayoutConstraintAxisHorizontal;
    row.spacing = 16;
    row.alignment = UIStackViewAlignmentCenter;
    [stack addArrangedSubview:row];
}
- (UIButton *)action:(NSString *)title selector:(SEL)a {
    UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
    [b setTitle:title forState:UIControlStateNormal];
    b.titleLabel.font = [UIFont systemFontOfSize:17 weight:UIFontWeightSemibold];
    [b addTarget:self action:a forControlEvents:UIControlEventTouchUpInside];
    return b;
}
- (instancetype)initWithFrame:(CGRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    UIVisualEffectView *blur = [[UIVisualEffectView alloc] initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemUltraThinMaterialDark]];
    blur.frame = self.bounds;
    blur.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [self addSubview:blur];
    scroll = [UIScrollView new];
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    [self addSubview:scroll];
    stack = [UIStackView new];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 14;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [scroll addSubview:stack];
    NSLayoutConstraint *wide = [scroll.widthAnchor constraintEqualToConstant:520];
    wide.priority = UILayoutPriorityDefaultHigh;  // narrower on a narrow screen
    [NSLayoutConstraint activateConstraints:@[
        [scroll.topAnchor constraintEqualToAnchor:self.safeAreaLayoutGuide.topAnchor constant:12],
        [scroll.bottomAnchor constraintEqualToAnchor:self.safeAreaLayoutGuide.bottomAnchor constant:-12],
        [scroll.centerXAnchor constraintEqualToAnchor:self.centerXAnchor],
        wide,
        [scroll.widthAnchor constraintLessThanOrEqualToAnchor:self.safeAreaLayoutGuide.widthAnchor constant:-24],
        [stack.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor],
        [stack.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor],
        [stack.leadingAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.leadingAnchor],
        [stack.trailingAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.trailingAnchor],
    ]];
    UILabel *title = [UILabel new];
    title.text = @"Настройки";
    title.textColor = UIColor.whiteColor;
    title.font = [UIFont systemFontOfSize:22 weight:UIFontWeightBold];
    [stack addArrangedSubview:title];

    double cap = setting(@"fpsCap", "ARKHAM_FPS_CAP", 0);
    NSInteger capIndex = cap <= 0 ? 3 : cap <= 30 ? 0 : cap <= 60 ? 1 : 2;
    [self row:@"Ограничение FPS" control:[self segments:@[@"30", @"60", @"120", @"Без"] value:capIndex action:@selector(capChanged:)]];
    [self row:@"Кнопки" control:[self segments:@[@"PlayStation", @"Xbox", @"Иконки"] value:(NSInteger)setting(@"buttonStyle", "ARKHAM_TOUCH_STYLE", STYLE_PS)
                                         action:@selector(styleChanged:)]];
    [self row:@"Размер кнопок" control:[self slider:0.7 max:1.5 value:setting(@"buttonScale", "ARKHAM_TOUCH_SIZE", 1.0) action:@selector(sizeChanged:)]];
    [self row:@"Прозрачность" control:[self slider:0.2 max:1.0 value:setting(@"opacity", "ARKHAM_TOUCH_OPACITY", 0.6) action:@selector(opacityChanged:)]];
    [self row:@"Скорость камеры" control:[self slider:0.25 max:4.0 value:setting(@"lookSpeed2", "ARKHAM_TOUCH_LOOK", 1.0) action:@selector(lookChanged:)]];
    [self row:@"Бег: стик за край" control:[self toggle:setting(@"autoRun", "ARKHAM_TOUCH_RUN", 1) > 0 action:@selector(runChanged:)]];
    [self row:@"Прятать кнопки вне игры" control:[self toggle:setting(@"hideInMenus", "ARKHAM_TOUCH_HIDE", 1) > 0 action:@selector(hideChanged:)]];
    [self row:@"Строка FPS и нагрева" control:[self toggle:setting(@"showFPS", "ARKHAM_FPS", 1) > 0 action:@selector(fpsLineChanged:)]];
    [self row:@"Metal HUD" control:[self toggle:setting(@"metalHUD", "ARKHAM_METAL_HUD", 0) > 0 action:@selector(hudChanged:)]];
    [stack addArrangedSubview:[self action:@"Расположение кнопок…" selector:@selector(editLayout)]];
    [stack addArrangedSubview:[self action:@"Готово" selector:@selector(close)]];
    return self;
}
- (void)capChanged:(UISegmentedControl *)s {
    static const double caps[] = {30, 60, 120, 0};
    set_setting(@"fpsCap", caps[s.selectedSegmentIndex]);
    rt_set_fps_cap(caps[s.selectedSegmentIndex]);
}
- (void)styleChanged:(UISegmentedControl *)s {
    set_setting(@"buttonStyle", s.selectedSegmentIndex);
    [controls restyle];
}
- (void)sizeChanged:(UISlider *)s {
    set_setting(@"buttonScale", s.value);
    [controls restyle];
}
- (void)opacityChanged:(UISlider *)s {
    set_setting(@"opacity", s.value);
    [controls restyle];
}
- (void)lookChanged:(UISlider *)s { set_setting(@"lookSpeed2", s.value); }
- (void)runChanged:(UISwitch *)s { set_setting(@"autoRun", s.on); }
- (void)hideChanged:(UISwitch *)s { set_setting(@"hideInMenus", s.on); }
- (void)fpsLineChanged:(UISwitch *)s {
    set_setting(@"showFPS", s.on);
    rt_ios_overlay_enable(s.on);
}
- (void)hudChanged:(UISwitch *)s {
    set_setting(@"metalHUD", s.on);
    rt_set_metal_hud(s.on);
}
- (void)editLayout {
    [self close];
    controls.hidden = NO;
    [controls startEditing];
}
- (void)close { [self removeFromSuperview]; }
@end

static UIView *game_view;
static void open_settings(void) {
    [controls releaseAll];
    SettingsView *s = [[SettingsView alloc] initWithFrame:game_view.bounds];
    [game_view addSubview:s];
}

// ---------- putting it together ----------
static void update_visibility(void) {
    BOOL pad = rt_controller_connected();
    if (pad && !controls.hidden) [controls releaseAll];
    if (!controls.editing) controls.hidden = pad;
    rt_touch_pad_on(!pad);
    fprintf(stderr, "[touch] %s\n", pad ? "a controller is connected: touch controls hidden" : "touch controls shown");
}
// What the settings say, applied once at start: the frame rate limit, the HUD and the frame rate line.
void rt_ios_apply_settings(void) {
    rt_set_fps_cap(setting(@"fpsCap", "ARKHAM_FPS_CAP", 0));
    rt_set_metal_hud(setting(@"metalHUD", "ARKHAM_METAL_HUD", 0) > 0);
    rt_ios_overlay_enable(setting(@"showFPS", "ARKHAM_FPS", 1) > 0);
}
// The gear for the settings: a view of its own, there with a controller connected too, when the touch controls hide.
@interface GearButton : UIButton
@end
@implementation GearButton
- (void)layoutSubviews {
    [super layoutSubviews];
    UIView *s = self.superview;
    UIEdgeInsets in = s.safeAreaInsets;
    self.center = CGPointMake(s.bounds.size.width - MAX(in.right, 8) - 34, MAX(in.top, 8) + 28);
}
@end
static void gear_tapped(void) { open_settings(); }
@interface GearTarget : NSObject
@end
@implementation GearTarget
- (void)tapped { gear_tapped(); }
@end
static GearTarget *gear_target;

void rt_ios_touch_controls(UIView *over) {
    game_view = over;
    const char *off = getenv("ARKHAM_TOUCH");
    if (!off || *off != '0') {
        controls = [[TouchControlsView alloc] initWithFrame:over.bounds];
        controls.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        [over addSubview:controls];
    }
    GearButton *gear = [GearButton buttonWithType:UIButtonTypeCustom];
    gear.frame = CGRectMake(0, 0, 44, 44);
    [gear setImage:symbol(@"gearshape.fill", 20, [UIColor colorWithWhite:1 alpha:0.7]) forState:UIControlStateNormal];
    gear.backgroundColor = [UIColor colorWithWhite:0 alpha:0.3];
    gear.layer.cornerRadius = 22;
    gear_target = [GearTarget new];
    [gear addTarget:gear_target action:@selector(tapped) forControlEvents:UIControlEventTouchUpInside];
    [over addSubview:gear];
    if (!controls) return;
    for (NSNotificationName n in @[GCControllerDidConnectNotification, GCControllerDidDisconnectNotification])
        [NSNotificationCenter.defaultCenter addObserverForName:n object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *note) { update_visibility(); }];
    update_visibility();
}
