// The soft dark ring round a button's icon (BatIcons.h), for the app and tools/bat_icons_preview.c. It lies under the white icon.
// It lets the icon read over a light picture, and over a dark one it doesn't show. Include after BatIcons.h.
#pragma once
#include <math.h>

// Into a box of w + pad * 2 across: the icon's shape grown by pad, black, fainter towards its edge, with the icon's own shape left clear.
// No shadows: those are drawn in device pixels, and the app's renderer and the preview scale differently.
static inline int bat_icon_halo_draw(CGContextRef g, const char *ident, CGFloat w, CGFloat pad) {
    int drawn = 0;
    // The icon drawn round two circles, each copy in a layer of its own so that its cut-outs don't cut the copies before it.
    for (int ring = 0; ring < 2; ring++) {
        CGFloat rr = ring ? pad * 0.45 : pad, alpha = ring ? 0.7 : 0.35;
        for (int k = 0; k < 16; k++) {
            CGContextSaveGState(g);
            CGContextSetAlpha(g, alpha);
            CGContextBeginTransparencyLayer(g, NULL);
            CGContextTranslateCTM(g, pad + rr * cos(k * M_PI / 8), pad + rr * sin(k * M_PI / 8));
            drawn = bat_icon_draw(g, ident, w);
            CGContextEndTransparencyLayer(g);
            CGContextRestoreGState(g);
        }
    }
    if (!drawn) return 0;
    // All of it black, keeping how covered each point is.
    CGContextSaveGState(g);
    CGContextSetBlendMode(g, kCGBlendModeSourceIn);
    CGContextSetGrayFillColor(g, 0, 1);
    CGContextFillRect(g, CGRectMake(0, 0, w + pad * 2, w + pad * 2));
    CGContextRestoreGState(g);
    // The icon itself knocked out, so that a faded button doesn't show the ring through its icon.
    // Drawn straight in that mode: a transparency layer keeps the blend mode inside it, and would draw nothing.
    CGContextSaveGState(g);
    CGContextSetBlendMode(g, kCGBlendModeDestinationOut);
    CGContextTranslateCTM(g, pad, pad);
    bat_icon_draw(g, ident, w);
    CGContextRestoreGState(g);
    return 1;
}
