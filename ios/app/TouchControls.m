// Touch controls, after Feral's for Tomb Raider on iOS: a stick wherever the left thumb lands, swipes on the right to look around, and the pad's buttons
// drawn over the game. What they do goes to the runtime as a pad of its own (rt_touch_pad in shims_hid.m, shown to the game as an Xbox controller, so
// the game's prompts name the buttons drawn here) and as mouse motion for the camera (rt_touch_look in shims_sys.c). They hide while a controller is
// connected. ARKHAM_TOUCH=0 turns them off; ARKHAM_TOUCH_LOOK=n sets how far the camera turns for a point of swipe (1.8 unless said).
#import <GameController/GameController.h>
#import <UIKit/UIKit.h>

void rt_touch_pad(float lx, float ly, float lt, float rt, unsigned buttons);
void rt_touch_pad_on(int on);
void rt_touch_look(double dx, double dy);
int rt_controller_connected(void);

// The runtime's numbers for the controls (the enum in shims_hid.m).
enum { C_LT = 4, C_RT = 5, B_SOUTH = 7, B_EAST, B_WEST, B_NORTH, B_LB, B_RB, B_START, B_BACK, B_L3, B_R3, B_HOME, D_UP, D_DOWN, D_LEFT, D_RIGHT };

@interface PadButton : NSObject
@property(nonatomic) int control;
@property(nonatomic, copy) NSString *label;
@property(nonatomic, strong) UIColor *color;
@property(nonatomic) CGPoint center;
@property(nonatomic) CGFloat radius;
@property(nonatomic) BOOL pill;
@property(nonatomic, strong) CAShapeLayer *shape;
@property(nonatomic, strong) CATextLayer *text;
@property(nonatomic) int holders;  // touches on it now
@end
@implementation PadButton
@end

@interface TouchControlsView : UIView
@end

@implementation TouchControlsView {
    NSMutableArray<PadButton *> *buttons;
    NSMapTable<UITouch *, id> *owners;  // what each touch is holding: a button, or the stick or the look area as a string
    UITouch *stick_touch, *look_touch;
    CGPoint stick_center, look_last;
    float lx, ly;
    CAShapeLayer *stick_base, *stick_knob;
    UIImpactFeedbackGenerator *tap;
    double look_scale;
}

static UIColor *rgb(int c) { return [UIColor colorWithRed:((c >> 16) & 255) / 255.0 green:((c >> 8) & 255) / 255.0 blue:(c & 255) / 255.0 alpha:1]; }

- (instancetype)initWithFrame:(CGRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    self.multipleTouchEnabled = YES;
    self.backgroundColor = UIColor.clearColor;
    owners = [NSMapTable strongToStrongObjectsMapTable];
    tap = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];
    const char *s = getenv("ARKHAM_TOUCH_LOOK");
    look_scale = s && atof(s) > 0 ? atof(s) : 1.8;
    buttons = [NSMutableArray array];
    struct { int control; const char *label; int color; BOOL pill; } defs[] = {
        {B_SOUTH, "A", 0x6DB33F, NO}, {B_EAST, "B", 0xE03A3E, NO}, {B_WEST, "X", 0x3C8FE0, NO}, {B_NORTH, "Y", 0xF5C518, NO},
        {B_LB, "LB", 0xFFFFFF, YES}, {B_RB, "RB", 0xFFFFFF, YES}, {C_LT, "LT", 0xFFFFFF, YES}, {C_RT, "RT", 0xFFFFFF, YES},
        {B_BACK, "Карта", 0xFFFFFF, YES}, {B_START, "Меню", 0xFFFFFF, YES}, {B_R3, "R3", 0xFFFFFF, NO}, {B_L3, "L3", 0xFFFFFF, NO},
        {D_UP, "▲", 0xFFFFFF, NO}, {D_DOWN, "▼", 0xFFFFFF, NO}, {D_LEFT, "◀", 0xFFFFFF, NO}, {D_RIGHT, "▶", 0xFFFFFF, NO},
    };
    for (size_t i = 0; i < sizeof defs / sizeof defs[0]; i++) {
        PadButton *b = [PadButton new];
        b.control = defs[i].control;
        b.label = @(defs[i].label);
        b.color = rgb(defs[i].color);
        b.pill = defs[i].pill;
        b.shape = [CAShapeLayer layer];
        b.shape.lineWidth = 1.5;
        b.text = [CATextLayer layer];
        b.text.string = b.label;
        b.text.alignmentMode = kCAAlignmentCenter;
        b.text.contentsScale = UIScreen.mainScreen.scale;
        [self.layer addSublayer:b.shape];
        [self.layer addSublayer:b.text];
        [buttons addObject:b];
    }
    stick_base = [CAShapeLayer layer];
    stick_knob = [CAShapeLayer layer];
    for (CAShapeLayer *l in @[stick_base, stick_knob]) {
        l.hidden = YES;
        [self.layer addSublayer:l];
    }
    stick_base.fillColor = [UIColor colorWithWhite:0 alpha:0.25].CGColor;
    stick_base.strokeColor = [UIColor colorWithWhite:1 alpha:0.45].CGColor;
    stick_base.lineWidth = 2;
    stick_knob.fillColor = [UIColor colorWithWhite:1 alpha:0.45].CGColor;
    return self;
}

// Where everything goes, for a landscape screen of any size: the face buttons at the bottom right, the shoulders and triggers in the top corners, the
// directions at the left above the stick's area, Map and Menu at the top in the middle.
- (void)layoutSubviews {
    [super layoutSubviews];
    CGSize s = self.bounds.size;
    UIEdgeInsets in = self.safeAreaInsets;
    CGFloat left = MAX(in.left, 16), right = s.width - MAX(in.right, 16), top = MAX(in.top, 12), bottom = s.height - MAX(in.bottom, 12);
    CGFloat r = 31, gap = 54;
    CGPoint face = CGPointMake(right - 110, bottom - 100);
    for (PadButton *b in buttons) {
        CGFloat rad = r;
        CGPoint c = CGPointZero;
        switch (b.control) {
            case B_SOUTH: c = CGPointMake(face.x, face.y + gap); break;
            case B_EAST: c = CGPointMake(face.x + gap, face.y); break;
            case B_WEST: c = CGPointMake(face.x - gap, face.y); break;
            case B_NORTH: c = CGPointMake(face.x, face.y - gap); break;
            case C_RT: c = CGPointMake(right - 52, top + 34); rad = 26; break;
            case B_RB: c = CGPointMake(right - 52, top + 96); rad = 26; break;
            case C_LT: c = CGPointMake(left + 52, top + 34); rad = 26; break;
            case B_LB: c = CGPointMake(left + 52, top + 96); rad = 26; break;
            case B_BACK: c = CGPointMake(s.width / 2 - 56, top + 22); rad = 20; break;
            case B_START: c = CGPointMake(s.width / 2 + 56, top + 22); rad = 20; break;
            case B_R3: c = CGPointMake(face.x - gap * 2.1, face.y + gap * 0.9); rad = 22; break;
            case B_L3: c = CGPointMake(left + 150, top + 34); rad = 20; break;
            case D_UP: c = CGPointMake(left + 150, top + 100); rad = 21; break;
            case D_DOWN: c = CGPointMake(left + 150, top + 176); rad = 21; break;
            case D_LEFT: c = CGPointMake(left + 112, top + 138); rad = 21; break;
            case D_RIGHT: c = CGPointMake(left + 188, top + 138); rad = 21; break;
        }
        b.center = c;
        b.radius = rad;
        CGRect box = b.pill ? CGRectMake(c.x - rad * 1.45, c.y - rad * 0.72, rad * 2.9, rad * 1.44) : CGRectMake(c.x - rad, c.y - rad, rad * 2, rad * 2);
        b.shape.path = [UIBezierPath bezierPathWithRoundedRect:box cornerRadius:b.pill ? rad * 0.72 : rad].CGPath;
        CGFloat font = b.label.length > 2 ? 12 : b.pill ? 15 : 22;
        b.text.fontSize = font;
        b.text.font = (__bridge CFTypeRef)[UIFont systemFontOfSize:font weight:UIFontWeightBold];
        b.text.frame = CGRectMake(box.origin.x, c.y - font * 0.62, box.size.width, font * 1.3);
        [self paint:b];
    }
}

- (void)paint:(PadButton *)b {
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    BOOL down = b.holders > 0;
    b.shape.fillColor = (down ? [b.color colorWithAlphaComponent:0.55] : [UIColor colorWithWhite:0 alpha:0.28]).CGColor;
    b.shape.strokeColor = [b.color colorWithAlphaComponent:down ? 0.95 : 0.6].CGColor;
    b.text.foregroundColor = (down ? UIColor.whiteColor : [b.color colorWithAlphaComponent:0.85]).CGColor;
    [CATransaction commit];
}

- (PadButton *)buttonAt:(CGPoint)p {
    PadButton *best = nil;
    CGFloat best_d = 1e9;
    for (PadButton *b in buttons) {
        CGFloat d = hypot(p.x - b.center.x, p.y - b.center.y), reach = b.radius * (b.pill ? 1.6 : 1.25);  // a little beyond what's drawn: thumbs are wide
        if (d < reach && d < best_d) best = b, best_d = d;
    }
    return best;
}

- (void)send {
    unsigned bits = 0;
    float lt = 0, rt = 0;
    for (PadButton *b in buttons) {
        if (!b.holders) continue;
        if (b.control == C_LT) lt = 1;
        else if (b.control == C_RT) rt = 1;
        else bits |= 1u << b.control;
    }
    rt_touch_pad(lx, ly, lt, rt, bits);
}

- (void)showStickAt:(CGPoint)c knob:(CGPoint)k {
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    CGFloat R = 62, kr = 26;
    stick_base.path = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(c.x - R, c.y - R, R * 2, R * 2)].CGPath;
    stick_knob.path = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(k.x - kr, k.y - kr, kr * 2, kr * 2)].CGPath;
    stick_base.hidden = stick_knob.hidden = NO;
    [CATransaction commit];
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    for (UITouch *t in touches) {
        CGPoint p = [t locationInView:self];
        PadButton *b = [self buttonAt:p];
        if (b) {
            [owners setObject:b forKey:t];
            b.holders++;
            [self paint:b];
            [tap impactOccurred];
        } else if (p.x < self.bounds.size.width * 0.42 && !stick_touch) {
            stick_touch = t;
            stick_center = p;
            lx = ly = 0;
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
        if (t == stick_touch) {
            CGFloat R = 62, dx = p.x - stick_center.x, dy = p.y - stick_center.y, d = hypot(dx, dy);
            if (d > R) dx *= R / d, dy *= R / d, d = R;
            float m = d / R;
            m = m < 0.12f ? 0 : (m - 0.12f) / 0.88f;  // a dead zone, so a resting thumb doesn't walk
            lx = d > 0 ? (float)(dx / d) * m : 0;
            ly = d > 0 ? (float)(-dy / d) * m : 0;
            [self showStickAt:stick_center knob:CGPointMake(stick_center.x + dx, stick_center.y + dy)];
        } else if (t == look_touch) {
            // Every touch event's motion, coalesced ones too, so fast swipes turn the camera as far as the thumb went.
            NSArray<UITouch *> *all = [event coalescedTouchesForTouch:t] ?: @[t];
            for (UITouch *c in all) {
                CGPoint q = [c locationInView:self];
                rt_touch_look((q.x - look_last.x) * look_scale, (q.y - look_last.y) * look_scale);
                look_last = q;
            }
        }
    }
    [self send];
}

- (void)lift:(NSSet<UITouch *> *)touches {
    for (UITouch *t in touches) {
        id owner = [owners objectForKey:t];
        if ([owner isKindOfClass:PadButton.class]) {
            PadButton *b = owner;
            if (b.holders > 0) b.holders--;
            [self paint:b];
        }
        if (t == stick_touch) {
            stick_touch = nil;
            lx = ly = 0;
            stick_base.hidden = stick_knob.hidden = YES;
        }
        if (t == look_touch) look_touch = nil;
        [owners removeObjectForKey:t];
    }
    [self send];
}
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { [self lift:touches]; }
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { [self lift:touches]; }

// Everything let go, for when the controls hide.
- (void)releaseAll {
    for (PadButton *b in buttons) {
        b.holders = 0;
        [self paint:b];
    }
    [owners removeAllObjects];
    stick_touch = look_touch = nil;
    lx = ly = 0;
    stick_base.hidden = stick_knob.hidden = YES;
    [self send];
}
@end

// Puts the controls over the game's view, and keeps them hidden while a controller is connected.
static TouchControlsView *controls;
static void update_visibility(void) {
    BOOL pad = rt_controller_connected();
    if (pad && !controls.hidden) [controls releaseAll];
    controls.hidden = pad;
    rt_touch_pad_on(!pad);
    fprintf(stderr, "[touch] %s\n", pad ? "a controller is connected: touch controls hidden" : "touch controls shown");
}
void rt_ios_touch_controls(UIView *over) {
    const char *off = getenv("ARKHAM_TOUCH");
    if (off && *off == '0') return;
    controls = [[TouchControlsView alloc] initWithFrame:over.bounds];
    controls.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [over addSubview:controls];
    for (NSNotificationName n in @[GCControllerDidConnectNotification, GCControllerDidDisconnectNotification])
        [NSNotificationCenter.defaultCenter addObserverForName:n object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *note) { update_visibility(); }];
    update_visibility();
}
