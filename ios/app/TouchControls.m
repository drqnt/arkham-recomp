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
#import <objc/message.h>
#include "BatIcons.h"
#include "BatHalo.h"

void rt_touch_pad(float lx, float ly, float rx, float ry, float lt, float rt, unsigned buttons);
void rt_touch_mouse(double dx, double dy);  // shims_sys.c: the pointer's motion in pixels, which the game reads as its mouse's
void rt_touch_move(float x, float y, int in_play);  // shims_sys.c: the walking stick's position, up and right positive
void rt_touch_pad_on(int on);
void rt_touch_pad_playstation(int ps);
int rt_controller_connected(void);
int rt_game_holds_cursor(void);
unsigned rt_frames_presented(void), rt_last_frame_draws(void);
void rt_ios_click(double share_x, double share_y);
void rt_ios_park_cursor(void);
void rt_set_fps_cap(double n);
void rt_set_metal_hud(int on);
void rt_set_metalfx(int on);  // mtl_device.m: the frame up to the screen's pixels through MetalFX
extern int rt_ios_hud_at_launch;  // main.m: whether Apple's HUD was on for this process from its launch, which it needs to show at all
void rt_set_async_pipelines(int on);  // mtl_device.m: new pipelines built off the game's thread
void rt_ios_overlay_enable(int on);  // main.m: the frame rate line
void rt_ios_overlay_hidden(int hidden);
CGRect rt_ios_game_rect(void);       // main.m: where the game's picture is

// The runtime's numbers for the controls (the enum in shims_hid.m), and the wheel's own.
enum { C_LT = 4, C_RT = 5, B_SOUTH = 7, B_EAST, B_WEST, B_NORTH, B_LB, B_RB, B_START, B_BACK, B_L3, B_R3, B_HOME, D_UP, D_DOWN, D_LEFT, D_RIGHT, C_WHEEL = 100, C_PHOTO, C_TWOSTICKS, C_COMBO_TD, C_COMBO_THROW, C_COMBO_GROUND };
enum { STYLE_PS, STYLE_XBOX, STYLE_ICONS };
enum { SH_NONE, SH_CROSS, SH_CIRCLE, SH_SQUARE, SH_TRIANGLE };
enum { SIDE_UP, SIDE_RIGHT, SIDE_DOWN, SIDE_LEFT };

// What a point of swipe turns the camera, at speed 1: a swipe across the phone's 951 points turns it about 270 degrees.
#define CAMERA_DEG_PER_POINT 0.284
// What a pixel of the game's mouse turns it, as the phone reads its settings (shims_libc.c: MouseSensitivity 7.5, no smoothing): 7.5 of the
// engine's rotation units, 65536 to a turn. A scripted run on the Mac measured the same, at any speed (docs/IOS_PORT.md, controls v7).
#define CAMERA_DEG_PER_PIXEL (7.5 * 360.0 / 65536.0)

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
    // Under the right thumb: the arc of four, then grapple and crouch further along it. Upright, the four are a diamond in the corner, with room above
    // and beside it for the camera's swipes, and the shoulders on top. The icons are what each does in the game (DefaultInput.ini's bindings).
    {"cross", B_SOUTH, {BR, -86, -66, 34}, {BR, -160, -80, 40}, "", "A", "figure.run", "Бег, действие, планирование, уклонение", SH_CROSS, 0x7FB2E5, 0x6DB33F},
    {"square", B_WEST, {BR, -184, -60, 36}, {BR, -245, -165, 40}, "", "X", "figure.boxing", "Удар, тихое добивание", SH_SQUARE, 0xE48AD6, 0x3C8FE0},
    {"triangle", B_NORTH, {BR, -140, -154, 34}, {BR, -160, -250, 38}, "", "Y", "bolt.shield.fill", "Контратака, захват", SH_TRIANGLE, 0x5CC49A, 0xF5C518},
    {"circle", B_EAST, {BR, -50, -164, 30}, {BR, -75, -165, 38}, "", "B", "tornado", "Плащ, отмена, пропуск сцены", SH_CIRCLE, 0xE86A6A, 0xE03A3E},
    {"r1", B_RB, {BR, -86, -258, 27}, {TR, -76, 55, 33}, "R1", "RB", "figure.climbing", "Крюк", SH_NONE, 0xFFFFFF, 0xFFFFFF},
    {"r2", C_RT, {BR, -236, -158, 28}, {TR, -185, 55, 33}, "R2", "RT", "arrow.down.to.line", "Присесть; с прицелом — бросок", SH_NONE, 0xFFFFFF, 0xFFFFFF},
    {"r3", B_R3, {BR, -200, -250, 21}, {BR, -45, -45, 26}, "R3", "RS", "plus.magnifyingglass", "Зум камеры", SH_NONE, 0xFFFFFF, 0xFFFFFF},
    // Above the left thumb's stick.
    {"l1", B_LB, {BL, 68, -262, 28}, {TL, 76, 55, 33}, "L1", "LB", "eye.fill", "Режим детектива", SH_NONE, 0xFFFFFF, 0xFFFFFF},
    {"l2", C_LT, {BL, 166, -276, 28}, {TL, 185, 55, 33}, "L2", "LT", "scope", "Прицел гаджета", SH_NONE, 0xFFFFFF, 0xFFFFFF},
    {"l3", B_L3, {BL, 60, -350, 20}, {TL, 60, 153, 26}, "L3", "LS", "camera.rotate.fill", "Камера за спину", SH_NONE, 0xFFFFFF, 0xFFFFFF},
    // The gadgets: one button for the pad's cross, a wheel of its four sides. Drawn as a batarang.
    {"gadgets", C_WHEEL, {TL, 124, 144, 30}, {BC, 0, -77, 37}, "", "", "batarang", "Гаджеты: зажмите и ведите в сторону", SH_NONE, 0xFFFFFF, 0xFFFFFF},
    // The top: the map and the pause, and, at the top left, hiding all of them for a picture.
    {"map", B_BACK, {TC, -64, 28, 20}, {TC, -70, 55, 24}, "", "", "map.fill", "Карта", SH_NONE, 0xFFFFFF, 0xFFFFFF},
    {"pause", B_START, {TC, 64, 28, 20}, {TC, 70, 55, 24}, "", "", "pause.fill", "Пауза", SH_NONE, 0xFFFFFF, 0xFFFFFF},
    {"photo", C_PHOTO, {TL, 30, 26, 19}, {TC, 0, 55, 22}, "", "", "eye.slash.fill", "Скрыть все кнопки (для скриншотов); вернуть — касание экрана", SH_NONE, 0xFFFFFF, 0xFFFFFF},
    // Both sticks at once, for the cryptographic sequencer, whose tuning takes the pad's left and right sticks together: while it's on, the stick goes to
    // the game as the pad's left one, and a touch where the camera's swipe would be is the pad's right stick. Below the photo button.
    {"twosticks", C_TWOSTICKS, {TL, 30, 74, 19}, {TC, 0, 120, 22}, "", "", "gamecontroller.fill", "Два стика (взлом шифров): слева левый стик геймпада, справа правый; выключить — та же кнопка", SH_NONE, 0xFFFFFF, 0xFFFFFF},
    // A second throw, on the left, shown only while a gadget is aimed (L2 held): with everything under the right thumb, holding the aim leaves no
    // finger for R2 (the owner, 2026-11-08). The pad's R2 like the other; drawn as the batarang, as R2 is while aiming.
    {"r2aim", C_RT, {BL, 120, -170, 34}, {BL, 75, -215, 38}, "R2", "RT", "batarang", "Бросок слева (видна только при прицеле)", SH_NONE, 0xFFFFFF, 0xFFFFFF},
    // The moves that take two buttons at once, one touch each, since one thumb can't press two (the owner, 2026-11-08): Special Combo
    // Takedown (triangle and circle, after a combo of 8), Special Combo Throw (circle and cross), Ground Takedown (R2 and triangle).
    {"combo_td", C_COMBO_TD, {BR, -290, -50, 24}, {TR, -185, 125, 24}, "△○", "Y+B", "bolt.fill", "Особое добивание (△ и ○ вместе), после серии ×8", SH_NONE, 0xFFFFFF, 0xFFFFFF},
    {"combo_throw", C_COMBO_THROW, {BR, -345, -50, 24}, {TR, -130, 125, 24}, "○✕", "B+A", "arrow.uturn.down", "Особый бросок (○ и ✕ вместе), после серии ×8", SH_NONE, 0xFFFFFF, 0xFFFFFF},
    {"combo_ground", C_COMBO_GROUND, {BR, -290, -105, 24}, {TR, -75, 125, 24}, "R2△", "RT+Y", "arrow.down.to.line", "Добивание лежачего (R2 и △ вместе)", SH_NONE, 0xFFFFFF, 0xFFFFFF},
};
#define N_DEFS (sizeof defs / sizeof defs[0])
// Outside play the only one: backs out of menus, closes the map, skips a cutscene. It stays at the top left, as the gear does at the right.
static const Def close_def = {"close", B_EAST, {TL, 0, 0, 22}, {TL, 0, 0, 22}, "", "", "xmark", "Назад", SH_NONE, 0xFFFFFF, 0xFFFFFF};
// On the map only: the game's "Show Batman", the pad's square there (its hints, 2026-11-08). At the bottom right, under the thumb.
static const Def findbat_def = {"findbat", B_WEST, {BR, 0, 0, 30}, {BR, 0, 0, 30}, "", "X", "location.fill", "Показать Бэтмена", SH_SQUARE, 0xE48AD6, 0x3C8FE0};
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

// The second throw button, shown only while a gadget is aimed.
static BOOL aim_only(const Def *d) { return !strcmp(d->ident, "r2aim"); }

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
// The bat: a batarang for the gadgets, and on R2 while aiming, where the game throws one. Wings with three scallops below each, ears, a pointed tail;
// s is half its width.
static UIBezierPath *bat_shape(CGPoint c, CGFloat s) {
    CGMutablePathRef p = CGPathCreateMutable();
    bat_path_add(p, c.x, c.y, s);
    UIBezierPath *b = [UIBezierPath bezierPathWithCGPath:p];
    CGPathRelease(p);
    return b;
}
// A button's own icon (BatIcons.h), white on clear, w points across; nil for a button without one. Kept, since a size comes back on every restyle.
static UIImage *bat_icon(const char *ident, CGFloat w) {
    static NSCache *made;
    if (!made) made = [NSCache new];
    NSString *key = [NSString stringWithFormat:@"%s@%.1f", ident, w];
    UIImage *img = [made objectForKey:key];
    if (img) return img;
    __block int drawn = 0;
    img = [[[UIGraphicsImageRenderer alloc] initWithSize:CGSizeMake(w, w)] imageWithActions:^(UIGraphicsImageRendererContext *ctx) {
        drawn = bat_icon_draw(ctx.CGContext, ident, w);
    }];
    if (!drawn) return nil;
    [made setObject:img forKey:key];
    return img;
}
// A soft dark ring around a button's icon, the icon's own shape left clear, pad points wider on each side. It lies under the white icon.
// It lets the icon read over a light picture, and over a dark one it doesn't show.
static UIImage *bat_icon_halo(const char *ident, CGFloat w, CGFloat pad) {
    static NSCache *made;
    if (!made) made = [NSCache new];
    NSString *key = [NSString stringWithFormat:@"%s@%.1f/%.1f", ident, w, pad];
    UIImage *img = [made objectForKey:key];
    if (img) return img;
    __block int drawn = 0;
    CGFloat side = w + pad * 2;
    img = [[[UIGraphicsImageRenderer alloc] initWithSize:CGSizeMake(side, side)] imageWithActions:^(UIGraphicsImageRendererContext *ctx) {
        drawn = bat_icon_halo_draw(ctx.CGContext, ident, w, pad);
    }];
    if (!drawn) return nil;
    [made setObject:img forKey:key];
    return img;
}
static UIImage *symbol(NSString *name, CGFloat size, UIColor *color) {
    UIImageSymbolConfiguration *cfg = [UIImageSymbolConfiguration configurationWithPointSize:size weight:UIImageSymbolWeightSemibold];
    UIImage *img = [UIImage systemImageNamed:name withConfiguration:cfg];
    if (!img && [name isEqualToString:@"dpad.fill"]) img = [UIImage systemImageNamed:@"plus.circle.fill" withConfiguration:cfg];
    return [img imageWithTintColor:color renderingMode:UIImageRenderingModeAlwaysOriginal];
}

@interface PadButton : NSObject
@property(nonatomic) const Def *def;
@property(nonatomic) CGPoint center;
@property(nonatomic) CGFloat radius;
@property(nonatomic, strong) CALayer *group;
// The badge in the corner (the PlayStation symbol or name) sits in a layer of its own over the button, at the button's opacity.
@property(nonatomic, strong) CALayer *badge_group;
@property(nonatomic, strong) CAShapeLayer *badge_symbol;
@property(nonatomic, strong) CAShapeLayer *back, *symbol, *badge;
@property(nonatomic, strong) CATextLayer *text, *badge_text;
@property(nonatomic, strong) CALayer *icon;
@property(nonatomic, strong) CALayer *icon_halo;  // a soft dark ring under the icon (bat_icon_halo)
@property(nonatomic) int holders;
@property(nonatomic) BOOL shown;           // drawn and taking touches now
@property(nonatomic) BOOL hiddenByPlayer;  // hidden in the layout editor
@property(nonatomic, strong) CALayer *icon_mask;  // the icon's shape; the icon layer's own colour shows through it
@property(nonatomic) int kind;                    // what's drawn: one of the KIND_ below
@property(nonatomic) int tint;                    // the colour of a PlayStation symbol or an Xbox letter, or 0 for the button's ink
@property(nonatomic) float ink, ink_shown;        // 0, white lines; what it was last drawn with (ink_color can also draw dark lines, which nothing asks for now)
@end
enum { KIND_PS_SHAPE, KIND_TEXT, KIND_ICON, KIND_BAT };
@implementation PadButton
@end

@interface TouchControlsView : UIView
@property(nonatomic) BOOL editing;
- (void)restyle;
- (void)releaseAll;
- (void)startEditing;
- (void)followFrameRateLimit;
@end
static TouchControlsView *controls;
static void open_settings(void);

@implementation TouchControlsView {
    NSMutableArray<PadButton *> *buttons;
    PadButton *close_button;
    PadButton *findbat_button;  // on the map only
    // The map: opened with the map button in play, over when play comes back or the back button closes it.
    // There a drag moves the map as far as the finger went, and two fingers zoom it.
    BOOL map_open;
    UITouch *pinch_touch;   // the second finger on the map
    CGPoint pinch_point;    // where it is
    CGFloat pinch_last;     // the two fingers' distance at the last display frame
    double zoom_debt;       // the pinch's zoom still to make, as the logarithm of its scale: more than 0 in, less than 0 out
    CGPoint pan_debt;       // how far the map has still to go to have followed the finger, in points
    float zoom_lt, zoom_rt; // the triggers that zoom it this frame
    CFTimeInterval map_opened_at;
    NSMapTable<UITouch *, id> *owners;
    NSMapTable<UITouch *, NSValue *> *tap_start;
    NSMapTable<UITouch *, NSNumber *> *tap_time;
    UITouch *stick_touch, *look_touch, *pan_touch, *wheel_touch;
    CGPoint stick_center, look_last, pan_last, pan_sum;
    float lx, ly, rx, ry;
    BOOL running;  // the stick pushed past its edge
    BOOL panning;  // the touch outside play has moved far enough to be a drag
    BOOL in_menu;  // outside play
    BOOL seen_play, photo;
    CFTimeInterval last_frame, last_play, heavy_until;
    unsigned play_frame;
    CGSize laid_out;
    UILabel *photo_hint;
    float stick_ink;
    unsigned pulse_bits;  // the wheel's choice, held for a moment
    CFTimeInterval pulse_until;
    CAShapeLayer *stick_base, *stick_knob;
    // Two sticks (the cryptographic sequencer): the second one, where the camera's swipe would be, as the pad's right stick.
    BOOL twosticks;
    UITouch *rstick_touch;
    CGPoint rstick_center;
    CAShapeLayer *rstick_base, *rstick_knob;
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
    b.icon_mask = [CALayer layer];
    b.icon_mask.contentsGravity = kCAGravityResizeAspect;
    b.icon.mask = b.icon_mask;
    for (CATextLayer *t in @[b.text, b.badge_text]) {
        t.alignmentMode = kCAAlignmentCenter;
        t.contentsScale = UIScreen.mainScreen.scale;
    }
    b.icon_halo = [CALayer layer];
    b.icon_halo.contentsGravity = kCAGravityResizeAspect;
    for (CALayer *l in @[b.back, b.symbol, b.text, b.icon_halo, b.icon]) [b.group addSublayer:l];
    [self.layer addSublayer:b.group];
    // The badge's symbol goes over its dark disc. It used to share the button's symbol layer, which lay under the disc, so only a trace of it showed through.
    b.badge_group = [CALayer layer];
    b.badge_symbol = [CAShapeLayer layer];
    for (CALayer *l in @[b.badge, b.badge_symbol, b.badge_text]) [b.badge_group addSublayer:l];
    [self.layer addSublayer:b.badge_group];
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
    findbat_button = [self makeButton:&findbat_def];
    stick_base = [CAShapeLayer layer];
    stick_knob = [CAShapeLayer layer];
    rstick_base = [CAShapeLayer layer];
    rstick_knob = [CAShapeLayer layer];
    for (CAShapeLayer *l in @[stick_base, stick_knob, rstick_base, rstick_knob]) {
        l.hidden = YES;
        [self.layer addSublayer:l];
    }
    stick_base.lineWidth = rstick_base.lineWidth = 2;
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
    in_menu = YES;  // until play is seen: the films and loading at the start are outside it
    photo_hint = [UILabel new];
    photo_hint.textColor = UIColor.whiteColor;
    photo_hint.font = [UIFont systemFontOfSize:15 weight:UIFontWeightMedium];
    photo_hint.backgroundColor = [UIColor colorWithWhite:0 alpha:0.6];
    photo_hint.numberOfLines = 0;
    photo_hint.textAlignment = NSTextAlignmentCenter;
    photo_hint.layer.cornerRadius = 10;
    photo_hint.clipsToBounds = YES;
    photo_hint.alpha = 0;
    [self addSubview:photo_hint];
    link = [CADisplayLink displayLinkWithTarget:self selector:@selector(frame:)];
    [self followFrameRateLimit];
    [link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    fprintf(stderr, "[touch] camera: %.0f degrees across the screen's width (speed %.2f)%s\n",
            setting(@"mouseCameraSpeed", "ARKHAM_TOUCH_LOOK", 1.0) * CAMERA_DEG_PER_POINT * MAX(frame.size.width, frame.size.height), setting(@"mouseCameraSpeed", "ARKHAM_TOUCH_LOOK", 1.0),
            setting(@"invertLook", "ARKHAM_TOUCH_INVERT", 0) > 0 ? ", up and down inverted" : "");
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
    if (b == findbat_button) {
        b.radius = 30;
        b.center = CGPointMake(vs.width - MAX(in.right, 8) - 48, vs.height - MAX(in.bottom, 8) - 44);
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
    for (PadButton *b in [self allButtons]) b.group.frame = b.badge_group.frame = self.bounds;
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
// Each button is drawn in two steps: its shape and what's on it (drawButton), and its colours (colorButton).
// It is white lines on a dark disc. A soft dark ring round the icon and a shadow round the disc let it read over a light picture too.
// (v14 to v16 could also follow the picture, dark over light; the user found that unreadable on white and it is gone.)
- (void)drawButton:(PadButton *)b style:(int)style {
    const Def *d = b.def;
    CGPoint c = b.center;
    CGFloat r = b.radius;
    b.back.path = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(c.x - r, c.y - r, r * 2, r * 2)].CGPath;
    b.back.lineWidth = self.editing && b == selected ? 3 : MAX(1.5, r * 0.05);
    b.back.lineDashPattern = self.editing ? @[@6, @4] : nil;
    b.symbol.hidden = b.text.hidden = b.icon.hidden = b.icon_halo.hidden = b.badge.hidden = b.badge_symbol.hidden = b.badge_text.hidden = YES;
    // A soft shadow round the disc, so that its rim shows over a light picture too.
    b.back.shadowPath = b.back.path;
    b.back.shadowColor = UIColor.blackColor.CGColor;
    b.back.shadowOffset = CGSizeZero;
    b.back.shadowRadius = MAX(2, r * 0.08);
    b.back.shadowOpacity = 0.45;
    b.tint = 0;
    BOOL aiming = NO;
    for (PadButton *o in buttons)
        if (o.def->control == C_LT && o.holders > 0) aiming = YES;
    BOOL bat = !strcmp(d->icon, "batarang") || (d->control == C_RT && aiming);
    BOOL letters = !bat && style != STYLE_ICONS && (d->shape != SH_NONE || *(style == STYLE_XBOX ? d->xbox : d->ps));
    if (bat) {
        // The batarang: the gadgets, and R2 while L2 aims, since that's where the game throws one.
        b.kind = KIND_BAT;
        b.symbol.hidden = NO;
        b.symbol.path = bat_shape(CGPointMake(c.x, c.y + r * 0.04), r * 0.66).CGPath;
        b.symbol.lineWidth = 0;
    } else if (letters && style == STYLE_PS && d->shape != SH_NONE) {
        // The PlayStation symbol, drawn in its colour.
        b.kind = KIND_PS_SHAPE;
        b.tint = d->ps_color;
        b.symbol.hidden = NO;
        b.symbol.path = ps_shape(d->shape, c, r * 0.42).CGPath;
        b.symbol.lineWidth = MAX(2.5, r * 0.11);
    } else if (letters) {
        const char *label = style == STYLE_XBOX ? d->xbox : d->ps;
        b.kind = KIND_TEXT;
        b.tint = style == STYLE_XBOX && d->xbox_color != 0xFFFFFF ? d->xbox_color : 0;
        CGFloat font = strlen(label) > 1 ? r * 0.62 : r * 0.9;
        b.text.hidden = NO;
        b.text.string = @(label);
        b.text.fontSize = font;
        b.text.font = (__bridge CFTypeRef)[UIFont systemFontOfSize:font weight:UIFontWeightBold];
        b.text.frame = CGRectMake(c.x - r, c.y - font * 0.62, r * 2, font * 1.3);
    } else {
        // An icon of what the button does, with the PlayStation symbol or name it is in a corner, to match the game's prompts. The icon's shape is
        // a mask over a layer of the button's colour, so that changing that colour is cheap.
        b.kind = KIND_ICON;
        CGFloat w = r * 1.15;
        UIImage *img = bat_icon(d->ident, w) ?: symbol(@(d->icon), r * 0.85, UIColor.whiteColor);
        if (img) {
            b.icon.hidden = NO;
            b.icon.frame = CGRectMake(c.x - w / 2, c.y - w / 2, w, w);
            b.icon_mask.frame = b.icon.bounds;
            b.icon_mask.contents = (__bridge id)img.CGImage;
            CGFloat pad = MAX(3, w * 0.09);
            UIImage *halo = bat_icon_halo(d->ident, w, pad);
            if (halo) {
                b.icon_halo.hidden = NO;
                b.icon_halo.frame = CGRectMake(c.x - w / 2 - pad, c.y - w / 2 - pad, w + pad * 2, w + pad * 2);
                b.icon_halo.contents = (__bridge id)halo.CGImage;
                b.icon_halo.opacity = 0.75;
            }
        }
        if (style == STYLE_ICONS && (d->shape != SH_NONE || *d->ps)) {
            // A dark disc with a light rim: the rim shows it on a dark picture, the disc on a light one.
            CGPoint bc = CGPointMake(c.x + r * 0.72, c.y + r * 0.72);
            CGFloat br = MAX(10, r * 0.36);
            b.badge.hidden = NO;
            b.badge.path = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(bc.x - br, bc.y - br, br * 2, br * 2)].CGPath;
            b.badge.fillColor = [UIColor colorWithWhite:0 alpha:0.85].CGColor;
            b.badge.strokeColor = [UIColor colorWithWhite:1 alpha:0.6].CGColor;
            b.badge.lineWidth = 1;
            if (d->shape != SH_NONE) {
                b.badge_symbol.hidden = NO;
                b.badge_symbol.path = ps_shape(d->shape, bc, br * 0.5).CGPath;
                b.badge_symbol.fillColor = UIColor.clearColor.CGColor;
                b.badge_symbol.strokeColor = rgb(d->ps_color, 1).CGColor;
                b.badge_symbol.lineWidth = MAX(2, br * 0.18);
                b.badge_symbol.lineCap = kCALineCapRound;
                b.badge_symbol.lineJoin = kCALineJoinRound;
            } else {
                NSString *badge = @(d->ps);  // in characters: the combos' symbols take several bytes each
                CGFloat font = badge.length > 2 ? br * 0.72 : badge.length > 1 ? br * 0.95 : br * 1.1;
                b.badge_text.hidden = NO;
                b.badge_text.string = badge;
                b.badge_text.fontSize = font;
                b.badge_text.font = (__bridge CFTypeRef)[UIFont systemFontOfSize:font weight:UIFontWeightHeavy];
                b.badge_text.foregroundColor = UIColor.whiteColor.CGColor;
                b.badge_text.frame = CGRectMake(bc.x - br, bc.y - font * 0.62, br * 2, font * 1.3);
            }
        }
    }
}
// A grey from white (ink 0) to black (ink 1).
static UIColor *ink_color(float ink, CGFloat alpha) { return [UIColor colorWithWhite:1 - ink alpha:alpha]; }
// How much of a button shows: the opposite of the settings' "Прозрачность кнопок", as the whole button's opacity, all of it while pressed, and the close button never faint.
- (CGFloat)opacityOf:(PadButton *)b {
    if (self.editing) return b.hiddenByPlayer ? 0.35 : 1;
    CGFloat o = setting(@"opacity", "ARKHAM_TOUCH_OPACITY", 0.8);
    if (b == close_button || b == findbat_button) o = MAX(0.7, o);
    return b.holders > 0 ? MAX(o, 0.95) : o;
}
// The pad's buttons, then the two shown only outside play: back, and on the map, Show Batman.
- (NSArray<PadButton *> *)allButtons { return [buttons arrayByAddingObjectsFromArray:@[close_button, findbat_button]]; }
- (void)colorButton:(PadButton *)b {
    BOOL down = b.holders > 0;
    float ink = b.ink;
    BOOL lit = (b.def->control == C_RT && b.kind == KIND_BAT)     // R2 while aiming: lit up, to show where to throw
               || (b.def->control == C_TWOSTICKS && twosticks);  // the two sticks, while they're on
    UIColor *fg = ink_color(ink, 1);
    b.group.opacity = b.badge_group.opacity = [self opacityOf:b];
    b.back.fillColor = (down ? ink_color(ink, 0.55) : lit && b.def->control == C_TWOSTICKS ? rgb(0x3C8FE0, 0.6) : ink_color(1 - ink, 0.5)).CGColor;
    b.back.strokeColor = (self.editing && b.hiddenByPlayer ? rgb(0xE86A6A, 0.95)
                          : lit                          ? rgb(0x7FB2E5, 1)
                                                         : ink_color(ink, 0.9)).CGColor;
    if (lit) b.back.lineWidth = MAX(3, b.radius * 0.09);
    switch (b.kind) {
        case KIND_PS_SHAPE:
            b.symbol.fillColor = UIColor.clearColor.CGColor;
            b.symbol.strokeColor = rgb(b.tint, 1).CGColor;
            break;
        case KIND_TEXT: b.text.foregroundColor = (b.tint ? rgb(b.tint, 1) : fg).CGColor; break;
        case KIND_ICON: b.icon.backgroundColor = fg.CGColor; break;
        case KIND_BAT:
            b.symbol.fillColor = (lit ? rgb(0x7FB2E5, 1) : fg).CGColor;
            b.symbol.strokeColor = UIColor.clearColor.CGColor;
            break;
    }
    b.ink_shown = ink;
}
// The controls' own display link asks the display for no more than the frame rate limit.
// It used to ask for 120 whatever the limit. After the phone was turned, iOS ran the display at 120 by it: a game at 60 then landed on 8.3 ms
// steps as it fell, and the frames went uneven (v16's log: from 25.5 to 21 ms present to screen, 40 a second off the step, at the turn).
- (void)followFrameRateLimit {
    double cap = setting(@"fpsCap", "ARKHAM_FPS_CAP", 0);
    if (@available(iOS 15.0, *)) {
        float top = cap > 0 && cap < 120 ? (float)cap : 120;
        link.preferredFrameRateRange = CAFrameRateRangeMake(MIN(top, 30), top, top);
    }
}
- (void)restyle {
    int style = (int)setting(@"buttonStyle", "ARKHAM_TOUCH_STYLE", STYLE_PS);
    rt_touch_pad_playstation(style != STYLE_XBOX);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    for (PadButton *b in [self allButtons]) {
        [self placeButton:b];
        BOOL show = b == close_button                                    ? in_menu && !self.editing && !photo
                    : b == findbat_button                                ? in_menu && map_open && !self.editing && !photo
                    : b.def->control == C_TWOSTICKS && twosticks && !self.editing ? !photo  // while on, always there to turn them off
                    : aim_only(b.def) && !self.editing                   ? !in_menu && !photo && !b.hiddenByPlayer && [self aiming]
                                                                         : self.editing || (!in_menu && !photo && !b.hiddenByPlayer);
        b.shown = show;
        b.group.hidden = b.badge_group.hidden = !show;
        if (!show) {
            b.holders = 0;
            continue;
        }
        b.group.opacity = b.badge_group.opacity = [self opacityOf:b];
        [self drawButton:b style:style];
        [self colorButton:b];
    }
    [self colorStick];
    [CATransaction commit];
}
- (void)colorStick {
    CGFloat o = setting(@"opacity", "ARKHAM_TOUCH_OPACITY", 0.8);
    stick_base.opacity = rstick_base.opacity = (float)o;
    stick_knob.opacity = rstick_knob.opacity = (float)MAX(o, 0.9);  // the thumb's own place always shows
    stick_base.fillColor = rstick_base.fillColor = ink_color(1 - stick_ink, 0.44).CGColor;
    stick_base.strokeColor = rstick_base.strokeColor = ink_color(stick_ink, 0.85).CGColor;
    stick_knob.fillColor = (running ? rgb(0x7FB2E5, 0.85) : ink_color(stick_ink, 0.75)).CGColor;
    rstick_knob.fillColor = ink_color(stick_ink, 0.75).CGColor;
}

- (PadButton *)buttonAt:(CGPoint)p {
    PadButton *best = nil;
    CGFloat best_d = 1e9;
    for (PadButton *b in [self allButtons]) {
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
    for (PadButton *b in [self allButtons]) {
        if (!b.holders) continue;
        int k = b.def->control;
        if (k == C_LT) lt = 1;
        else if (k == C_RT) rt = 1;
        else if (k < C_WHEEL) bits |= 1u << k;
        else if (k == C_COMBO_TD) bits |= 1u << B_NORTH | 1u << B_EAST;
        else if (k == C_COMBO_THROW) bits |= 1u << B_EAST | 1u << B_SOUTH;
        else if (k == C_COMBO_GROUND) {
            rt = 1;
            bits |= 1u << B_NORTH;
        }
    }
    if (running) bits |= 1u << B_SOUTH;
    if (pulse_bits && CACurrentMediaTime() < pulse_until) bits |= pulse_bits;
    if (in_menu && map_open) {
        lt = MAX(lt, zoom_lt);  // a pinch on the map: R2 zooms in, L2 out
        rt = MAX(rt, zoom_rt);
    }
    rt_touch_pad(lx, ly, rx, ry, lt, rt, bits);
    // In play walking goes straight to the engine (shims_sys.c); in menus the stick pans, through the pad; with two sticks it is the pad's, everywhere.
    rt_touch_move(lx, ly, !in_menu && !twosticks);
}

- (CGFloat)stickRadius { return 62 * [self unit] / 1.15 * ([self portrait] ? 1.2 : 1); }
- (void)showStick:(BOOL)right at:(CGPoint)c knob:(CGPoint)k {
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    CGFloat R = [self stickRadius], kr = R * 0.42;
    CAShapeLayer *base = right ? rstick_base : stick_base, *knob = right ? rstick_knob : stick_knob;
    base.path = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(c.x - R, c.y - R, R * 2, R * 2)].CGPath;
    knob.path = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(k.x - kr, k.y - kr, kr * 2, kr * 2)].CGPath;
    base.hidden = knob.hidden = photo;
    [self colorStick];
    [CATransaction commit];
}
// A stick's tilt, from its centre to the thumb: up positive, with a dead zone so that a resting thumb doesn't move it; and where its knob is drawn.
- (CGPoint)tiltFrom:(CGPoint)c to:(CGPoint)p knob:(CGPoint *)k {
    CGFloat R = [self stickRadius], dx = p.x - c.x, dy = p.y - c.y, d = hypot(dx, dy), shown = MIN(d, R);
    float m = (float)(shown / R);
    m = m < 0.12f ? 0 : (m - 0.12f) / 0.88f;
    *k = CGPointMake(c.x + (d > 0 ? dx / d * shown : 0), c.y + (d > 0 ? dy / d * shown : 0));
    return CGPointMake(d > 0 ? dx / d * m : 0, d > 0 ? -dy / d * m : 0);
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
// Whether the game is in play: there it warps the cursor back every frame, and a frame takes hundreds of draws. Films and loading screens hold the
// cursor too but take a few dozen; menus let it go. A game whose frames go on without play is outside it; one that has stopped making frames is only
// busy, and stays where it was. Until play has been seen once, as through the films and loading at the start, it's outside.
- (void)updatePlay:(CFTimeInterval)now {
    unsigned frames = rt_frames_presented();
    if (rt_last_frame_draws() >= 200) heavy_until = now + 0.6;
    BOOL play = rt_game_holds_cursor() && now < heavy_until;
    if (play || self.editing || setting(@"hideInMenus", "ARKHAM_TOUCH_HIDE", 1) <= 0) {
        seen_play = YES;
        last_play = now;
        play_frame = frames;
    }
    BOOL menu = !seen_play || (frames - play_frame >= 20 && now - last_play > 0.3);
    // The map button pressed, and no map after all ("MAP NOT AVAILABLE" in some places): the next menu isn't the map.
    if (map_open && !in_menu && !menu && now - map_opened_at > 1.5) map_open = NO;
    if (menu == in_menu) return;
    [self releaseAll];
    in_menu = menu;
    if (menu) {
        rt_ios_park_cursor();
        [self setPhoto:NO];
    } else {
        map_open = NO;
    }
    [self restyle];
    fprintf(stderr, "[touch] %s\n", !menu ? "in play: the pad is back"
                                    : map_open ? "on the map: a drag moves it as far as the finger, two fingers zoom, the corner button shows Batman"
                                               : "outside play: the back button only; taps click, drags are the left stick");
}
// Hiding everything for a picture: the buttons, the gear and the frame rate line. The stick and the camera go on working, unseen; a tap brings them back.
static UIView *gear_view;
- (void)setPhoto:(BOOL)on {
    if (on == photo) return;
    photo = on;
    gear_view.hidden = on;
    rt_ios_overlay_hidden(on);
    if (on) {
        stick_base.hidden = stick_knob.hidden = rstick_base.hidden = rstick_knob.hidden = YES;
        [self flashHint:@"Коснитесь экрана, чтобы вернуть кнопки" for:1.6];
    } else {
        photo_hint.alpha = 0;
    }
    [self restyle];
    fprintf(stderr, "[touch] %s\n", on ? "photo mode: everything hidden" : "photo mode over");
}
// A few words of help over the bottom of the picture, for a moment.
- (void)flashHint:(NSString *)text for:(NSTimeInterval)secs {
    photo_hint.text = text;
    CGFloat most = MIN(self.bounds.size.width - 40, 560);
    CGSize s = [photo_hint sizeThatFits:CGSizeMake(most - 24, CGFLOAT_MAX)];
    photo_hint.bounds = CGRectMake(0, 0, MIN(most, s.width + 24), s.height + 16);
    photo_hint.center = CGPointMake(self.bounds.size.width / 2, CGRectGetMaxY(rt_ios_game_rect()) - 24 - photo_hint.bounds.size.height / 2);
    [photo_hint.layer removeAllAnimations];
    photo_hint.alpha = 1;
    [UIView animateWithDuration:0.6 delay:secs options:0 animations:^{ self->photo_hint.alpha = 0; } completion:nil];
}
// Two sticks, on or off. The camera's swipe and the right stick don't carry over from one to the other; the left stick goes on, without running.
- (void)setTwoSticks:(BOOL)on {
    if (on == twosticks) return;
    twosticks = on;
    look_touch = rstick_touch = nil;
    rx = ry = 0;
    running = NO;
    rstick_base.hidden = rstick_knob.hidden = YES;
    [self restyle];
    [self flashHint:on ? @"Два стика: слева — левый стик геймпада, справа — правый (для взлома шифров). Выключить — та же кнопка"
                       : @"Два стика выключены: справа снова камера"
                    for:on ? 3 : 1.2];
    fprintf(stderr, "[touch] %s\n", on ? "two sticks: the stick is the pad's left one and a touch elsewhere the pad's right one (the cryptographic sequencer)"
                                       : "two sticks off: the swipe turns the camera again");
    [self send];
}
// The map, each display frame. Measured on the Mac (2026-11-08):
// - The pad's left stick moves the map's view: full right moves it right, so the map itself goes left. At full tilt that is about a third of
//   the picture's width a second at the map's first zoom, and twice that zoomed in. Half tilt is about two thirds as fast.
// - L2 zooms out by 0.62 in a quarter of a second at full pull; R2 zooms in, and "Show Batman" (square) leaves it zoomed in all the way.
// A drag adds how far the finger went to what the map has still to do, and the stick is tilted to do that within about a tenth of a second:
// a quick swipe ends where the finger did, a moment later. A pinch does the same with the zoom, pulling a trigger all the way while there is
// some left to do, since the interface may take the triggers as keys.
#define MAP_PAN 0.45   // of the picture's width a second, at full tilt
#define MAP_TAU 0.12   // seconds to catch up with the finger
#define MAP_MIN 0.3    // the least tilt that moves it
#define MAP_ZOOM 1.9   // the logarithm of the zoom a second, at full pull
- (void)mapFrame:(CFTimeInterval)dt {
    CGFloat width = MAX(100, rt_ios_game_rect().size.width), v = MAP_PAN * width;  // points a second at full tilt
    if (pinch_touch && pan_touch) {
        CGFloat d = hypot(pinch_point.x - pan_last.x, pinch_point.y - pan_last.y);
        if (pinch_last > 10 && d > 10) zoom_debt += log(d / pinch_last);
        pinch_last = d;
    } else if (pan_touch && panning) {
        pan_debt.x = MAX(-width * 1.5, MIN(width * 1.5, pan_debt.x + pan_sum.x));
        pan_debt.y = MAX(-width * 1.5, MIN(width * 1.5, pan_debt.y + pan_sum.y));
    }
    pan_sum = CGPointZero;
    // The view goes against the finger, so that the map goes with it. Up is positive on the stick and down on the screen.
    double sx = -pan_debt.x / (v * MAP_TAU), sy = pan_debt.y / (v * MAP_TAU), m = hypot(sx, sy);
    if (m > 1) sx /= m, sy /= m, m = 1;
    if (hypot(pan_debt.x, pan_debt.y) < 3) sx = sy = m = 0;
    else if (m < MAP_MIN) sx *= MAP_MIN / m, sy *= MAP_MIN / m;
    lx = (float)sx;
    ly = (float)sy;
    pan_debt.x += sx * v * dt;  // what the tilt moves this frame comes off what's left: the map goes -sx * v sideways and +sy * v down
    pan_debt.y -= sy * v * dt;
    // The zoom: a trigger all the way while there's more than a little left to do.
    zoom_lt = zoom_rt = 0;
    if (fabs(zoom_debt) > 0.03) {
        if (zoom_debt > 0) zoom_rt = 1;
        else zoom_lt = 1;
        double step = MAP_ZOOM * dt;
        zoom_debt = zoom_debt > 0 ? MAX(0, zoom_debt - step) : MIN(0, zoom_debt + step);
    }
}
// Each display frame: whether the game is in play, the buttons' colours, the map's drag outside play, the wheel's pulse. The camera isn't here: the
// thumb's turning goes to the runtime as it moves (touchesMoved), which hands the game exactly each frame's share (shims_hid.m).
- (void)frame:(CADisplayLink *)l {
    CFTimeInterval now = l.timestamp, dt = last_frame > 0 ? now - last_frame : 1.0 / 60;
    last_frame = now;
    if (dt <= 0 || dt > 0.1) dt = 1.0 / 60;
    [self updatePlay:now];
    if (!rstick_touch) rx = ry = 0;
    if (twosticks) {
        // Both sticks are the thumbs' own, outside play too.
    } else if (in_menu && map_open) {
        [self mapFrame:dt];
    } else if (in_menu) {
        // Elsewhere outside play a drag is the left stick, the other way from the finger, at a tilt from its speed.
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
// Whether a gadget is being aimed: the touch L2 held.
- (BOOL)aiming {
    for (PadButton *b in buttons)
        if (b.def->control == C_LT && b.holders > 0) return YES;
    return NO;
}
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
- (void)startStick:(UITouch *)t at:(CGPoint)p {
    stick_touch = t;
    stick_center = p;
    lx = ly = 0;
    running = NO;
    [owners setObject:@"stick" forKey:t];
    [self showStick:NO at:p knob:p];
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
        if (b && b.def->control == C_PHOTO) {
            [tap impactOccurred];
            [owners setObject:@"photo" forKey:t];
            [self setPhoto:YES];
            continue;
        }
        if (b && b.def->control == C_TWOSTICKS) {
            [tap impactOccurred];
            [owners setObject:@"twosticks" forKey:t];
            [self setTwoSticks:!twosticks];
            continue;
        }
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
            if (b.def->control == B_BACK && !in_menu) {  // the map button in play: what comes next is the map
                map_open = YES;
                map_opened_at = CACurrentMediaTime();
                pan_debt = CGPointZero;
                zoom_debt = 0;
            }
            if (b == close_button && map_open) map_open = NO;  // the back button closes the map
            [owners setObject:b forKey:t];
            b.holders++;
            [self restyle];
            [tap impactOccurred];
            continue;
        }
        [tap_start setObject:[NSValue valueWithCGPoint:p] forKey:t];
        [tap_time setObject:@(t.timestamp) forKey:t];
        if (twosticks) {
            // The stick's side as ever, and anywhere else the right stick, where the thumb lands. Outside play as well, in case the sequencer's
            // screen doesn't look like play here; a tap there still clicks.
            if ([self inStickZone:p] && !stick_touch) {
                [self startStick:t at:p];
            } else if (!rstick_touch) {
                rstick_touch = t;
                rstick_center = p;
                rx = ry = 0;
                [owners setObject:@"rstick" forKey:t];
                [self showStick:YES at:p knob:p];
            }
            continue;
        }
        if (in_menu) {
            if (!pan_touch) {
                pan_touch = t;
                pan_last = p;
                pan_sum = CGPointZero;
                panning = NO;
                [owners setObject:@"pan" forKey:t];
            } else if (map_open && !pinch_touch) {
                // A second finger on the map: the two zoom it, and neither is a tap.
                pinch_touch = t;
                pinch_point = p;
                pinch_last = hypot(p.x - pan_last.x, p.y - pan_last.y);
                pan_debt = CGPointZero;
                [tap_start removeObjectForKey:t];
                [tap_start removeObjectForKey:pan_touch];
                [owners setObject:@"pinch" forKey:t];
            }
            continue;
        }
        if ([self inStickZone:p] && !stick_touch) {
            [self startStick:t at:p];
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
            NSValue *start = [tap_start objectForKey:t];
            CGPoint s0 = start.CGPointValue;
            if (!panning && (!start || hypot(p.x - s0.x, p.y - s0.y) > 14)) panning = YES;
        } else if (t == pinch_touch) {
            pinch_point = p;
        } else if (t == stick_touch) {
            // Past the stick's edge Batman runs, as the pad's cross held; not with two sticks, where the cross isn't wanted.
            BOOL run = !twosticks && setting(@"autoRun", "ARKHAM_TOUCH_RUN", 1) > 0 && hypot(p.x - stick_center.x, p.y - stick_center.y) > [self stickRadius] * 1.45;
            if (run != running) {
                running = run;
                if (run) [tap impactOccurred];
            }
            CGPoint k, v = [self tiltFrom:stick_center to:p knob:&k];
            lx = (float)v.x;
            ly = (float)v.y;
            [self showStick:NO at:stick_center knob:k];
        } else if (t == rstick_touch) {
            CGPoint k, v = [self tiltFrom:rstick_center to:p knob:&k];
            rx = (float)v.x;
            ry = (float)v.y;
            [self showStick:YES at:rstick_center knob:k];
        } else if (t == look_touch) {
            // The camera: the thumb's travel as the game's mouse, as it comes. Right turns right; up looks up, unless the settings invert it.
            CGPoint d = CGPointZero;
            for (UITouch *c in [event coalescedTouchesForTouch:t] ?: @[t]) {
                CGPoint q = [c locationInView:self];
                d.x += q.x - look_last.x;
                d.y += q.y - look_last.y;
                look_last = q;
            }
            if (!in_menu) {
                double k = setting(@"mouseCameraSpeed", "ARKHAM_TOUCH_LOOK", 1.0) * CAMERA_DEG_PER_POINT / CAMERA_DEG_PER_PIXEL;
                // While a gadget is aimed (L2 held) the game turns the camera much more slowly for the same mouse travel (the owner, v23:
                // "very, very slow, hard to aim"), so the swipe is scaled up then, by its own setting.
                if ([self aiming]) k *= setting(@"aimCameraSpeed", "ARKHAM_TOUCH_AIM_LOOK", 2.5);
                rt_touch_mouse(d.x * k, d.y * k * (setting(@"invertLook", "ARKHAM_TOUCH_INVERT", 0) > 0 ? -1 : 1));
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
        // A short touch that hardly moved: outside play, a click there; with everything hidden for a picture, the controls back.
        NSValue *start = [tap_start objectForKey:t];
        NSNumber *when = [tap_time objectForKey:t];
        if (ended && start && when) {
            CGPoint p = [t locationInView:self], s0 = start.CGPointValue;
            if (t.timestamp - when.doubleValue < 0.35 && hypot(p.x - s0.x, p.y - s0.y) < 14) {
                if (photo)
                    [self setPhoto:NO];
                else if (in_menu || !rt_game_holds_cursor())
                    [self clickAt:p];
            }
        }
        [tap_start removeObjectForKey:t];
        [tap_time removeObjectForKey:t];
        if (t == pinch_touch) {
            pinch_touch = nil;
            pinch_last = 0;
            pan_sum = CGPointZero;  // the finger left on the map goes on as a drag from here
        } else if (t == pan_touch && pinch_touch) {
            pan_touch = pinch_touch;  // the other finger of a pinch carries on as the drag
            pan_last = pinch_point;
            pan_sum = CGPointZero;
            panning = YES;
            pinch_touch = nil;
            pinch_last = 0;
            [owners setObject:@"pan" forKey:pan_touch];
        } else if (t == pan_touch) {
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
        if (t == rstick_touch) {
            rstick_touch = nil;
            rx = ry = 0;
            rstick_base.hidden = rstick_knob.hidden = YES;
        }
        if (t == look_touch) look_touch = nil;
        [owners removeObjectForKey:t];
    }
    [self send];
}
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { [self lift:touches tapped:YES]; }
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { [self lift:touches tapped:NO]; }

- (void)releaseAll {
    for (PadButton *b in [self allButtons]) b.holders = 0;
    [owners removeAllObjects];
    [tap_start removeAllObjects];
    [tap_time removeAllObjects];
    stick_touch = look_touch = pan_touch = wheel_touch = pinch_touch = rstick_touch = nil;
    lx = ly = rx = ry = 0;
    pan_sum = pan_debt = CGPointZero;
    pinch_last = 0;
    zoom_debt = 0;
    zoom_lt = zoom_rt = 0;
    running = panning = NO;
    pulse_bits = 0;
    stick_base.hidden = stick_knob.hidden = rstick_base.hidden = rstick_knob.hidden = YES;
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
    if (!b || b == close_button || b == findbat_button) return;
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
// A grouped table, as iOS's own settings: each control under its name and as wide as the table, so that nothing is squeezed on a narrow screen. It
// opens over the game as a sheet; what's changed applies at once.
@interface SettingsController : UITableViewController
@end
@implementation SettingsController {
    NSMutableArray<NSString *> *titles, *footers;
    NSMutableArray<NSMutableArray<UITableViewCell *> *> *rows;
    NSMutableDictionary<NSValue *, NSString *> *actions;  // rows that do something when tapped
    UILabel *size_value, *opacity_value, *look_value, *aim_value;
}
- (UITableViewCell *)cellWith:(NSArray<UIView *> *)views {
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:views];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 10;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [cell.contentView addSubview:stack];
    UILayoutGuide *m = cell.contentView.layoutMarginsGuide;
    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:m.leadingAnchor], [stack.trailingAnchor constraintEqualToAnchor:m.trailingAnchor],
        [stack.topAnchor constraintEqualToAnchor:m.topAnchor constant:2], [stack.bottomAnchor constraintEqualToAnchor:m.bottomAnchor constant:-2],
    ]];
    return cell;
}
- (UILabel *)label:(NSString *)text {
    UILabel *l = [UILabel new];
    l.text = text;
    l.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    l.numberOfLines = 0;
    return l;
}
- (void)section:(NSString *)title footer:(NSString *)footer {
    [titles addObject:title];
    [footers addObject:footer ?: @""];
    [rows addObject:[NSMutableArray array]];
}
- (void)add:(UITableViewCell *)cell { [rows.lastObject addObject:cell]; }
- (void)choice:(NSString *)title items:(NSArray *)items value:(NSInteger)v action:(SEL)a {
    UISegmentedControl *seg = [[UISegmentedControl alloc] initWithItems:items];
    seg.selectedSegmentIndex = v;
    seg.apportionsSegmentWidthsByContent = YES;
    [seg addTarget:self action:a forControlEvents:UIControlEventValueChanged];
    [self add:[self cellWith:@[[self label:title], seg]]];
}
- (UILabel *)slider:(NSString *)title min:(float)min max:(float)max value:(float)v action:(SEL)a {
    UILabel *value = [UILabel new];
    value.font = [UIFont monospacedDigitSystemFontOfSize:17 weight:UIFontWeightRegular];
    value.textColor = UIColor.secondaryLabelColor;
    UIStackView *head = [[UIStackView alloc] initWithArrangedSubviews:@[[self label:title], value]];
    head.distribution = UIStackViewDistributionEqualSpacing;
    UISlider *s = [UISlider new];
    s.minimumValue = min;
    s.maximumValue = max;
    s.value = v;
    [s addTarget:self action:a forControlEvents:UIControlEventValueChanged];
    [self add:[self cellWith:@[head, s]]];
    return value;
}
- (void)toggle:(NSString *)title on:(BOOL)on action:(SEL)a {
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    cell.textLabel.text = title;
    cell.textLabel.numberOfLines = 0;
    UISwitch *sw = [UISwitch new];
    sw.on = on;
    [sw addTarget:self action:a forControlEvents:UIControlEventValueChanged];
    cell.accessoryView = sw;
    [self add:cell];
}
- (void)action:(NSString *)title selector:(SEL)a {
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
    cell.textLabel.text = title;
    cell.textLabel.textColor = self.view.tintColor;
    cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    [self add:cell];
    actions[[NSValue valueWithNonretainedObject:cell]] = NSStringFromSelector(a);
}
- (instancetype)init {
    if (!(self = [super initWithStyle:UITableViewStyleInsetGrouped])) return nil;
    self.title = @"Настройки";
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone target:self action:@selector(close)];
    titles = [NSMutableArray array];
    footers = [NSMutableArray array];
    rows = [NSMutableArray array];
    actions = [NSMutableDictionary dictionary];

    [self section:@"Изображение"
           footer:@"Экран показывает не больше 120 кадров в секунду; ограничение 60 бережёт батарею и греет меньше. Без рывков: в месте, где вы ещё не были, "
                  @"новый объект или эффект может появиться на кадр-другой позже, зато игра не замирает, пока видеокарта готовит его в первый раз. "
                  @"MetalFX дотягивает картинку игры до полного разрешения экрана — чётче, чем простое растяжение. Разрешение игры: высокое — половина "
                  @"пикселей экрана, как было; среднее и низкое дают больше FPS, MetalFX дотягивает их сильнее. Разрешение меняется со следующего запуска."];
    double cap = setting(@"fpsCap", "ARKHAM_FPS_CAP", 0);
    [self choice:@"Ограничение FPS" items:@[@"30", @"60", @"120", @"Без ограничения"] value:cap <= 0 ? 3 : cap <= 30 ? 0 : cap <= 60 ? 1 : 2 action:@selector(capChanged:)];
    [self toggle:@"MetalFX" on:setting(@"metalFX", "ARKHAM_METALFX", 1) > 0 action:@selector(metalFXChanged:)];
    double share = setting(@"renderScale", "ARKHAM_RENDER_SCALE", 0.5);
    [self choice:@"Разрешение игры" items:@[@"Высокое", @"Среднее", @"Низкое"] value:share >= 0.49 ? 0 : share >= 0.41 ? 1 : 2 action:@selector(renderScaleChanged:)];
    [self toggle:@"Без рывков в новых местах" on:setting(@"asyncPipelines", "ARKHAM_ASYNC_PIPELINES", 1) > 0 action:@selector(asyncChanged:)];
    [self toggle:@"Строка FPS и нагрева" on:setting(@"showFPS", "ARKHAM_FPS", 1) > 0 action:@selector(fpsLineChanged:)];
    [self toggle:@"Metal HUD (со следующего запуска)" on:setting(@"metalHUD", "ARKHAM_METAL_HUD", 0) > 0 action:@selector(hudChanged:)];

    [self section:@"Кнопки" footer:@"Глаз в углу прячет все кнопки для скриншота; вернуть — коснуться экрана."];
    [self choice:@"Вид" items:@[@"PlayStation", @"Xbox", @"Иконки"] value:(NSInteger)setting(@"buttonStyle", "ARKHAM_TOUCH_STYLE", STYLE_PS) action:@selector(styleChanged:)];
    size_value = [self slider:@"Размер" min:0.7 max:1.5 value:setting(@"buttonScale", "ARKHAM_TOUCH_SIZE", 1.0) action:@selector(sizeChanged:)];
    // Shown as transparency, as its name says; kept as the opacity, from 20% to all of it.
    opacity_value = [self slider:@"Прозрачность кнопок" min:0 max:0.8 value:1 - setting(@"opacity", "ARKHAM_TOUCH_OPACITY", 0.8) action:@selector(opacityChanged:)];
    [self toggle:@"Прятать вне игры" on:setting(@"hideInMenus", "ARKHAM_TOUCH_HIDE", 1) > 0 action:@selector(hideChanged:)];
    [self action:@"Расположение кнопок" selector:@selector(editLayout)];

    [self section:@"Управление" footer:@"Камера поворачивается ровно на столько, на сколько проведён палец, с любой скоростью и на ходу. Скорость — на сколько градусов поворачивает свайп через весь экран. При прицеле (зажата L2) игра сама замедляет камеру, поэтому свайп тогда во столько раз быстрее."];
    look_value = [self slider:@"Скорость камеры" min:0.25 max:3.0 value:setting(@"mouseCameraSpeed", "ARKHAM_TOUCH_LOOK", 1.0) action:@selector(lookChanged:)];
    aim_value = [self slider:@"Камера при прицеле" min:1.0 max:5.0 value:setting(@"aimCameraSpeed", "ARKHAM_TOUCH_AIM_LOOK", 2.5) action:@selector(aimLookChanged:)];
    [self toggle:@"Инвертировать вверх-вниз" on:setting(@"invertLook", "ARKHAM_TOUCH_INVERT", 0) > 0 action:@selector(invertChanged:)];
    [self toggle:@"Бег: стик за край" on:setting(@"autoRun", "ARKHAM_TOUCH_RUN", 1) > 0 action:@selector(runChanged:)];
    [self section:@"Камера игры" footer:@"Помощь камеры — сама разворачивает камеру за спину Бэтмена на бегу. С сенсорной камерой она мешает поворачивать на бегу, с геймпадом может быть удобна. Меняется со следующего запуска игры."];
    [self toggle:@"Помощь камеры" on:setting(@"cameraAssist", "ARKHAM_CAMERA_ASSIST", 0) > 0 action:@selector(assistChanged:)];
    [self showValues];
    return self;
}
- (void)showValues {
    size_value.text = [NSString stringWithFormat:@"%.0f %%", setting(@"buttonScale", "ARKHAM_TOUCH_SIZE", 1.0) * 100];
    opacity_value.text = [NSString stringWithFormat:@"%.0f %%", (1 - setting(@"opacity", "ARKHAM_TOUCH_OPACITY", 0.8)) * 100];
    look_value.text = [NSString stringWithFormat:@"%.0f° на экран", setting(@"mouseCameraSpeed", "ARKHAM_TOUCH_LOOK", 1.0) * CAMERA_DEG_PER_POINT * 951];
    aim_value.text = [NSString stringWithFormat:@"× %.1f", setting(@"aimCameraSpeed", "ARKHAM_TOUCH_AIM_LOOK", 2.5)];
}
- (NSInteger)numberOfSectionsInTableView:(UITableView *)t { return (NSInteger)rows.count; }
- (NSInteger)tableView:(UITableView *)t numberOfRowsInSection:(NSInteger)s { return (NSInteger)rows[(NSUInteger)s].count; }
- (UITableViewCell *)tableView:(UITableView *)t cellForRowAtIndexPath:(NSIndexPath *)i { return rows[(NSUInteger)i.section][(NSUInteger)i.row]; }
- (NSString *)tableView:(UITableView *)t titleForHeaderInSection:(NSInteger)s { return titles[(NSUInteger)s]; }
- (NSString *)tableView:(UITableView *)t titleForFooterInSection:(NSInteger)s { return footers[(NSUInteger)s]; }
- (void)tableView:(UITableView *)t didSelectRowAtIndexPath:(NSIndexPath *)i {
    [t deselectRowAtIndexPath:i animated:YES];
    NSString *a = actions[[NSValue valueWithNonretainedObject:rows[(NSUInteger)i.section][(NSUInteger)i.row]]];
    if (a) ((void (*)(id, SEL))objc_msgSend)(self, NSSelectorFromString(a));
}
- (void)capChanged:(UISegmentedControl *)s {
    static const double caps[] = {30, 60, 120, 0};
    set_setting(@"fpsCap", caps[s.selectedSegmentIndex]);
    rt_set_fps_cap(caps[s.selectedSegmentIndex]);
    [controls followFrameRateLimit];
}
- (void)styleChanged:(UISegmentedControl *)s {
    set_setting(@"buttonStyle", s.selectedSegmentIndex);
    [controls restyle];
}
- (void)sizeChanged:(UISlider *)s {
    set_setting(@"buttonScale", s.value);
    [self showValues];
    [controls restyle];
}
- (void)opacityChanged:(UISlider *)s {
    set_setting(@"opacity", 1 - s.value);
    [self showValues];
    [controls restyle];
}
- (void)lookChanged:(UISlider *)s {
    set_setting(@"mouseCameraSpeed", s.value);
    [self showValues];
}
- (void)aimLookChanged:(UISlider *)s {
    set_setting(@"aimCameraSpeed", s.value);
    [self showValues];
}
- (void)invertChanged:(UISwitch *)s { set_setting(@"invertLook", s.on); }
- (void)runChanged:(UISwitch *)s { set_setting(@"autoRun", s.on); }
- (void)assistChanged:(UISwitch *)s { set_setting(@"cameraAssist", s.on); }
- (void)hideChanged:(UISwitch *)s { set_setting(@"hideInMenus", s.on); }
- (void)fpsLineChanged:(UISwitch *)s {
    set_setting(@"showFPS", s.on);
    rt_ios_overlay_enable(s.on);
}
// iOS shows Apple's HUD only in an app it was on for from the launch, and doesn't take it away while it runs; so the switch says what the next launch
// does, and the HUD of this one stays as it is.
- (void)hudChanged:(UISwitch *)s {
    set_setting(@"metalHUD", s.on);
    [prefs() setBool:s.on forKey:@"MetalForceHudEnabled"];  // Metal's own key: the HUD from the next launch
    if (s.on && rt_ios_hud_at_launch) rt_set_metal_hud(1);   // on in this process from its start: it can show again now
    UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Metal HUD"
                                                               message:s.on ? @"Включится при следующем запуске игры." : @"Пропадёт при следующем запуске игры."
                                                        preferredStyle:UIAlertControllerStyleAlert];
    [a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:a animated:YES completion:nil];
}
- (void)asyncChanged:(UISwitch *)s {
    set_setting(@"asyncPipelines", s.on);
    rt_set_async_pipelines(s.on);
}
- (void)metalFXChanged:(UISwitch *)s {
    set_setting(@"metalFX", s.on);
    rt_set_metalfx(s.on);
}
// The share of the screen's pixels the game draws at: half (1424 x 1000 on the inner screen), 0.42 and 0.375. The game takes it at its start.
- (void)renderScaleChanged:(UISegmentedControl *)s {
    static const double shares[] = {0.5, 0.42, 0.375};
    set_setting(@"renderScale", shares[s.selectedSegmentIndex]);
    UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Разрешение игры" message:@"Поменяется при следующем запуске игры."
                                                        preferredStyle:UIAlertControllerStyleAlert];
    [a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:a animated:YES completion:nil];
}
- (void)editLayout {
    [self.navigationController dismissViewControllerAnimated:YES completion:^{
        controls.hidden = NO;
        [controls startEditing];
    }];
}
- (void)close { [self.navigationController dismissViewControllerAnimated:YES completion:nil]; }
@end

static UIView *game_view;
static void open_settings(void) {
    [controls releaseAll];
    UIViewController *root = game_view.window.rootViewController;
    if (root.presentedViewController) return;
    UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:[SettingsController new]];
    nav.modalPresentationStyle = UIModalPresentationFormSheet;
    nav.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
    [root presentViewController:nav animated:YES completion:nil];
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
    // Read by the engine at its start, from its camera settings (shims_libc.c): this game's camera assist.
    setenv("ARKHAM_CAMERA_ASSIST", setting(@"cameraAssist", "ARKHAM_CAMERA_ASSIST", 0) > 0 ? "1" : "0", 1);
    rt_set_fps_cap(setting(@"fpsCap", "ARKHAM_FPS_CAP", 0));
    rt_set_async_pipelines(setting(@"asyncPipelines", "ARKHAM_ASYNC_PIPELINES", 1) > 0);
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
    gear_view = gear;
    if (!controls) return;
    for (NSNotificationName n in @[GCControllerDidConnectNotification, GCControllerDidDisconnectNotification])
        [NSNotificationCenter.defaultCenter addObserverForName:n object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *note) { update_visibility(); }];
    update_visibility();
}
