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
// Square: strike, the silent takedown. A clenched fist raised, from the palm's side: four curled fingers, the thumb across them, the wrist in
// the glove's cuff.
static inline void bi_fist(CGContextRef g) {
    static const CGFloat f[4][3] = {{20, 17, 17}, {37, 11, 18}, {55, 14, 17}, {72, 23, 14}};  // each finger's left, top, width; the middle one highest
    for (int i = 0; i < 4; i++) bi_rrect(g, f[i][0], f[i][1], f[i][2], 60 - f[i][1], 6.5);
    CGContextMoveToPoint(g, 20, 40);  // the palm, the thumb's root bulging out at the left, down into the wrist
    CGContextAddLineToPoint(g, 86, 40);
    CGContextAddLineToPoint(g, 86, 62);
    CGContextAddCurveToPoint(g, 86, 72, 79, 77, 72, 79);
    CGContextAddLineToPoint(g, 72, 98);
    CGContextAddLineToPoint(g, 31, 98);
    CGContextAddLineToPoint(g, 31, 80);
    CGContextAddCurveToPoint(g, 19, 76, 12, 66, 13, 52);
    CGContextAddLineToPoint(g, 13, 46);
    CGContextClosePath(g);
    CGContextFillPath(g);
    bi_cut(g, 1);
    for (int i = 1; i < 4; i++) bi_line(g, 4.5, f[i][0], f[i][1] + 8, f[i][0], i < 3 ? 41 : 54);  // between the fingers, down to the thumb
    bi_round(g, 4.5);  // the thumb's top, in from the hand's edge, and round its tip
    CGContextMoveToPoint(g, 10, 41);
    CGContextAddLineToPoint(g, 52, 41);
    CGContextAddCurveToPoint(g, 59, 41, 62, 45, 62, 50);
    CGContextAddCurveToPoint(g, 62, 55, 58, 59, 52, 59);
    CGContextAddLineToPoint(g, 47, 59);
    CGContextStrokePath(g);
    bi_line(g, 4.5, 26, 86, 78, 86);  // the cuff
    bi_cut(g, 0);
}
// Triangle: counter. A shield, and the blow that comes at it sent straight back.
static inline void bi_shield(CGContextRef g) {
    CGContextMoveToPoint(g, 2, 6);  // the shield: its top dipping a little to the middle, the sides straight, then in to the point
    CGContextAddQuadCurveToPoint(g, 27, 14.8, 52, 6);
    CGContextAddLineToPoint(g, 52, 43);
    CGContextAddCurveToPoint(g, 52, 69.4, 38, 85.2, 27, 94);
    CGContextAddCurveToPoint(g, 16, 85.2, 2, 69.4, 2, 43);
    CGContextClosePath(g);
    CGContextFillPath(g);
    bi_round(g, 13);  // the blow: in from the bottom right to the shield's face, and back out, up and to the right
    CGContextMoveToPoint(g, 90, 82);
    CGContextAddLineToPoint(g, 64, 56);
    CGContextAddLineToPoint(g, 79, 36);
    CGContextStrokePath(g);
    bi_tri(g, 94, 16, 92.6, 46.2, 65.4, 25.8);  // its head, along the way out
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
// The map's "Show Batman": the bat in a target's ring, with its four ticks.
static inline void bi_findbat(CGContextRef g) {
    bi_round(g, 7);
    CGContextStrokeEllipseInRect(g, CGRectMake(14, 14, 72, 72));
    CGContextSetLineCap(g, kCGLineCapButt);
    for (int i = 0; i < 4; i++) {
        CGFloat a = i * (CGFloat)M_PI_2, dx = cos(a), dy = sin(a);
        CGContextMoveToPoint(g, 50 + dx * 50, 50 + dy * 50);
        CGContextAddLineToPoint(g, 50 + dx * 38, 50 + dy * 38);
    }
    CGContextStrokePath(g);
    CGMutablePathRef p = CGPathCreateMutable();
    bat_path_add(p, 50, 52, 27);
    CGContextAddPath(g, p);
    CGPathRelease(p);
    CGContextFillPath(g);
}
// Both sticks at once, for the cryptographic sequencer: a pad, its two sticks cut out of it with their knobs in them.
static inline void bi_twosticks(CGContextRef g) {
    CGContextMoveToPoint(g, 27, 22);
    CGContextAddLineToPoint(g, 73, 22);
    CGContextAddCurveToPoint(g, 88, 22, 94, 46, 96, 70);    // the right side, down into its grip
    CGContextAddCurveToPoint(g, 97, 82, 88, 88, 80, 82);
    CGContextAddCurveToPoint(g, 74, 76, 70, 68, 64, 68);    // the right grip's inside
    CGContextAddLineToPoint(g, 36, 68);
    CGContextAddCurveToPoint(g, 30, 68, 26, 76, 20, 82);
    CGContextAddCurveToPoint(g, 12, 88, 3, 82, 4, 70);
    CGContextAddCurveToPoint(g, 6, 46, 12, 22, 27, 22);
    CGContextFillPath(g);
    bi_cut(g, 1);
    CGContextFillEllipseInRect(g, CGRectMake(20, 33, 28, 28));
    CGContextFillEllipseInRect(g, CGRectMake(52, 33, 28, 28));
    bi_cut(g, 0);
    CGContextFillEllipseInRect(g, CGRectMake(26.5, 39.5, 15, 15));
    CGContextFillEllipseInRect(g, CGRectMake(58.5, 39.5, 15, 15));
}



// The two-button moves, one touch each. Special Combo Takedown (triangle and circle): a lightning bolt.
static inline void bi_combo_takedown(CGContextRef g) {
    CGContextMoveToPoint(g, 62, 4);
    CGContextAddLineToPoint(g, 20, 56);
    CGContextAddLineToPoint(g, 46, 56);
    CGContextAddLineToPoint(g, 36, 96);
    CGContextAddLineToPoint(g, 80, 40);
    CGContextAddLineToPoint(g, 54, 40);
    CGContextAddLineToPoint(g, 66, 4);
    CGContextClosePath(g);
    CGContextFillPath(g);
}
// Special Combo Throw (circle and cross): an arrow up and over.
static inline void bi_combo_throw(CGContextRef g) {
    bi_round(g, 12);
    CGContextMoveToPoint(g, 22, 86);
    CGContextAddLineToPoint(g, 22, 46);
    CGContextAddArc(g, 50, 46, 28, (CGFloat)M_PI, 0, 0);
    CGContextAddLineToPoint(g, 78, 56);
    CGContextStrokePath(g);
    bi_tri(g, 60, 54, 96, 54, 78, 84);
    CGContextFillPath(g);
}
// Ground Takedown (R2 and triangle): an arrow down onto the floor.
static inline void bi_combo_ground(CGContextRef g) {
    bi_line(g, 16, 50, 8, 50, 50);
    bi_tri(g, 24, 46, 76, 46, 50, 76);
    CGContextFillPath(g);
    bi_rrect(g, 12, 84, 76, 12, 4);
    CGContextFillPath(g);
}

// The gadgets, for the wheel's eight directions (TouchControls.m), drawn after the game's own wheel in the owner's picture of it: each traced in a
// square of that picture, in its own 100 units (the _raw drawings), then fitted to the box. Solid, with the game's lines cut out of them.
static inline void bi_fit(CGContextRef g, CGFloat x0, CGFloat y0, CGFloat x1, CGFloat y1) {
    CGFloat s = 94 / fmax(x1 - x0, y1 - y0);
    CGContextTranslateCTM(g, 50 - s * (x0 + x1) / 2, 50 - s * (y0 + y1) / 2);
    CGContextScaleCTM(g, s, s);
}
// A dashed line, as the game draws sound and flight.
static inline void bi_dashes(CGContextRef g, CGFloat w, CGFloat x0, CGFloat y0, CGFloat x1, CGFloat y1) {
    CGFloat d[] = {w * 1.3, w * 0.9};
    CGContextSetLineDash(g, 0, d, 2);
    CGContextSetLineWidth(g, w);
    CGContextSetLineCap(g, kCGLineCapButt);
    CGContextMoveToPoint(g, x0, y0);
    CGContextAddLineToPoint(g, x1, y1);
    CGContextStrokePath(g);
    CGContextSetLineDash(g, 0, NULL, 0);
}
static inline void bi_arrowhead(CGContextRef g, CGFloat w, CGFloat x0, CGFloat y0, CGFloat x1, CGFloat y1, CGFloat x2, CGFloat y2) {
    bi_round(g, w);
    CGContextMoveToPoint(g, x0, y0);
    CGContextAddLineToPoint(g, x1, y1);
    CGContextAddLineToPoint(g, x2, y2);
    CGContextStrokePath(g);
}
// The game's batarang, flat, its disc about (50, 53.5): long wings swept up to a point beside the disc and out to a point below.
static inline void game_bat_outline(CGMutablePathRef p, const CGAffineTransform *t) {
    CGPathMoveToPoint(p, t, 50, 45.5);
    CGPathAddQuadCurveToPoint(p, t, 45.5, 45.8, 43.5, 49.5);
    CGPathAddQuadCurveToPoint(p, t, 38, 47.5, 35.5, 36.5);
    CGPathAddQuadCurveToPoint(p, t, 21, 45.5, 3.5, 63.5);
    CGPathAddQuadCurveToPoint(p, t, 22, 52, 40, 57.5);
    CGPathAddQuadCurveToPoint(p, t, 45, 61.8, 50, 61.8);
    CGPathAddQuadCurveToPoint(p, t, 55, 61.8, 60, 57.5);
    CGPathAddQuadCurveToPoint(p, t, 78, 52, 96.5, 63.5);
    CGPathAddQuadCurveToPoint(p, t, 79, 45.5, 64.5, 36.5);
    CGPathAddQuadCurveToPoint(p, t, 62, 47.5, 56.5, 49.5);
    CGPathAddQuadCurveToPoint(p, t, 54.5, 45.8, 50, 45.5);
    CGPathCloseSubpath(p);
}
// One batarang, moved by t: with a gap cut round it first (gap wide, 0 for none) where it lies over others, and with its disc's ring and four
// rivets cut out of it if it's in front.
static inline void bi_game_bat(CGContextRef g, CGAffineTransform t, CGFloat gap, int front) {
    CGMutablePathRef body = CGPathCreateMutable(), disc = CGPathCreateMutable();
    game_bat_outline(body, &t);
    CGPathAddEllipseInRect(disc, &t, CGRectMake(40.5, 44, 19, 19));
    if (gap > 0) {
        bi_cut(g, 1);
        bi_round(g, gap);
        CGContextAddPath(g, body);
        CGContextAddPath(g, disc);
        CGContextStrokePath(g);
        bi_cut(g, 0);
    }
    CGContextAddPath(g, body);
    CGContextFillPath(g);
    CGContextAddPath(g, disc);
    CGContextFillPath(g);
    CGPathRelease(body);
    CGPathRelease(disc);
    if (!front) return;
    bi_cut(g, 1);
    CGMutablePathRef ring = CGPathCreateMutable();
    CGPathAddEllipseInRect(ring, &t, CGRectMake(43.6, 47.1, 12.8, 12.8));
    CGContextAddPath(g, ring);
    CGContextSetLineWidth(g, 2.1);
    CGContextStrokePath(g);
    CGPathRelease(ring);
    static const CGFloat rivets[][2] = {{38.5, 54.5}, {43.5, 57.3}, {56.5, 57.3}, {61.5, 54.5}};
    for (int i = 0; i < 4; i++) {
        CGPoint c = CGPointApplyAffineTransform(CGPointMake(rivets[i][0], rivets[i][1]), t);
        CGContextFillEllipseInRect(g, CGRectMake(c.x - 1.7, c.y - 1.7, 3.4, 3.4));
    }
    bi_cut(g, 0);
}
// Up: the Batarang.
static inline void bi_g_batarang(CGContextRef g) {
    bi_fit(g, 3.5, 36.5, 96.5, 63.5);
    bi_game_bat(g, CGAffineTransformIdentity, 0, 1);
}
// Up and left: several Batarangs at once, seen tilted: three blades stacked out to the left, the front one's disc, and its wing down to the right
// with the points of the ones behind it. Each blade over another has a gap cut round it first.
static inline void bi_g_batarangs_cut_round(CGContextRef g, CGPathRef p) {
    bi_cut(g, 1);
    bi_round(g, 3.2);
    CGContextAddPath(g, p);
    CGContextStrokePath(g);
    bi_cut(g, 0);
    CGContextAddPath(g, p);
    CGContextFillPath(g);
}
static inline void bi_g_batarangs_raw(CGContextRef g) {
    CGMutablePathRef top = CGPathCreateMutable(), mid = CGPathCreateMutable(), front = CGPathCreateMutable(), right = CGPathCreateMutable();
    CGPathMoveToPoint(top, NULL, 8, 20);
    CGPathAddQuadCurveToPoint(top, NULL, 30.5, 6.5, 53.5, 19.5);
    CGPathAddLineToPoint(top, NULL, 47, 24);
    CGPathAddQuadCurveToPoint(top, NULL, 30, 20.5, 16, 21.5);
    CGPathCloseSubpath(top);
    CGPathMoveToPoint(mid, NULL, 4, 31);
    CGPathAddQuadCurveToPoint(mid, NULL, 10, 23, 20, 22);
    CGPathAddLineToPoint(mid, NULL, 47, 22);
    CGPathAddLineToPoint(mid, NULL, 47, 27);
    CGPathAddLineToPoint(mid, NULL, 29, 27.5);
    CGPathAddQuadCurveToPoint(mid, NULL, 22, 31, 17, 31);
    CGPathCloseSubpath(mid);
    CGPathMoveToPoint(front, NULL, 3, 42.5);
    CGPathAddQuadCurveToPoint(front, NULL, 9, 34, 17, 31.5);
    CGPathAddLineToPoint(front, NULL, 29, 28.5);
    CGPathAddLineToPoint(front, NULL, 47, 28);
    CGPathAddLineToPoint(front, NULL, 52, 30);
    CGPathAddQuadCurveToPoint(front, NULL, 61, 33, 63.5, 45);
    CGPathAddLineToPoint(front, NULL, 66, 59);
    CGPathAddQuadCurveToPoint(front, NULL, 60, 61, 56, 60);
    CGPathAddQuadCurveToPoint(front, NULL, 50, 58.5, 46, 55);
    CGPathAddLineToPoint(front, NULL, 43, 54);
    CGPathAddLineToPoint(front, NULL, 42, 49.5);
    CGPathAddQuadCurveToPoint(front, NULL, 38, 45, 33, 42);
    CGPathAddQuadCurveToPoint(front, NULL, 28, 40, 25, 40);
    CGPathAddQuadCurveToPoint(front, NULL, 12, 41, 3, 42.5);
    CGPathCloseSubpath(front);
    CGPathMoveToPoint(right, NULL, 62, 46);
    CGPathAddLineToPoint(right, NULL, 76, 38.5);
    CGPathAddQuadCurveToPoint(right, NULL, 92, 55, 96.5, 79);
    CGPathAddLineToPoint(right, NULL, 91.5, 74.5);
    CGPathAddLineToPoint(right, NULL, 89.5, 84);
    CGPathAddLineToPoint(right, NULL, 86, 76.5);
    CGPathAddLineToPoint(right, NULL, 83.5, 85.5);
    CGPathAddLineToPoint(right, NULL, 80, 74);
    CGPathAddQuadCurveToPoint(right, NULL, 77, 63, 66, 59);
    CGPathCloseSubpath(right);
    CGContextAddPath(g, top);
    CGContextFillPath(g);
    bi_g_batarangs_cut_round(g, mid);
    bi_g_batarangs_cut_round(g, front);
    CGContextAddPath(g, right);
    CGContextFillPath(g);
    CGPathRelease(top), CGPathRelease(mid), CGPathRelease(front), CGPathRelease(right);
    bi_cut(g, 1);
    CGContextSetLineWidth(g, 2.1);
    CGContextStrokeEllipseInRect(g, CGRectMake(52.5, 44.5, 12, 12));  // the disc's ring
    static const CGFloat rivets[][2] = {{45.6, 34.6}, {43.2, 41.2}, {43.8, 46.6}, {64, 53.5}, {68.5, 53.5}};
    for (int i = 0; i < 5; i++) CGContextFillEllipseInRect(g, CGRectMake(rivets[i][0] - 1.6, rivets[i][1] - 1.6, 3.2, 3.2));
    bi_cut(g, 0);
}
static inline void bi_g_batarangs(CGContextRef g) { bi_fit(g, 3, 12.5, 97, 86); bi_g_batarangs_raw(g); }
// Up and right: the Sonic Batarang, two blades curving off a disc, sound dashed out to both sides.
static inline void bi_g_sonic_raw(CGContextRef g) {
    CGContextFillEllipseInRect(g, CGRectMake(36.5, 37.5, 31, 31));
    CGContextMoveToPoint(g, 47, 38.5);
    CGContextAddCurveToPoint(g, 58, 26, 72, 18, 86, 21);
    CGContextAddCurveToPoint(g, 91, 22, 95, 26, 97.5, 30.5);
    CGContextAddCurveToPoint(g, 90, 28.5, 80, 30, 73, 34);
    CGContextAddCurveToPoint(g, 69, 36.5, 66.5, 40, 65.5, 43);
    CGContextClosePath(g);
    CGContextFillPath(g);
    CGContextMoveToPoint(g, 57, 68.5);
    CGContextAddCurveToPoint(g, 47, 79, 30, 84, 18, 79);
    CGContextAddCurveToPoint(g, 11, 76.5, 5, 73, 2, 70);
    CGContextAddCurveToPoint(g, 10, 69.5, 22, 69, 30, 67);
    CGContextAddCurveToPoint(g, 34, 65.5, 37, 63, 38.5, 60);
    CGContextClosePath(g);
    CGContextFillPath(g);
    bi_cut(g, 1);
    bi_round(g, 2.6);
    CGContextAddArc(g, 52, 53, 9.5, -1.1, 1.25, 0);  // the crescent in the disc
    CGContextStrokePath(g);
    bi_cut(g, 0);
    bi_dashes(g, 2.8, 35, 32, 27, 21.5);
    bi_dashes(g, 2.8, 31, 39, 13.5, 32.5);
    bi_dashes(g, 2.8, 29, 48, 12.5, 48);
    bi_dashes(g, 2.8, 75, 49, 92.5, 49);
    bi_dashes(g, 2.8, 73, 59, 90.5, 66.5);
    bi_dashes(g, 2.8, 67.5, 66, 77.5, 77.5);
}
static inline void bi_g_sonic(CGContextRef g) { bi_fit(g, 2, 18, 98, 85); bi_g_sonic_raw(g); }
// Right: the Line Launcher, a drum with its barrel up, a handle to the right and jaws down to the left, the line dashed out of the barrel.
static inline void bi_g_line_raw(CGContextRef g) {
    bi_rrect(g, 45.5, 4, 15.5, 22, 1.5);
    CGContextFillPath(g);
    CGContextFillEllipseInRect(g, CGRectMake(25.5, 24.5, 55, 55));
    CGContextMoveToPoint(g, 77, 37.5);
    CGContextAddLineToPoint(g, 93, 32.5);
    CGContextAddLineToPoint(g, 96.5, 45.5);
    CGContextAddLineToPoint(g, 79, 51);
    CGContextClosePath(g);
    CGContextFillPath(g);
    CGContextMoveToPoint(g, 8, 66);
    CGContextAddLineToPoint(g, 16, 57);
    CGContextAddLineToPoint(g, 29, 54.5);
    CGContextAddLineToPoint(g, 42, 60);
    CGContextAddLineToPoint(g, 52, 70);
    CGContextAddLineToPoint(g, 54.5, 81);
    CGContextAddLineToPoint(g, 49, 89);
    CGContextAddLineToPoint(g, 38.5, 93.5);
    CGContextAddLineToPoint(g, 20, 86.5);
    CGContextAddLineToPoint(g, 10, 78.5);
    CGContextClosePath(g);
    CGContextFillPath(g);
    bi_cut(g, 1);
    CGContextSetLineWidth(g, 2.4);
    CGContextStrokeEllipseInRect(g, CGRectMake(43.5, 42.5, 19, 19));  // the hub
    bi_round(g, 2.2);
    CGContextAddArc(g, 53, 52, 21, -2.7, 0.95, 0);  // the drum's rim, inside it
    CGContextStrokePath(g);
    for (int i = 0; i < 6; i++) {  // its teeth
        CGFloat a = -1.25 + i * 0.36;
        bi_line(g, 2.2, 53 + cos(a) * 13.5, 52 + sin(a) * 13.5, 53 + cos(a) * 17.5, 52 + sin(a) * 17.5);
    }
    bi_round(g, 2.6);
    CGContextMoveToPoint(g, 12, 72.5);  // the jaws' mouth, and their two holes
    CGContextAddQuadCurveToPoint(g, 32, 72, 52, 80);
    CGContextStrokePath(g);
    CGContextFillEllipseInRect(g, CGRectMake(19.5, 59.5, 5, 8));
    CGContextFillEllipseInRect(g, CGRectMake(34, 63.5, 6, 9));
    bi_cut(g, 0);
    bi_dashes(g, 2.8, 73, 22.5, 83, 9.5);
    bi_arrowhead(g, 2.8, 78.5, 8, 86, 7, 85.5, 14.5);
}
static inline void bi_g_line(CGContextRef g) { bi_fit(g, 8, 4, 97, 94); bi_g_line_raw(g); }
// Down and right: the Cryptographic Sequencer, a handheld tilted to the right, its screen with a wave in it, its grip down to the left.
static inline void bi_g_sequencer_raw(CGContextRef g) {
    static const CGFloat outline[][2] = {{25, 7.5}, {75, 36.5}, {81, 36}, {87.5, 40}, {88.5, 47}, {84.5, 56}, {56, 96.5}, {50.5, 97},
                                         {31, 78.5}, {49, 55}, {27, 42}, {18, 34}, {14, 24}, {15.5, 16}, {21, 9}};
    CGContextMoveToPoint(g, outline[0][0], outline[0][1]);
    for (size_t i = 1; i < sizeof outline / sizeof outline[0]; i++) CGContextAddLineToPoint(g, outline[i][0], outline[i][1]);
    CGContextClosePath(g);
    bi_round(g, 3);
    CGContextDrawPath(g, kCGPathFillStroke);  // filled, its corners rounded by the stroke
    CGContextSaveGState(g);  // the screen, cut out along the device, and the wave on it
    CGContextTranslateCTM(g, 53, 37);
    CGContextRotateCTM(g, (CGFloat)M_PI / 6);
    bi_cut(g, 1);
    bi_rrect(g, -17, -7.5, 34, 15, 1.5);
    CGContextFillPath(g);
    bi_cut(g, 0);
    bi_round(g, 2.4);
    CGContextMoveToPoint(g, -13, 2);
    CGContextAddCurveToPoint(g, -8, -9, -4, -9, 0, 0);
    CGContextAddCurveToPoint(g, 4, 9, 8, 9, 13, -2);
    CGContextStrokePath(g);
    CGContextRestoreGState(g);
    bi_cut(g, 1);
    bi_round(g, 2.2);
    CGContextAddArc(g, 29.5, 18.5, 4.5, 0.7, 5.6, 0);  // a dial
    CGContextStrokePath(g);
    bi_line(g, 2.2, 50.5, 57, 82.5, 57.5);  // where the grip meets the body
    bi_cut(g, 0);
}
static inline void bi_g_sequencer(CGContextRef g) { bi_fit(g, 12.5, 6, 90, 98.5); bi_g_sequencer_raw(g); }
// Down: the Batclaw, a head at the back with two blades swept forward from it, above and below, and the body between them out to a point.
static inline void bi_g_batclaw_raw(CGContextRef g) {
    CGContextFillEllipseInRect(g, CGRectMake(6, 39.5, 23, 22));
    CGContextMoveToPoint(g, 13, 41);
    CGContextAddCurveToPoint(g, 22, 30, 42, 20, 68, 18.5);
    CGContextAddCurveToPoint(g, 55, 22, 40, 29, 31, 44);
    CGContextClosePath(g);
    CGContextFillPath(g);
    CGContextMoveToPoint(g, 13, 60);
    CGContextAddCurveToPoint(g, 22, 72, 42, 85, 66.5, 89);
    CGContextAddCurveToPoint(g, 54, 84, 40, 75, 31, 57.5);
    CGContextClosePath(g);
    CGContextFillPath(g);
    CGContextMoveToPoint(g, 27, 44);
    CGContextAddLineToPoint(g, 52.5, 42.5);
    CGContextAddLineToPoint(g, 56.5, 37);
    CGContextAddCurveToPoint(g, 75, 36, 88, 40, 96, 50);
    CGContextAddCurveToPoint(g, 88, 60, 75, 64.5, 56.5, 66.5);
    CGContextAddLineToPoint(g, 52.5, 58);
    CGContextAddLineToPoint(g, 27, 57);
    CGContextClosePath(g);
    CGContextFillPath(g);
    bi_cut(g, 1);
    bi_line(g, 2.2, 44, 47.5, 88, 49);  // the body's grooves
    bi_line(g, 2.2, 44, 53, 88, 51.5);
    CGContextSetLineWidth(g, 2.2);
    CGContextStrokeEllipseInRect(g, CGRectMake(11, 44.5, 12, 12));  // a ring on the head
    bi_cut(g, 0);
}
static inline void bi_g_batclaw(CGContextRef g) { bi_fit(g, 6, 17, 96.5, 90); bi_g_batclaw_raw(g); }
// Down and left: the Remote Control Batarang, tilted, its flight steered both ways in dashed arrows.
static inline void bi_g_remote_raw(CGContextRef g) {
    CGContextFillEllipseInRect(g, CGRectMake(40, 40, 19, 19));
    CGContextMoveToPoint(g, 41.5, 46);
    CGContextAddQuadCurveToPoint(g, 33, 50.5, 25.5, 49);
    CGContextAddLineToPoint(g, 21.5, 42);
    CGContextAddCurveToPoint(g, 13, 52, 6, 66, 9, 89.5);
    CGContextAddCurveToPoint(g, 16, 76, 27, 66.5, 38.5, 61.5);
    CGContextAddLineToPoint(g, 43, 57.5);
    CGContextClosePath(g);
    CGContextFillPath(g);
    CGContextMoveToPoint(g, 53.5, 41);
    CGContextAddLineToPoint(g, 49, 22.5);
    CGContextAddCurveToPoint(g, 62, 17, 82, 17.5, 96.5, 27);
    CGContextAddCurveToPoint(g, 82, 29, 70, 36, 62.5, 45.5);
    CGContextAddLineToPoint(g, 58.5, 52);
    CGContextClosePath(g);
    CGContextFillPath(g);
    bi_cut(g, 1);
    CGContextSetLineWidth(g, 2.1);
    CGContextStrokeEllipseInRect(g, CGRectMake(43.1, 43.1, 12.8, 12.8));
    static const CGFloat rivets[][2] = {{32.5, 54}, {38.5, 52.5}, {55.7, 37}, {60, 42}};
    for (int i = 0; i < 4; i++) CGContextFillEllipseInRect(g, CGRectMake(rivets[i][0] - 1.7, rivets[i][1] - 1.7, 3.4, 3.4));
    bi_cut(g, 0);
    bi_dashes(g, 2.8, 36.5, 34, 29.5, 22.5);
    bi_dashes(g, 2.8, 57, 62, 65, 75);
    bi_arrowhead(g, 2.8, 26.5, 27, 26, 18.5, 33.5, 20);
    bi_arrowhead(g, 2.8, 61.5, 78.5, 69, 79, 70, 71.5);
}
static inline void bi_g_remote(CGContextRef g) { bi_fit(g, 7, 17, 97, 90.5); bi_g_remote_raw(g); }
// Left: Explosive Gel, its spray gun, the canister up to the right, the grip down to the right, the gel dashed out of the nozzle.
static inline void bi_g_gel_raw(CGContextRef g) {
    bi_line(g, 13.5, 46, 33, 66.5, 19.5);
    CGContextMoveToPoint(g, 30, 41);
    CGContextAddLineToPoint(g, 30, 36.5);
    CGContextAddLineToPoint(g, 40.5, 27);
    CGContextAddLineToPoint(g, 49, 27);
    CGContextAddLineToPoint(g, 56, 40);
    CGContextAddLineToPoint(g, 57, 60);
    CGContextAddLineToPoint(g, 48.5, 61.5);
    CGContextAddLineToPoint(g, 48.5, 66.5);
    CGContextAddLineToPoint(g, 45.5, 69);
    CGContextAddLineToPoint(g, 32.5, 68.5);
    CGContextAddLineToPoint(g, 30.5, 65);
    CGContextAddLineToPoint(g, 30.5, 59);
    CGContextClosePath(g);
    CGContextFillPath(g);
    bi_rrect(g, 23.5, 43, 10, 15.5, 5);
    CGContextFillPath(g);
    CGContextFillEllipseInRect(g, CGRectMake(59.5, 41.5, 13, 13));  // the joint
    CGContextFillEllipseInRect(g, CGRectMake(60, 35, 7, 7));  // a knob on top
    bi_line(g, 13, 61, 55, 89, 76.5);  // the grip
    bi_line(g, 4.5, 57, 62, 58.5, 71.5);  // the trigger
    bi_cut(g, 1);
    bi_line(g, 2.2, 32, 41, 54, 41);  // the body's top face
    CGContextSetLineWidth(g, 2.2);
    CGContextStrokeEllipseInRect(g, CGRectMake(25.5, 46.5, 5, 9));  // the nozzle's mouth
    CGContextStrokeEllipseInRect(g, CGRectMake(61.5, 43.5, 9, 9));  // the joint's ring
    bi_cut(g, 0);
    bi_dashes(g, 2.8, 22, 46, 9, 41.5);
    bi_dashes(g, 2.8, 21, 50.5, 4, 50.5);
    bi_dashes(g, 2.8, 22, 55, 9, 60);
}
static inline void bi_g_gel(CGContextRef g) { bi_fit(g, 4, 12, 96, 85); bi_g_gel_raw(g); }
// The gadgets' button, the wheel closed: the pad's cross as the game's wheel has it in its middle, four keys pointing in, and around it the game's
// eight arrows pointing out, for "hold and slide".
static inline void bi_dpad(CGContextRef g) {
    for (int i = 0; i < 8; i++) {
        CGContextSaveGState(g);
        CGContextTranslateCTM(g, 50, 50);
        CGContextRotateCTM(g, i * (CGFloat)M_PI_4);
        if (i % 2 == 0) {  // a key, its point in
            CGContextMoveToPoint(g, 0, -8);
            CGContextAddLineToPoint(g, 9.5, -16);
            CGContextAddLineToPoint(g, 9.5, -33);
            CGContextAddLineToPoint(g, -9.5, -33);
            CGContextAddLineToPoint(g, -9.5, -16);
            CGContextClosePath(g);
            bi_round(g, 3);
            CGContextDrawPath(g, kCGPathFillStroke);
            bi_tri(g, -6, -40.5, 6, -40.5, 0, -48);  // the arrow beyond it
        } else {
            bi_tri(g, -4.5, -38, 4.5, -38, 0, -44.5);  // a smaller one between
        }
        CGContextFillPath(g);
        CGContextRestoreGState(g);
    }
}

// Draws the icon for a button's ident into g, white, in a box of size x size with its top left at the origin and y down; 0 if there's none.
static inline int bat_icon_draw(CGContextRef g, const char *ident, CGFloat size) {
    static const struct {
        const char *ident;
        void (*draw)(CGContextRef);
    } icons[] = {
        {"cross", bi_run},   {"square", bi_fist},  {"triangle", bi_shield}, {"circle", bi_cape}, {"r1", bi_grapple},  {"r2", bi_crouch},  {"r3", bi_zoom},
        {"l1", bi_cowl},     {"l2", bi_reticle},   {"l3", bi_recenter},     {"map", bi_map},     {"pause", bi_pause}, {"photo", bi_hide}, {"close", bi_close},
        {"gadgets", bi_dpad}, {"r2aim", bi_g_batarang}, {"findbat", bi_findbat}, {"twosticks", bi_twosticks}, {"combo_td", bi_combo_takedown},
        {"combo_throw", bi_combo_throw}, {"combo_ground", bi_combo_ground},
        {"g_batarang", bi_g_batarang}, {"g_sonic", bi_g_sonic}, {"g_line", bi_g_line}, {"g_sequencer", bi_g_sequencer},
        {"g_batclaw", bi_g_batclaw}, {"g_remote", bi_g_remote}, {"g_gel", bi_g_gel}, {"g_batarangs", bi_g_batarangs},
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
