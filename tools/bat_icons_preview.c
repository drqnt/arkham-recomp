// Draws the touch controls' icons (ios/app/BatIcons.h) into a picture, as the app draws its buttons, to look at them on the Mac before they go on a
// phone: white on their dark discs with the soft dark ring under each icon (BatHalo.h) and the disc's shadow, over a dark, a light and a grey
// picture, big and at the small buttons' size.
// clang -O1 tools/bat_icons_preview.c -framework CoreGraphics -framework ImageIO -framework CoreText -framework CoreFoundation -o /tmp/bat_icons_preview
// /tmp/bat_icons_preview out.png [bands]
#include "../ios/app/BatIcons.h"
#include "../ios/app/BatHalo.h"
#include <CoreText/CoreText.h>
#include <ImageIO/ImageIO.h>
#include <stdio.h>
#include <stdlib.h>

static const struct {
    const char *ident, *label;
    int ps;  // the PlayStation symbol in its corner: 1 cross, 2 square, 3 triangle, 4 circle, 0 none
} buttons[] = {
    {"cross", "Бег, действие", 1}, {"square", "Удар", 2},    {"triangle", "Контратака", 3}, {"circle", "Плащ", 4},   {"r1", "Крюк", 0},
    {"r2", "Присесть", 0},         {"r3", "Зум", 0},         {"l1", "Детектив", 0},        {"l2", "Прицел", 0},     {"l3", "Камера за спину", 0},
    {"gadgets", "Гаджеты", 0},     {"map", "Карта", 0},      {"pause", "Пауза", 0},         {"photo", "Скрыть кнопки", 0}, {"close", "Назад", 0},
    {"findbat", "Показать Бэтмена", 2}, {"twosticks", "Два стика", 0}, {"combo_td", "△+○ добивание", 0},
    {"combo_throw", "○+✕ бросок", 0}, {"combo_ground", "R2+△ лежачего", 0},
    {"g_batarang", "↑ Бэтаранг", 0}, {"g_sonic", "↗ Звуковой", 0}, {"g_line", "→ Тросомет", 0}, {"g_sequencer", "↘ Секвенатор", 0},
    {"g_batclaw", "↓ Бэткоготь", 0}, {"g_remote", "↙ Управляемый", 0}, {"g_gel", "← Гель", 0}, {"g_batarangs", "↖ Бэтаранги", 0},
};
#define N (sizeof buttons / sizeof buttons[0])
#define COLS 5

static CGContextRef bitmap(int w, int h) {
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef g = CGBitmapContextCreate(NULL, w, h, 8, 0, cs, kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(cs);
    return g;
}
// The icon alone, white on clear, as the app makes the mask of it.
static CGImageRef icon_image(const char *ident, int px) {
    CGContextRef g = bitmap(px, px);
    CGContextTranslateCTM(g, 0, px);
    CGContextScaleCTM(g, 1, -1);
    bat_icon_draw(g, ident, px);
    CGImageRef img = CGBitmapContextCreateImage(g);
    CGContextRelease(g);
    return img;
}
static void text(CGContextRef g, const char *s, CGFloat x, CGFloat y, CGFloat size, CGFloat grey) {
    CFStringRef str = CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8);
    CTFontRef font = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, size, NULL);
    CGColorRef color = CGColorCreateGenericGray(grey, 1);
    CFStringRef keys[] = {kCTFontAttributeName, kCTForegroundColorAttributeName};
    CFTypeRef values[] = {font, color};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, (const void **)keys, (const void **)values, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef as = CFAttributedStringCreate(NULL, str, attrs);
    CTLineRef line = CTLineCreateWithAttributedString(as);
    CGFloat w = CTLineGetTypographicBounds(line, NULL, NULL, NULL);
    CGContextSaveGState(g);
    CGContextSetTextMatrix(g, CGAffineTransformMakeScale(1, -1));  // the picture is y down
    CGContextSetTextPosition(g, x - w / 2, y);
    CTLineDraw(line, g);
    CGContextRestoreGState(g);
    CFRelease(line), CFRelease(as), CFRelease(attrs), CGColorRelease(color), CFRelease(font), CFRelease(str);
}
// The soft dark ring under an icon, as the app makes it: w + pad * 2 across.
static CGImageRef halo_image(const char *ident, int w, int pad) {
    int px = w + pad * 2;
    CGContextRef g = bitmap(px, px);
    CGContextTranslateCTM(g, 0, px);
    CGContextScaleCTM(g, 1, -1);
    bat_icon_halo_draw(g, ident, w, pad);
    CGImageRef img = CGBitmapContextCreateImage(g);
    CGContextRelease(g);
    return img;
}
// A mask image put down in the y down picture in a grey: it is y up in its own bitmap, so it's flipped back.
static void put_mask(CGContextRef g, CGImageRef img, CGRect box, CGFloat grey, CGFloat alpha) {
    CGContextSaveGState(g);
    CGContextTranslateCTM(g, 0, box.origin.y * 2 + box.size.height);
    CGContextScaleCTM(g, 1, -1);
    CGContextClipToMask(g, box, img);
    CGContextSetGrayFillColor(g, grey, alpha);
    CGContextFillRect(g, box);
    CGContextRestoreGState(g);
}
// One button as TouchControls.m draws it: the disc with its shadow and ring, the dark ring under the icon, the icon, and the PlayStation symbol
// in a dark badge at its corner.
static void button(CGContextRef g, int i, CGFloat cx, CGFloat cy, CGFloat r) {
    CGContextSaveGState(g);
    CGColorRef shadow = CGColorCreateGenericGray(0, 0.45);
    CGContextSetShadowWithColor(g, CGSizeZero, fmax(2, r * 0.08), shadow);
    CGColorRelease(shadow);
    CGContextSetGrayFillColor(g, 0, 0.5);
    CGContextFillEllipseInRect(g, CGRectMake(cx - r, cy - r, r * 2, r * 2));
    CGContextRestoreGState(g);
    CGContextSetGrayStrokeColor(g, 1, 0.9);
    CGContextSetLineWidth(g, fmax(1.5, r * 0.05));
    CGContextStrokeEllipseInRect(g, CGRectMake(cx - r, cy - r, r * 2, r * 2));
    CGFloat w = !strcmp(buttons[i].ident, "gadgets") ? r * 1.32 : r * 1.15;
    int wi = (int)ceil(w), pad = (int)ceil(fmax(3, w * 0.09));
    CGImageRef halo = halo_image(buttons[i].ident, wi, pad);
    put_mask(g, halo, CGRectMake(cx - wi / 2.0 - pad, cy - wi / 2.0 - pad, wi + pad * 2, wi + pad * 2), 0, 0.75);
    CGImageRelease(halo);
    CGImageRef img = icon_image(buttons[i].ident, wi);
    put_mask(g, img, CGRectMake(cx - wi / 2.0, cy - wi / 2.0, wi, wi), 1, 1);
    CGImageRelease(img);
    if (!buttons[i].ps) return;
    // As TouchControls.m draws the badge: a dark disc with a light rim, the symbol over it in its colour.
    CGFloat bx = cx + r * 0.72, by = cy + r * 0.72, br = fmax(10, r * 0.36), s = br * 0.5;
    CGContextSetGrayFillColor(g, 0, 0.85);
    CGContextFillEllipseInRect(g, CGRectMake(bx - br, by - br, br * 2, br * 2));
    CGContextSetGrayStrokeColor(g, 1, 0.6);
    CGContextSetLineWidth(g, 1);
    CGContextStrokeEllipseInRect(g, CGRectMake(bx - br, by - br, br * 2, br * 2));
    static const CGFloat colors[][3] = {{0, 0, 0}, {0x7F, 0xB2, 0xE5}, {0xE4, 0x8A, 0xD6}, {0x5C, 0xC4, 0x9A}, {0xE8, 0x6A, 0x6A}};
    const CGFloat *c = colors[buttons[i].ps];
    CGContextSetRGBStrokeColor(g, c[0] / 255, c[1] / 255, c[2] / 255, 1);
    CGContextSetLineWidth(g, fmax(2, br * 0.18));
    CGContextSetLineCap(g, kCGLineCapRound);
    CGContextSetLineJoin(g, kCGLineJoinRound);
    switch (buttons[i].ps) {
        case 1:
            CGContextMoveToPoint(g, bx - s, by - s), CGContextAddLineToPoint(g, bx + s, by + s);
            CGContextMoveToPoint(g, bx + s, by - s), CGContextAddLineToPoint(g, bx - s, by + s);
            break;
        case 2: CGContextAddRect(g, CGRectMake(bx - s * 0.9, by - s * 0.9, s * 1.8, s * 1.8)); break;
        case 3: bi_tri(g, bx, by - s * 1.05, bx + s * 1.05, by + s * 0.8, bx - s * 1.05, by + s * 0.8); break;
        case 4: CGContextAddEllipseInRect(g, CGRectMake(bx - s, by - s, s * 2, s * 2)); break;
    }
    CGContextStrokePath(g);
}
int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : "bat_icons.png";
    // the bands: over a dark, a light and a grey picture, big, and at the small buttons' size
    static const struct {
        int picture, tiny;  // picture: 0 dark, 1 light, 2 grey
        const char *title;
    } bands[] = {
        {0, 0, "На тёмной картинке"}, {1, 0, "На светлой картинке"}, {2, 0, "На серой картинке"},
        {0, 1, "Маленькие, на тёмной"}, {1, 1, "Маленькие, на светлой"}, {2, 1, "Маленькие, на серой"},
    };
    const int passes = argc > 2 ? atoi(argv[2]) : (int)(sizeof bands / sizeof bands[0]);  // how many of the bands to draw
    const CGFloat cell = 190, big = 62, small = 30;
    const int rows = (N + COLS - 1) / COLS;
    CGFloat W = COLS * cell, H = 0;
    for (int pass = 0; pass < passes; pass++) H += rows * (bands[pass].tiny ? 110 : cell) + 36;
    CGContextRef g = bitmap((int)W, (int)H);
    CGContextTranslateCTM(g, 0, H);
    CGContextScaleCTM(g, 1, -1);
    CGFloat y0 = 0;
    for (int pass = 0; pass < passes; pass++) {
        int light = bands[pass].picture > 0, tiny = bands[pass].tiny;
        CGFloat r = tiny ? small : big, h = rows * (tiny ? 110 : cell) + 36;
        // a picture's colours behind them: dark blue-grey like the asylum at night, a pale stone, or the grey of a concrete pillar
        if (bands[pass].picture == 1) CGContextSetRGBFillColor(g, 0.80, 0.78, 0.74, 1);
        else if (bands[pass].picture == 2) CGContextSetRGBFillColor(g, 0.52, 0.51, 0.48, 1);
        else CGContextSetRGBFillColor(g, 0.10, 0.12, 0.15, 1);
        CGContextFillRect(g, CGRectMake(0, y0, W, h));
        text(g, bands[pass].title, W / 2, y0 + 24, 17, light ? 0.15 : 0.85);
        for (unsigned i = 0; i < N; i++) {
            CGFloat cx = (i % COLS + 0.5) * cell, cy = y0 + 36 + (i / COLS) * (tiny ? 110 : cell) + (tiny ? 44 : 78);
            button(g, (int)i, cx, cy, r);
            text(g, buttons[i].label, cx, cy + r + 24, 14, light ? 0.1 : 0.9);
        }
        y0 += h;
    }
    CGImageRef img = CGBitmapContextCreateImage(g);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)out, (CFIndex)strlen(out), false);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
    CGImageDestinationAddImage(dst, img, NULL);
    int ok = CGImageDestinationFinalize(dst);
    CFRelease(dst), CFRelease(url), CGImageRelease(img), CGContextRelease(g);
    if (!ok) return fprintf(stderr, "can't write %s\n", out), 1;
    printf("%s: %d x %d\n", out, (int)W, (int)H);
    return 0;
}
