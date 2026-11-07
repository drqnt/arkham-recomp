// CoreText and the CoreGraphics text and path calls. The game draws its D3DX-style fonts with these into bitmaps.
// CGFloat is a float on the guest and a double here; CGRect-sized results come back through a hidden pointer the
// callee pops.
#include "shim.h"
#include "shim_reg.h"

#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <CoreGraphics/CoreGraphics.h>
#else
#include <ApplicationServices/ApplicationServices.h>
#endif
#include <CoreText/CoreText.h>
#include <stdio.h>
#include <stdlib.h>

extern u32 cf_wrap(CFTypeRef o);
extern CFTypeRef cf_unwrap(u32 g);

#define OBJ(n) cf_unwrap(ARG(n))
#define FONT(n) ((CTFontRef)OBJ(n))
#define CTX(n) ((CGContextRef)OBJ(n))
#define PATH(n) ((CGMutablePathRef)OBJ(n))

static void put_rect(u32 g, CGRect r) {
    *(f32a *)GP(g) = (float)r.origin.x;
    *(f32a *)GP(g + 4) = (float)r.origin.y;
    *(f32a *)GP(g + 8) = (float)r.size.width;
    *(f32a *)GP(g + 12) = (float)r.size.height;
}
static CGAffineTransform get_transform(u32 g) {
    const f32a *f = GP(g);
    return CGAffineTransformMake(f[0], f[1], f[2], f[3], f[4], f[5]);
}
static CGAffineTransform arg_transform(CPU *c, int n) {
    return CGAffineTransformMake(ARGF(n), ARGF(n + 1), ARGF(n + 2), ARGF(n + 3), ARGF(n + 4), ARGF(n + 5));
}
static void put_transform(u32 g, CGAffineTransform t) {
    f32a *f = GP(g);
    f[0] = (float)t.a; f[1] = (float)t.b; f[2] = (float)t.c; f[3] = (float)t.d; f[4] = (float)t.tx; f[5] = (float)t.ty;
}
#define MATRIX(n, var) \
    CGAffineTransform var##_v; \
    const CGAffineTransform *var = NULL; \
    if (ARG(n)) { var##_v = get_transform(ARG(n)); var = &var##_v; }

// ---------- fonts ----------
SHIM(CTFontCreateWithName) {
    MATRIX(2, m);
    RET(cf_wrap(CTFontCreateWithName((CFStringRef)OBJ(0), ARGF(1), m)));
}
SHIM(CTFontCreateUIFontForLanguage) { RET(cf_wrap(CTFontCreateUIFontForLanguage(ARG(0), ARGF(1), (CFStringRef)OBJ(2)))); }
SHIM(CTFontCreateCopyWithSymbolicTraits) {
    MATRIX(2, m);
    RET(cf_wrap(CTFontCreateCopyWithSymbolicTraits(FONT(0), ARGF(1), m, ARG(3), ARG(4))));
}
SHIM(CTFontGetSize) { RETF(CTFontGetSize(FONT(0))); }
SHIM(CTFontGetAscent) { RETF(CTFontGetAscent(FONT(0))); }
SHIM(CTFontGetDescent) { RETF(CTFontGetDescent(FONT(0))); }
SHIM(CTFontGetLeading) { RETF(CTFontGetLeading(FONT(0))); }
SHIM(CTFontGetGlyphsForCharacters) { RET(CTFontGetGlyphsForCharacters(FONT(0), GP(ARG(1)), GP(ARG(2)), (i32)ARG(3))); }
SHIM(CTFontGetBoundingBox) {
    u32 buf = ARG(0);
    put_rect(buf, CTFontGetBoundingBox(FONT(1)));
    RET(buf);
    c->esp += 4;
}
SHIM(CTFontGetBoundingRectsForGlyphs) {
    u32 buf = ARG(0), out = ARG(4);
    i32 n = (i32)ARG(5);
    CGRect *rects = out ? calloc((size_t)n + 1, sizeof(CGRect)) : NULL;
    CGRect all = CTFontGetBoundingRectsForGlyphs(FONT(1), ARG(2), GP(ARG(3)), rects, n);
    for (i32 i = 0; rects && i < n; i++) put_rect(out + (u32)i * 16, rects[i]);
    free(rects);
    put_rect(buf, all);
    RET(buf);
    c->esp += 4;
}
SHIM(CTFontGetAdvancesForGlyphs) {
    u32 out = ARG(3);
    i32 n = (i32)ARG(4);
    CGSize *adv = out ? calloc((size_t)n + 1, sizeof(CGSize)) : NULL;
    double total = CTFontGetAdvancesForGlyphs(FONT(0), ARG(1), GP(ARG(2)), adv, n);
    for (i32 i = 0; adv && i < n; i++) {
        *(f32a *)GP(out + (u32)i * 8) = (float)adv[i].width;
        *(f32a *)GP(out + (u32)i * 8 + 4) = (float)adv[i].height;
    }
    free(adv);
    RETF(total);
}
SHIM(CTFontCopyGraphicsFont) { RET(cf_wrap(CTFontCopyGraphicsFont(FONT(0), NULL))); }
SHIM(CTFontCreatePathForGlyph) {
    MATRIX(2, m);
    RET(cf_wrap(CTFontCreatePathForGlyph(FONT(0), (CGGlyph)ARG(1), m)));
}

// ---------- attributed text lines ----------
SHIM(CFAttributedStringCreate) { RET(cf_wrap(CFAttributedStringCreate(NULL, (CFStringRef)OBJ(1), (CFDictionaryRef)OBJ(2)))); }
SHIM(CTLineCreateWithAttributedString) { RET(cf_wrap(CTLineCreateWithAttributedString((CFAttributedStringRef)OBJ(0)))); }
SHIM(CTLineDraw) { CTLineDraw((CTLineRef)OBJ(0), CTX(1)); }
SHIM(CTLineGetImageBounds) {
    u32 buf = ARG(0);
    put_rect(buf, CTLineGetImageBounds((CTLineRef)OBJ(1), CTX(2)));
    RET(buf);
    c->esp += 4;
}

// ---------- CoreGraphics state, text and colour ----------
SHIM(CGContextSetFont) { CGContextSetFont(CTX(0), (CGFontRef)OBJ(1)); }
SHIM(CGContextSetFontSize) { CGContextSetFontSize(CTX(0), ARGF(1)); }
SHIM(CGContextSetTextMatrix) { CGContextSetTextMatrix(CTX(0), arg_transform(c, 1)); }
SHIM(CGContextSetTextPosition) { CGContextSetTextPosition(CTX(0), ARGF(1), ARGF(2)); }
SHIM(CGContextGetTextPosition) {
    CGPoint p = CGContextGetTextPosition(CTX(0));
    float x = (float)p.x, y = (float)p.y;
    memcpy(&c->eax, &x, 4);
    memcpy(&c->edx, &y, 4);
}
SHIM(CGContextSetTextDrawingMode) { CGContextSetTextDrawingMode(CTX(0), (CGTextDrawingMode)ARG(1)); }
SHIM(CGContextSetCharacterSpacing) { CGContextSetCharacterSpacing(CTX(0), ARGF(1)); }
SHIM(CGContextSetRGBStrokeColor) { CGContextSetRGBStrokeColor(CTX(0), ARGF(1), ARGF(2), ARGF(3), ARGF(4)); }
SHIM(CGContextSetLineWidth) { CGContextSetLineWidth(CTX(0), ARGF(1)); }
SHIM(CGContextSetFillColorWithColor) { CGContextSetFillColorWithColor(CTX(0), (CGColorRef)OBJ(1)); }
SHIM(CGContextMoveToPoint) { CGContextMoveToPoint(CTX(0), ARGF(1), ARGF(2)); }
SHIM(CGContextAddLineToPoint) { CGContextAddLineToPoint(CTX(0), ARGF(1), ARGF(2)); }
SHIM(CGContextAddArcToPoint) { CGContextAddArcToPoint(CTX(0), ARGF(1), ARGF(2), ARGF(3), ARGF(4), ARGF(5)); }
SHIM(CGContextAddRect) { CGContextAddRect(CTX(0), CGRectMake(ARGF(1), ARGF(2), ARGF(3), ARGF(4))); }
SHIM(CGContextAddPath) { CGContextAddPath(CTX(0), (CGPathRef)OBJ(1)); }
SHIM(CGContextFillPath) { CGContextFillPath(CTX(0)); }
SHIM(CGContextStrokePath) { CGContextStrokePath(CTX(0)); }
SHIM(CGContextClip) { CGContextClip(CTX(0)); }
SHIM(CGContextEOClip) { CGContextEOClip(CTX(0)); }
SHIM(CGColorCreateGenericGray) { RET(cf_wrap(CGColorCreateGenericGray(ARGF(0), ARGF(1)))); }
SHIM(release_any) {
    CFTypeRef o = OBJ(0);
    if (o) CFRelease(o);
}

// ---------- images built from raw guest bytes ----------
SHIM(CGDataProviderCreateWithData) {
    // The bytes are copied, so the guest's release callback is not needed: run it right away if it wants the memory back.
    CFDataRef d = CFDataCreate(NULL, GP(ARG(1)), (i32)ARG(2));
    CGDataProviderRef p = CGDataProviderCreateWithCFData(d);
    CFRelease(d);
    if (ARG(3)) {
        u32 a[3] = {ARG(0), ARG(1), ARG(2)};
        rt_call_guest(c, ARG(3), 3, a);
    }
    RET(cf_wrap(p));
}
SHIM(CGImageCreate) {
    const CGFloat *dec = NULL;
    RET(cf_wrap(CGImageCreate(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), (CGColorSpaceRef)OBJ(5), ARG(6), (CGDataProviderRef)OBJ(7), dec, (bool)ARG(9), (CGColorRenderingIntent)ARG(10))));
}

// ---------- paths ----------
SHIM(CGPathCreateMutable) { RET(cf_wrap(CGPathCreateMutable())); }
SHIM(CGPathMoveToPoint) {
    MATRIX(1, m);
    CGPathMoveToPoint(PATH(0), m, ARGF(2), ARGF(3));
}
SHIM(CGPathAddLineToPoint) {
    MATRIX(1, m);
    CGPathAddLineToPoint(PATH(0), m, ARGF(2), ARGF(3));
}
SHIM(CGPathAddArcToPoint) {
    MATRIX(1, m);
    CGPathAddArcToPoint(PATH(0), m, ARGF(2), ARGF(3), ARGF(4), ARGF(5), ARGF(6));
}
SHIM(CGPathCloseSubpath) { CGPathCloseSubpath(PATH(0)); }

// ---------- plain geometry: pure functions over guest floats ----------
#define RX ARGF(0)
#define RY ARGF(1)
#define RW ARGF(2)
#define RH ARGF(3)
SHIM(CGRectGetMinX) { RETF(RX); }
SHIM(CGRectGetMinY) { RETF(RY); }
SHIM(CGRectGetMaxX) { RETF(RX + RW); }
SHIM(CGRectGetMaxY) { RETF(RY + RH); }
SHIM(CGRectGetMidX) { RETF(RX + RW / 2); }
SHIM(CGRectGetMidY) { RETF(RY + RH / 2); }
SHIM(CGAffineTransformMakeScale) {
    u32 buf = ARG(0);
    put_transform(buf, CGAffineTransformMakeScale(ARGF(1), ARGF(2)));
    RET(buf);
    c->esp += 4;
}

void rt_text_init(void) {
    u32 s = rt_import_addr("_CGAffineTransformIdentity");
    if (s) put_transform(s, CGAffineTransformIdentity);
}

const ShimReg rt_shims_text[] = {
    R(CGDataProviderCreateWithData) R(CGImageCreate) R(CTFontCreateWithName) R(CTFontCreateUIFontForLanguage) R(CTFontCreateCopyWithSymbolicTraits) R(CTFontGetSize) R(CTFontGetAscent)
    R(CTFontGetDescent) R(CTFontGetLeading) R(CTFontGetGlyphsForCharacters) R(CTFontGetBoundingBox) R(CTFontGetBoundingRectsForGlyphs)
    R(CTFontGetAdvancesForGlyphs) R(CTFontCopyGraphicsFont) R(CTFontCreatePathForGlyph) R(CFAttributedStringCreate)
    R(CTLineCreateWithAttributedString) R(CTLineDraw) R(CTLineGetImageBounds) R(CGContextSetFont) R(CGContextSetFontSize)
    R(CGContextSetTextMatrix) R(CGContextSetTextPosition) R(CGContextGetTextPosition) R(CGContextSetTextDrawingMode)
    R(CGContextSetCharacterSpacing) R(CGContextSetRGBStrokeColor) R(CGContextSetLineWidth) R(CGContextSetFillColorWithColor)
    R(CGContextMoveToPoint) R(CGContextAddLineToPoint) R(CGContextAddArcToPoint) R(CGContextAddRect) R(CGContextAddPath)
    R(CGContextFillPath) R(CGContextStrokePath) R(CGContextClip) R(CGContextEOClip) R(CGColorCreateGenericGray)
    R2("_CGColorRelease", release_any) R(CGPathCreateMutable) R(CGPathMoveToPoint) R(CGPathAddLineToPoint) R(CGPathAddArcToPoint)
    R(CGPathCloseSubpath) R2("_CGPathRelease", release_any) R(CGRectGetMinX) R(CGRectGetMinY) R(CGRectGetMaxX) R(CGRectGetMaxY)
    R(CGRectGetMidX) R(CGRectGetMidY) R(CGAffineTransformMakeScale)
    {NULL, NULL},
};
