// Touch controls and the phone's settings, after Feral's Tomb Raider for iOS.
//
// The controls: a stick wherever the left thumb lands (pushed past its edge it also holds the run button), swipes on the right that turn the camera,
// and the pad's buttons where the thumbs rest: the main four in an arc under the right thumb, detective mode and aiming above the left one. They draw as
// a PlayStation pad, an Xbox pad, or as icons of what each does in the game (from the game's own bindings in BmInput.ini). What they do goes to the
// runtime as a pad of its own (rt_touch_pad in shims_hid.m), shown to the game as the pad drawn so that its prompts match; the camera is that pad's
// right stick, since the game ignores the mouse while a pad moves. In menus, where the game doesn't hold the cursor, a tap is also a click there.
// They hide while a real controller is connected.
//
// The settings, behind the gear at the top right: the frame rate limit, the controls' style, size, opacity and camera speed, the run push, the frame
// rate line and Apple's Metal HUD, and moving and resizing each button. They're kept in the app's own defaults. ARKHAM_TOUCH=0 turns the controls off.
#import <GameController/GameController.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>

void rt_touch_pad(float lx, float ly, float rx, float ry, float lt, float rt, unsigned buttons);
void rt_touch_pad_on(int on);
void rt_touch_pad_playstation(int ps);
int rt_controller_connected(void);
int rt_game_holds_cursor(void);
void rt_ios_click(double x, double y);
void rt_set_fps_cap(double n);
void rt_set_metal_hud(int on);
void rt_ios_overlay_enable(int on);  // main.m: the frame rate line

// The runtime's numbers for the controls (the enum in shims_hid.m).
enum { C_LT = 4, C_RT = 5, B_SOUTH = 7, B_EAST, B_WEST, B_NORTH, B_LB, B_RB, B_START, B_BACK, B_L3, B_R3, B_HOME, D_UP, D_DOWN, D_LEFT, D_RIGHT, C_SETTINGS = 99 };
enum { STYLE_PS, STYLE_XBOX, STYLE_ICONS };
enum { A_BR, A_BL, A_TR, A_TL, A_TC };       // what a button's place is measured from: a corner of the screen, or the top's middle
enum { SH_NONE, SH_CROSS, SH_CIRCLE, SH_SQUARE, SH_TRIANGLE };

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
static void set_setting(NSString *key, double v) { [prefs() setDouble:v forKey:key]; }

// ---------- the buttons ----------
typedef struct {
    const char *ident;
    int control, anchor;
    CGFloat dx, dy, r;  // the default place from the anchor, in points before scaling, and the size
    const char *ps, *xbox, *icon, *hint;
    int shape, ps_color, xbox_color;
} Def;
static const Def defs[] = {
    // Under the right thumb: the arc of four, then grapple and crouch further along it.
    {"cross", B_SOUTH, A_BR, -86, -66, 34, "", "A", "figure.run", "Бег, действие", SH_CROSS, 0x7FB2E5, 0x6DB33F},
    {"square", B_WEST, A_BR, -184, -60, 36, "", "X", "hand.raised.fill", "Удар", SH_SQUARE, 0xE48AD6, 0x3C8FE0},
    {"triangle", B_NORTH, A_BR, -140, -154, 34, "", "Y", "shield.fill", "Захват, контратака", SH_TRIANGLE, 0x5CC49A, 0xF5C518},
    {"circle", B_EAST, A_BR, -50, -164, 30, "", "B", "wind", "Плащ, отмена", SH_CIRCLE, 0xE86A6A, 0xE03A3E},
    {"r1", B_RB, A_BR, -86, -258, 27, "R1", "RB", "arrow.up.right", "Крюк", SH_NONE, 0xFFFFFF, 0xFFFFFF},
    {"r2", C_RT, A_BR, -236, -158, 28, "R2", "RT", "arrow.down.to.line", "Присесть, бросок", SH_NONE, 0xFFFFFF, 0xFFFFFF},
    {"r3", B_R3, A_BR, -200, -250, 21, "R3", "RS", "plus.magnifyingglass", "Зум", SH_NONE, 0xFFFFFF, 0xFFFFFF},
    // Above the left thumb's stick.
    {"l1", B_LB, A_BL, 68, -262, 28, "L1", "LB", "eye.fill", "Режим детектива", SH_NONE, 0xFFFFFF, 0xFFFFFF},
    {"l2", C_LT, A_BL, 166, -276, 28, "L2", "LT", "scope", "Прицел", SH_NONE, 0xFFFFFF, 0xFFFFFF},
    {"l3", B_L3, A_BL, 60, -350, 20, "L3", "LS", "dot.viewfinder", "Камера за спину", SH_NONE, 0xFFFFFF, 0xFFFFFF},
    // Gadgets, top left.
    {"up", D_UP, A_TL, 124, 104, 22, "", "", "chevron.up", "", SH_NONE, 0xFFFFFF, 0xFFFFFF},
    {"down", D_DOWN, A_TL, 124, 184, 22, "", "", "chevron.down", "", SH_NONE, 0xFFFFFF, 0xFFFFFF},
    {"left", D_LEFT, A_TL, 84, 144, 22, "", "", "chevron.left", "", SH_NONE, 0xFFFFFF, 0xFFFFFF},
    {"right", D_RIGHT, A_TL, 164, 144, 22, "", "", "chevron.right", "", SH_NONE, 0xFFFFFF, 0xFFFFFF},
    // The top.
    {"map", B_BACK, A_TC, -64, 28, 20, "", "", "map.fill", "Карта", SH_NONE, 0xFFFFFF, 0xFFFFFF},
    {"pause", B_START, A_TC, 64, 28, 20, "", "", "pause.fill", "Пауза", SH_NONE, 0xFFFFFF, 0xFFFFFF},
};
#define N_DEFS (sizeof defs / sizeof defs[0])

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

@interface PadButton : NSObject
@property(nonatomic) const Def *def;
@property(nonatomic) CGPoint center;
@property(nonatomic) CGFloat radius;
@property(nonatomic, strong) CAShapeLayer *back, *symbol, *badge;
@property(nonatomic, strong) CATextLayer *text, *badge_text;
@property(nonatomic, strong) CALayer *icon;
@property(nonatomic) int holders;
@end
@implementation PadButton
@end

@class SettingsView;
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
    NSMapTable<UITouch *, id> *owners;
    NSMapTable<UITouch *, NSValue *> *tap_start;
    NSMapTable<UITouch *, NSNumber *> *tap_time;
    UITouch *stick_touch, *look_touch;
    CGPoint stick_center, look_last;
    CGPoint look_sum;  // swipe since the last display frame
    float lx, ly, rx, ry;
    BOOL running;  // the stick pushed past its edge
    CAShapeLayer *stick_base, *stick_knob;
    UIImpactFeedbackGenerator *tap;
    CADisplayLink *link;
    CFTimeInterval last_frame;
    // editing
    PadButton *dragged, *selected;
    CGPoint drag_offset;
    UIView *toolbar;
    UISlider *size_slider;
    UILabel *size_label;
}

- (instancetype)initWithFrame:(CGRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    self.multipleTouchEnabled = YES;
    self.backgroundColor = UIColor.clearColor;
    owners = [NSMapTable strongToStrongObjectsMapTable];
    tap_start = [NSMapTable strongToStrongObjectsMapTable];
    tap_time = [NSMapTable strongToStrongObjectsMapTable];
    tap = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];
    buttons = [NSMutableArray array];
    for (size_t i = 0; i < N_DEFS; i++) {
        PadButton *b = [PadButton new];
        b.def = &defs[i];
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
        for (CALayer *l in @[b.back, b.symbol, b.text, b.icon, b.badge, b.badge_text]) [self.layer addSublayer:l];
        [buttons addObject:b];
    }
    stick_base = [CAShapeLayer layer];
    stick_knob = [CAShapeLayer layer];
    for (CAShapeLayer *l in @[stick_base, stick_knob]) {
        l.hidden = YES;
        [self.layer addSublayer:l];
    }
    stick_base.lineWidth = 2;
    link = [CADisplayLink displayLinkWithTarget:self selector:@selector(frame:)];
    if (@available(iOS 15.0, *)) link.preferredFrameRateRange = CAFrameRateRangeMake(60, 120, 120);
    [link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    return self;
}

- (CGFloat)scale {
    CGSize s = self.bounds.size;
    CGFloat fit = MIN(MAX(MIN(s.width, s.height) / 480.0, 0.8), 1.3);
    return fit * setting(@"buttonScale", "ARKHAM_TOUCH_SIZE", 1.0);
}
- (int)style { return (int)setting(@"buttonStyle", "ARKHAM_TOUCH_STYLE", STYLE_PS); }

// A button's place: the default, or where the player moved it (kept as offsets from the same anchor, in unscaled points).
- (void)placeButton:(PadButton *)b {
    CGSize s = self.bounds.size;
    UIEdgeInsets in = self.safeAreaInsets;
    CGFloat left = MAX(in.left, 8), right = s.width - MAX(in.right, 8), top = MAX(in.top, 8), bottom = s.height - MAX(in.bottom, 8);
    NSDictionary *mine = [prefs() dictionaryForKey:@"layout"][@(b.def->ident)];
    CGFloat dx = mine ? [mine[@"dx"] doubleValue] : b.def->dx, dy = mine ? [mine[@"dy"] doubleValue] : b.def->dy;
    CGFloat r = (mine ? [mine[@"r"] doubleValue] : b.def->r), k = [self scale];
    CGPoint a = CGPointZero;
    switch (b.def->anchor) {
        case A_BR: a = CGPointMake(right, bottom); break;
        case A_BL: a = CGPointMake(left, bottom); break;
        case A_TR: a = CGPointMake(right, top); break;
        case A_TL: a = CGPointMake(left, top); break;
        case A_TC: a = CGPointMake(s.width / 2, top); break;
    }
    b.center = CGPointMake(a.x + dx * k, a.y + dy * k);
    b.radius = r * k;
}
- (CGPoint)anchorOf:(PadButton *)b {
    CGSize s = self.bounds.size;
    UIEdgeInsets in = self.safeAreaInsets;
    CGFloat left = MAX(in.left, 8), right = s.width - MAX(in.right, 8), top = MAX(in.top, 8), bottom = s.height - MAX(in.bottom, 8);
    switch (b.def->anchor) {
        case A_BR: return CGPointMake(right, bottom);
        case A_BL: return CGPointMake(left, bottom);
        case A_TR: return CGPointMake(right, top);
        case A_TL: return CGPointMake(left, top);
        default: return CGPointMake(s.width / 2, top);
    }
}
- (void)saveButton:(PadButton *)b {
    CGPoint a = [self anchorOf:b];
    CGFloat k = [self scale];
    NSMutableDictionary *layout = [[prefs() dictionaryForKey:@"layout"] mutableCopy] ?: [NSMutableDictionary dictionary];
    layout[@(b.def->ident)] = @{@"dx": @((b.center.x - a.x) / k), @"dy": @((b.center.y - a.y) / k), @"r": @(b.radius / k)};
    [prefs() setObject:layout forKey:@"layout"];
}

- (void)layoutSubviews {
    [super layoutSubviews];
    [self restyle];
    toolbar.center = CGPointMake(self.bounds.size.width / 2, MAX(self.safeAreaInsets.top, 8) + 70);
}

// Draws every button for the style chosen.
- (void)restyle {
    int style = [self style];
    CGFloat alpha = setting(@"opacity", "ARKHAM_TOUCH_OPACITY", 0.6);
    rt_touch_pad_playstation(style != STYLE_XBOX);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    for (PadButton *b in buttons) {
        [self placeButton:b];
        const Def *d = b.def;
        CGPoint c = b.center;
        CGFloat r = b.radius;
        BOOL down = b.holders > 0;
        b.back.path = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(c.x - r, c.y - r, r * 2, r * 2)].CGPath;
        b.back.fillColor = [UIColor colorWithWhite:down ? 0.35 : 0 alpha:(down ? 0.6 : 0.32) * (alpha / 0.6)].CGColor;
        b.back.strokeColor = [UIColor colorWithWhite:1 alpha:(self.editing ? 0.9 : 0.5) * alpha].CGColor;
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
            UIImageSymbolConfiguration *cfg = [UIImageSymbolConfiguration configurationWithPointSize:r * 0.85 weight:UIImageSymbolWeightSemibold];
            UIImage *img = [[UIImage systemImageNamed:@(d->icon) withConfiguration:cfg] imageWithTintColor:UIColor.whiteColor renderingMode:UIImageRenderingModeAlwaysOriginal];
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
                UIBezierPath *bp = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(bc.x - br, bc.y - br, br * 2, br * 2)];
                b.badge.path = bp.CGPath;
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
    stick_base.fillColor = [UIColor colorWithWhite:0 alpha:0.25 * (alpha / 0.6)].CGColor;
    stick_base.strokeColor = [UIColor colorWithWhite:1 alpha:0.45 * (alpha / 0.6)].CGColor;
    [CATransaction commit];
}

- (PadButton *)buttonAt:(CGPoint)p {
    PadButton *best = nil;
    CGFloat best_d = 1e9;
    for (PadButton *b in buttons) {
        CGFloat d = hypot(p.x - b.center.x, p.y - b.center.y);
        if (d < b.radius * 1.25 && d < best_d) best = b, best_d = d;  // a little beyond what's drawn: thumbs are wide
    }
    return best;
}

- (void)send {
    unsigned bits = 0;
    float lt = 0, rt = 0;
    for (PadButton *b in buttons) {
        if (!b.holders) continue;
        int k = b.def->control;
        if (k == C_LT) lt = 1;
        else if (k == C_RT) rt = 1;
        else if (k != C_SETTINGS) bits |= 1u << k;
    }
    if (running) bits |= 1u << B_SOUTH;
    rt_touch_pad(lx, ly, rx, ry, lt, rt, bits);
}

- (void)showStickAt:(CGPoint)c knob:(CGPoint)k {
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    CGFloat R = 62 * [self scale] / 1.15, kr = R * 0.42;
    stick_base.path = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(c.x - R, c.y - R, R * 2, R * 2)].CGPath;
    stick_knob.path = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(k.x - kr, k.y - kr, kr * 2, kr * 2)].CGPath;
    stick_knob.fillColor = (running ? rgb(0x7FB2E5, 0.8) : [UIColor colorWithWhite:1 alpha:0.45]).CGColor;
    stick_base.hidden = stick_knob.hidden = NO;
    [CATransaction commit];
}

// The camera, once a display frame: how fast the thumb moved, as the right stick's push. The game's own dead zone (0.2) is stepped over so that a slow
// swipe still turns, and a still thumb stops the camera at once.
- (void)frame:(CADisplayLink *)l {
    CFTimeInterval now = l.timestamp, dt = last_frame > 0 ? now - last_frame : 1.0 / 60;
    last_frame = now;
    if (dt <= 0 || dt > 0.1) dt = 1.0 / 60;
    float nx = 0, ny = 0;
    if (look_touch) {
        double speed = setting(@"lookSpeed", "ARKHAM_TOUCH_LOOK", 1.0);
        double full = 1100.0 / speed;  // points a second that turn the camera as fast as the stick can
        double vx = look_sum.x / dt, vy = look_sum.y / dt;
        double v = hypot(vx, vy);
        if (v > 15) {
            double m = 0.22 + 0.78 * MIN(1.0, v / full);
            nx = (float)(vx / v * m);
            ny = (float)(-vy / v * m);
        }
    }
    look_sum = CGPointZero;
    // A little smoothing over frames, so a swipe's speed doesn't jitter between them.
    rx = rx * 0.35f + nx * 0.65f;
    ry = ry * 0.35f + ny * 0.65f;
    if (fabsf(rx) < 0.05f && fabsf(ry) < 0.05f && !look_touch) rx = ry = 0;
    [self send];
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    for (UITouch *t in touches) {
        CGPoint p = [t locationInView:self];
        if (self.editing) {
            [self editTouchBegan:p];
            continue;
        }
        PadButton *b = [self buttonAt:p];
        if (b && b.def->control == C_SETTINGS) {
            [tap impactOccurred];
            open_settings();
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
        if (p.x < self.bounds.size.width * 0.45 && !stick_touch) {
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
        if (t == stick_touch) {
            CGFloat R = 62 * [self scale] / 1.15, dx = p.x - stick_center.x, dy = p.y - stick_center.y, d = hypot(dx, dy);
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
            NSArray<UITouch *> *all = [event coalescedTouchesForTouch:t] ?: @[t];
            for (UITouch *c in all) {
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
        if ([owner isKindOfClass:PadButton.class]) {
            PadButton *b = owner;
            if (b.holders > 0) b.holders--;
            [self restyle];
        }
        // A short touch that hardly moved, where the game isn't holding the cursor (its menus), is a click there too.
        NSValue *start = [tap_start objectForKey:t];
        NSNumber *when = [tap_time objectForKey:t];
        if (ended && start && when) {
            CGPoint p = [t locationInView:self], s0 = start.CGPointValue;
            if (t.timestamp - when.doubleValue < 0.35 && hypot(p.x - s0.x, p.y - s0.y) < 14 && !rt_game_holds_cursor()) rt_ios_click(p.x, p.y);
        }
        [tap_start removeObjectForKey:t];
        [tap_time removeObjectForKey:t];
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
    [owners removeAllObjects];
    [tap_start removeAllObjects];
    [tap_time removeAllObjects];
    stick_touch = look_touch = nil;
    lx = ly = rx = ry = 0;
    running = NO;
    stick_base.hidden = stick_knob.hidden = YES;
    [self restyle];
    [self send];
}

// ---------- moving and resizing the buttons ----------
- (void)startEditing {
    [self releaseAll];
    self.editing = YES;
    selected = nil;
    if (!toolbar) {
        toolbar = [[UIView alloc] initWithFrame:CGRectMake(0, 0, 430, 96)];
        toolbar.backgroundColor = [UIColor colorWithWhite:0.08 alpha:0.88];
        toolbar.layer.cornerRadius = 14;
        UILabel *hint = [[UILabel alloc] initWithFrame:CGRectMake(14, 8, 402, 20)];
        hint.text = @"Перетаскивайте кнопки. Размер — у выбранной.";
        hint.textColor = UIColor.whiteColor;
        hint.font = [UIFont systemFontOfSize:13 weight:UIFontWeightMedium];
        [toolbar addSubview:hint];
        size_label = [[UILabel alloc] initWithFrame:CGRectMake(14, 40, 70, 24)];
        size_label.textColor = UIColor.lightGrayColor;
        size_label.font = [UIFont systemFontOfSize:13];
        size_label.text = @"Размер";
        [toolbar addSubview:size_label];
        size_slider = [[UISlider alloc] initWithFrame:CGRectMake(80, 38, 150, 28)];
        size_slider.minimumValue = 14;
        size_slider.maximumValue = 60;
        [size_slider addTarget:self action:@selector(sizeChanged:) forControlEvents:UIControlEventValueChanged];
        [toolbar addSubview:size_slider];
        UIButton *reset = [UIButton buttonWithType:UIButtonTypeSystem];
        [reset setTitle:@"Сбросить" forState:UIControlStateNormal];
        reset.frame = CGRectMake(240, 36, 84, 32);
        [reset addTarget:self action:@selector(resetLayout) forControlEvents:UIControlEventTouchUpInside];
        [toolbar addSubview:reset];
        UIButton *done = [UIButton buttonWithType:UIButtonTypeSystem];
        [done setTitle:@"Готово" forState:UIControlStateNormal];
        done.titleLabel.font = [UIFont systemFontOfSize:17 weight:UIFontWeightSemibold];
        done.frame = CGRectMake(330, 36, 86, 32);
        [done addTarget:self action:@selector(stopEditing) forControlEvents:UIControlEventTouchUpInside];
        [toolbar addSubview:done];
        UILabel *what = [[UILabel alloc] initWithFrame:CGRectMake(14, 68, 402, 20)];
        what.tag = 7;
        what.textColor = UIColor.lightGrayColor;
        what.font = [UIFont systemFontOfSize:12];
        [toolbar addSubview:what];
    }
    toolbar.hidden = NO;
    [self addSubview:toolbar];
    [self setNeedsLayout];
    [self restyle];
}
- (void)stopEditing {
    self.editing = NO;
    toolbar.hidden = YES;
    selected = dragged = nil;
    [self restyle];
}
- (void)resetLayout {
    [prefs() removeObjectForKey:@"layout"];
    selected = nil;
    [self restyle];
}
- (void)select:(PadButton *)b {
    selected = b;
    size_slider.value = b ? b.radius / [self scale] : 30;
    UILabel *what = [toolbar viewWithTag:7];
    what.text = b ? [NSString stringWithFormat:@"%s", *b.def->hint ? b.def->hint : b.def->ident] : @"";
    [self restyle];
}
- (void)sizeChanged:(UISlider *)s {
    if (!selected) return;
    selected.radius = s.value * [self scale];
    [self saveButton:selected];
    [self restyle];
}
- (void)editTouchBegan:(CGPoint)p {
    PadButton *b = [self buttonAt:p];
    if (!b || b.def->control == C_SETTINGS) return;
    dragged = b;
    drag_offset = CGPointMake(b.center.x - p.x, b.center.y - p.y);
    [self select:b];
}
- (void)editTouchMoved:(CGPoint)p {
    if (!dragged) return;
    dragged.center = CGPointMake(p.x + drag_offset.x, p.y + drag_offset.y);
    [self saveButton:dragged];
    [self restyle];
}
- (void)editTouchEnded { dragged = nil; }
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
    [NSLayoutConstraint activateConstraints:@[
        [scroll.topAnchor constraintEqualToAnchor:self.safeAreaLayoutGuide.topAnchor constant:12],
        [scroll.bottomAnchor constraintEqualToAnchor:self.safeAreaLayoutGuide.bottomAnchor constant:-12],
        [scroll.centerXAnchor constraintEqualToAnchor:self.centerXAnchor],
        [scroll.widthAnchor constraintEqualToConstant:520],
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
    [self row:@"Скорость камеры" control:[self slider:0.3 max:3.0 value:setting(@"lookSpeed", "ARKHAM_TOUCH_LOOK", 1.0) action:@selector(lookChanged:)]];
    [self row:@"Бег: стик за край" control:[self toggle:setting(@"autoRun", "ARKHAM_TOUCH_RUN", 1) > 0 action:@selector(runChanged:)]];
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
- (void)lookChanged:(UISlider *)s { set_setting(@"lookSpeed", s.value); }
- (void)runChanged:(UISwitch *)s { set_setting(@"autoRun", s.on); }
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
    UIImageSymbolConfiguration *cfg = [UIImageSymbolConfiguration configurationWithPointSize:20 weight:UIImageSymbolWeightSemibold];
    [gear setImage:[[UIImage systemImageNamed:@"gearshape.fill" withConfiguration:cfg] imageWithTintColor:[UIColor colorWithWhite:1 alpha:0.7]
                                                                                             renderingMode:UIImageRenderingModeAlwaysOriginal]
          forState:UIControlStateNormal];
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
