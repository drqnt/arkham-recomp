// The touch controls' own icons, drawn with Core Graphics in the batarang's manner: solid white shapes, some with lines cut out of them, on clear.
// One header for the app (TouchControls.m, as each icon's mask) and for tools/bat_icons_preview.c, which draws them all into a picture on the Mac.
// Each is drawn in a box of 100 x 100 units, y down, scaled to the size asked for.
#pragma once
#include <CoreGraphics/CoreGraphics.h>
#include <math.h>
#include <string.h>

// The batarang: wings with three scallops below each, ears, a pointed tail; centred on cx, cy, s is half its width, y down.
static inline void bat_path_add(CGMutablePathRef p, CGFloat cx, CGFloat cy, CGFloat s) {
#define AT(x, y) cx + (x) * s, cy + (y) * s
    static const CGFloat right[][2] = {{0.06, -0.14}, {0.10, -0.40}, {0.15, -0.12}, {0.32, -0.12}};
    CGPathMoveToPoint(p, NULL, AT(0, -0.14));
    for (int i = 0; i < 4; i++) CGPathAddLineToPoint(p, NULL, AT(right[i][0], right[i][1]));
    CGPathAddQuadCurveToPoint(p, NULL, AT(0.62, -0.16), AT(1.0, -0.36));
    CGPathAddLineToPoint(p, NULL, AT(0.86, 0.04));
    CGPathAddQuadCurveToPoint(p, NULL, AT(0.74, -0.08), AT(0.66, 0.02));
    CGPathAddQuadCurveToPoint(p, NULL, AT(0.52, 0.02), AT(0.46, 0.18));
    CGPathAddQuadCurveToPoint(p, NULL, AT(0.30, 0.10), AT(0.22, 0.24));
    CGPathAddLineToPoint(p, NULL, AT(0, 0.44));
    CGPathAddLineToPoint(p, NULL, AT(-0.22, 0.24));
    CGPathAddQuadCurveToPoint(p, NULL, AT(-0.30, 0.10), AT(-0.46, 0.18));
    CGPathAddQuadCurveToPoint(p, NULL, AT(-0.52, 0.02), AT(-0.66, 0.02));
    CGPathAddQuadCurveToPoint(p, NULL, AT(-0.74, -0.08), AT(-0.86, 0.04));
    CGPathAddLineToPoint(p, NULL, AT(-1.0, -0.36));
    CGPathAddQuadCurveToPoint(p, NULL, AT(-0.62, -0.16), AT(-0.32, -0.12));
    for (int i = 3; i >= 0; i--) CGPathAddLineToPoint(p, NULL, AT(-right[i][0], right[i][1]));
    CGPathCloseSubpath(p);
#undef AT
}

static inline void bi_round(CGContextRef g, CGFloat w) {
    CGContextSetLineWidth(g, w);
    CGContextSetLineCap(g, kCGLineCapRound);
    CGContextSetLineJoin(g, kCGLineJoinRound);
}
static inline void bi_line(CGContextRef g, CGFloat w, CGFloat x0, CGFloat y0, CGFloat x1, CGFloat y1) {
    bi_round(g, w);
    CGContextMoveToPoint(g, x0, y0);
    CGContextAddLineToPoint(g, x1, y1);
    CGContextStrokePath(g);
}
static inline void bi_rrect(CGContextRef g, CGFloat x, CGFloat y, CGFloat w, CGFloat h, CGFloat r) {
    CGPathRef p = CGPathCreateWithRoundedRect(CGRectMake(x, y, w, h), r, r, NULL);
    CGContextAddPath(g, p);
    CGPathRelease(p);
}
static inline void bi_tri(CGContextRef g, CGFloat x0, CGFloat y0, CGFloat x1, CGFloat y1, CGFloat x2, CGFloat y2) {
    CGContextMoveToPoint(g, x0, y0);
    CGContextAddLineToPoint(g, x1, y1);
    CGContextAddLineToPoint(g, x2, y2);
    CGContextClosePath(g);
}
// What's drawn while on is cut out of what's drawn already.
static inline void bi_cut(CGContextRef g, int on) { CGContextSetBlendMode(g, on ? kCGBlendModeClear : kCGBlendModeNormal); }
// Batman's head, the cowl's two ears on it: centred on x, its top at y, w wide.
static inline void bi_head(CGContextRef g, CGFloat x, CGFloat y, CGFloat w) {
    CGFloat e = w * 0.32;  // how far the ears stand up
    CGContextFillEllipseInRect(g, CGRectMake(x - w / 2, y + e, w, w));
    bi_tri(g, x - w * 0.46, y + e + w * 0.40, x - w * 0.40, y, x - w * 0.12, y + e + w * 0.08);
    bi_tri(g, x + w * 0.46, y + e + w * 0.40, x + w * 0.40, y, x + w * 0.12, y + e + w * 0.08);
    CGContextFillPath(g);
}

// Cross: run, the action at hand, glide, evade. Two chevrons forward, as fast as the game's own sprint lines.
static inline void bi_run(CGContextRef g) {
    for (int i = 0; i < 2; i++) {
        CGFloat x = 10 + i * 34;
        CGContextMoveToPoint(g, x, 14);
        CGContextAddLineToPoint(g, x + 22, 14);
        CGContextAddLineToPoint(g, x + 50, 50);
        CGContextAddLineToPoint(g, x + 22, 86);
        CGContextAddLineToPoint(g, x, 86);
        CGContextAddLineToPoint(g, x + 28, 50);
        CGContextClosePath(g);
    }
    CGContextFillPath(g);
}
// Square: strike, the silent takedown. A clenched fist from the front, four knuckles over the hand, the thumb folded across.
static inline void bi_fist(CGContextRef g) {
    bi_rrect(g, 16, 30, 68, 60, 14);
    CGContextFillPath(g);
    for (int i = 0; i < 4; i++) CGContextFillEllipseInRect(g, CGRectMake(16 + i * 17, 16, 17, 26));
    bi_cut(g, 1);
    for (int i = 1; i < 4; i++) bi_line(g, 4, 16 + i * 17, 22, 16 + i * 17, 46);  // between the fingers
    bi_rrect(g, 10, 50, 56, 22, 11);                                                    // the thumb's outline
    CGContextSetLineWidth(g, 5);
    CGContextStrokePath(g);
    bi_cut(g, 0);
}
// Triangle: counter. A lightning bolt, as the game marks the man about to hit.
static inline void bi_bolt(CGContextRef g) {
    CGContextMoveToPoint(g, 58, 2);
    CGContextAddLineToPoint(g, 18, 56);
    CGContextAddLineToPoint(g, 45, 56);
    CGContextAddLineToPoint(g, 34, 98);
    CGContextAddLineToPoint(g, 84, 38);
    CGContextAddLineToPoint(g, 56, 38);
    CGContextAddLineToPoint(g, 74, 2);
    CGContextClosePath(g);
    CGContextFillPath(g);
}
// Circle: the cape's stun, back out. Batman from behind, the cape spread, its hem scalloped like the bat's wings.
static inline void bi_cape(CGContextRef g) {
    CGContextMoveToPoint(g, 50, 30);
    CGContextAddCurveToPoint(g, 26, 30, 10, 54, 2, 92);
    CGContextAddQuadCurveToPoint(g, 16, 78, 28, 88);
    CGContextAddQuadCurveToPoint(g, 39, 76, 50, 90);
    CGContextAddQuadCurveToPoint(g, 61, 76, 72, 88);
    CGContextAddQuadCurveToPoint(g, 84, 78, 98, 92);
    CGContextAddCurveToPoint(g, 90, 54, 74, 30, 50, 30);
    CGContextClosePath(g);
    CGContextFillPath(g);
    bi_head(g, 50, 4, 24);
    bi_cut(g, 1);
    bi_line(g, 3.5, 50, 44, 50, 80);  // the cape's middle fold
    bi_cut(g, 0);
}
// R1: the grapnel. A grappling hook: the rope's ring above, three claws curling up below.
static inline void bi_grapple(CGContextRef g) {
    bi_round(g, 10);
    CGContextMoveToPoint(g, 50, 26);
    CGContextAddLineToPoint(g, 50, 90);
    CGContextStrokePath(g);
    CGContextMoveToPoint(g, 50, 84);
    CGContextAddQuadCurveToPoint(g, 18, 90, 14, 56);
    CGContextMoveToPoint(g, 50, 84);
    CGContextAddQuadCurveToPoint(g, 82, 90, 86, 56);
    CGContextStrokePath(g);
    bi_tri(g, 4, 62, 24, 62, 14, 42);  // the claws' points, up
    bi_tri(g, 96, 62, 76, 62, 86, 42);
    bi_tri(g, 40, 88, 60, 88, 50, 100);
    CGContextFillPath(g);
    CGContextSetLineWidth(g, 8);
    CGContextStrokeEllipseInRect(g, CGRectMake(40, 8, 20, 18));  // the ring
    bi_round(g, 6);  // the rope, off to the top right
    CGContextMoveToPoint(g, 58, 12);
    CGContextAddQuadCurveToPoint(g, 70, 0, 80, 10);
    CGContextAddQuadCurveToPoint(g, 90, 20, 100, 6);
    CGContextStrokePath(g);
}
// R2: crouch. Down onto the ground.
static inline void bi_crouch(CGContextRef g) {
    CGContextMoveToPoint(g, 10, 22);
    CGContextAddLineToPoint(g, 30, 22);
    CGContextAddLineToPoint(g, 50, 44);
    CGContextAddLineToPoint(g, 70, 22);
    CGContextAddLineToPoint(g, 90, 22);
    CGContextAddLineToPoint(g, 50, 66);
    CGContextClosePath(g);
    CGContextFillPath(g);
    bi_rrect(g, 8, 76, 84, 14, 4);
    CGContextFillPath(g);
}
// R3: zoom. A magnifier with a plus.
static inline void bi_zoom(CGContextRef g) {
    bi_round(g, 10);
    CGContextStrokeEllipseInRect(g, CGRectMake(12, 10, 58, 58));
    bi_line(g, 16, 64, 62, 88, 88);
    bi_line(g, 9, 41, 27, 41, 51);
    bi_line(g, 9, 29, 39, 53, 39);
}
// L1: detective mode. The cowl from the front, its eyes cut out.
static inline void bi_cowl(CGContextRef g) {
    CGContextMoveToPoint(g, 18, 96);
    CGContextAddLineToPoint(g, 18, 46);
    CGContextAddQuadCurveToPoint(g, 18, 30, 27, 24);
    CGContextAddLineToPoint(g, 24, 2);
    CGContextAddLineToPoint(g, 41, 19);
    CGContextAddQuadCurveToPoint(g, 50, 17, 59, 19);
    CGContextAddLineToPoint(g, 76, 2);
    CGContextAddLineToPoint(g, 73, 24);
    CGContextAddQuadCurveToPoint(g, 82, 30, 82, 46);
    CGContextAddLineToPoint(g, 82, 96);
    CGContextClosePath(g);
    CGContextFillPath(g);
    bi_cut(g, 1);
    for (int side = -1; side <= 1; side += 2) {  // the eyes, slanting down to the middle
        CGContextMoveToPoint(g, 50 + side * 5, 52);
        CGContextAddLineToPoint(g, 50 + side * 25, 43);
        CGContextAddLineToPoint(g, 50 + side * 26, 51);
        CGContextAddLineToPoint(g, 50 + side * 8, 59);
        CGContextClosePath(g);
    }
    CGContextFillPath(g);
    CGContextMoveToPoint(g, 30, 98);  // where the face shows below the mask: the jaw
    CGContextAddLineToPoint(g, 33, 72);
    CGContextAddLineToPoint(g, 44, 68);
    CGContextAddLineToPoint(g, 50, 74);
    CGContextAddLineToPoint(g, 56, 68);
    CGContextAddLineToPoint(g, 67, 72);
    CGContextAddLineToPoint(g, 70, 98);
    CGContextClosePath(g);
    CGContextFillPath(g);
    bi_cut(g, 0);
}
// L2: aim a gadget. A reticle.
static inline void bi_reticle(CGContextRef g) {
    bi_round(g, 8);
    CGContextStrokeEllipseInRect(g, CGRectMake(18, 18, 64, 64));
    CGContextSetLineCap(g, kCGLineCapButt);
    for (int i = 0; i < 4; i++) {
        CGFloat a = i * (CGFloat)M_PI_2, dx = cos(a), dy = sin(a);
        CGContextMoveToPoint(g, 50 + dx * 48, 50 + dy * 48);
        CGContextAddLineToPoint(g, 50 + dx * 22, 50 + dy * 22);
    }
    CGContextStrokePath(g);
    CGContextFillEllipseInRect(g, CGRectMake(44, 44, 12, 12));
}
// L3: the camera behind Batman. An arrow round his head.
static inline void bi_recenter(CGContextRef g) {
    bi_round(g, 9);
    CGFloat a0 = (CGFloat)(-M_PI * 0.25), a1 = (CGFloat)(M_PI * 1.35);
    CGContextAddArc(g, 50, 54, 36, a0, a1, 0);
    CGContextStrokePath(g);
    // the arrowhead at the arc's start, pointing back along it, so the arrow goes round anticlockwise as drawn
    CGFloat x = 50 + 36 * cos(a0), y = 54 + 36 * sin(a0), tx = sin(a0), ty = -cos(a0);  // tx, ty: backwards along the arc
    bi_tri(g, x + tx * 18, y + ty * 18, x - ty * 14 - tx * 2, y + tx * 14 - ty * 2, x + ty * 14 - tx * 2, y - tx * 14 - ty * 2);
    CGContextFillPath(g);
    bi_head(g, 50, 36, 26);
}
// The map, folded in three.
static inline void bi_map(CGContextRef g) {
    CGContextMoveToPoint(g, 8, 22);
    CGContextAddLineToPoint(g, 36, 10);
    CGContextAddLineToPoint(g, 64, 22);
    CGContextAddLineToPoint(g, 92, 10);
    CGContextAddLineToPoint(g, 92, 78);
    CGContextAddLineToPoint(g, 64, 90);
    CGContextAddLineToPoint(g, 36, 78);
    CGContextAddLineToPoint(g, 8, 90);
    CGContextClosePath(g);
    CGContextFillPath(g);
    bi_cut(g, 1);
    bi_line(g, 5, 36, 14, 36, 76);
    bi_line(g, 5, 64, 26, 64, 86);
    bi_cut(g, 0);
}
static inline void bi_pause(CGContextRef g) {
    bi_rrect(g, 20, 14, 22, 72, 5);
    bi_rrect(g, 58, 14, 22, 72, 5);
    CGContextFillPath(g);
}
// Hide everything for a picture: an eye, struck through.
static inline void bi_hide(CGContextRef g) {
    bi_round(g, 9);
    CGContextMoveToPoint(g, 6, 50);
    CGContextAddQuadCurveToPoint(g, 50, 4, 94, 50);
    CGContextAddQuadCurveToPoint(g, 50, 96, 6, 50);
    CGContextClosePath(g);
    CGContextStrokePath(g);
    CGContextFillEllipseInRect(g, CGRectMake(35, 35, 30, 30));
    bi_cut(g, 1);
    bi_line(g, 22, 14, 86, 86, 14);
    bi_cut(g, 0);
    bi_line(g, 9, 14, 86, 86, 14);
}
static inline void bi_close(CGContextRef g) {
    bi_line(g, 14, 22, 22, 78, 78);
    bi_line(g, 14, 78, 22, 22, 78);
}
static inline void bi_batarang(CGContextRef g) {
    CGMutablePathRef p = CGPathCreateMutable();
    bat_path_add(p, 50, 52, 50);
    CGContextAddPath(g, p);
    CGPathRelease(p);
    CGContextFillPath(g);
}



// Draws the icon for a button's ident into g, white, in a box of size x size with its top left at the origin and y down; 0 if there's none.
static inline int bat_icon_draw(CGContextRef g, const char *ident, CGFloat size) {
    static const struct {
        const char *ident;
        void (*draw)(CGContextRef);
    } icons[] = {
        {"cross", bi_run},   {"square", bi_fist},  {"triangle", bi_bolt}, {"circle", bi_cape}, {"r1", bi_grapple},  {"r2", bi_crouch},  {"r3", bi_zoom},
        {"l1", bi_cowl},     {"l2", bi_reticle},   {"l3", bi_recenter},   {"map", bi_map},     {"pause", bi_pause}, {"photo", bi_hide}, {"close", bi_close},
        {"gadgets", bi_batarang},
    };
    for (size_t i = 0; i < sizeof icons / sizeof icons[0]; i++) {
        if (strcmp(icons[i].ident, ident)) continue;
        CGContextSaveGState(g);
        CGContextScaleCTM(g, size / 100, size / 100);
        CGContextSetRGBFillColor(g, 1, 1, 1, 1);
        CGContextSetRGBStrokeColor(g, 1, 1, 1, 1);
        icons[i].draw(g);
        CGContextRestoreGState(g);
        return 1;
    }
    return 0;
}
