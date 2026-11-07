// AppKit on the phone, as far as Feral's code uses it (docs/IOS_PORT.md lists what ARKHAM_TRACE_OBJC showed). iOS has none of these classes, and the game sends
// messages to them and subclasses two of them, FeralWindow and FeralContentView. The runtime's Objective-C bridge calls host methods by their type encodings,
// so these are real classes with AppKit's signatures, CGRect standing in for NSRect. Windows and views remember what they're told and are the phone's screen;
// menus keep their items; the application's run loop turns the thread's run loop; the rest answers with nothing. Built only by tools/build.py --ios.
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <objc/runtime.h>

// The classes are named Arkham<AppKit name>, since iOS has classes of its own called NSColor and WebView, and its private frameworks may have more:
// two classes of one name in a process are one too many. The bridge asks for classes through rt_ios_class, which knows the names.
#define AK_CLASSES(X) X(NSResponder) X(NSView) X(NSScreen) X(NSWindow) X(NSEvent) X(NSMenuItem) X(NSMenu) X(NSApplication) X(NSCursor) X(NSImage) X(NSBitmapImageRep) X(NSGraphicsContext) X(NSOpenGLPixelFormat) X(NSOpenGLContext) X(NSColor) X(NSPasteboard) X(NSRunningApplication) X(NSWorkspace) X(NSAppleEventManager) X(WebFrame) X(WebView) X(WebUndefined) X(NSTableCellView) X(NSWindowController) X(NSAlert)
#define NSResponder ArkhamNSResponder
#define NSView ArkhamNSView
#define NSScreen ArkhamNSScreen
#define NSWindow ArkhamNSWindow
#define NSEvent ArkhamNSEvent
#define NSMenuItem ArkhamNSMenuItem
#define NSMenu ArkhamNSMenu
#define NSApplication ArkhamNSApplication
#define NSCursor ArkhamNSCursor
#define NSImage ArkhamNSImage
#define NSBitmapImageRep ArkhamNSBitmapImageRep
#define NSGraphicsContext ArkhamNSGraphicsContext
#define NSOpenGLPixelFormat ArkhamNSOpenGLPixelFormat
#define NSOpenGLContext ArkhamNSOpenGLContext
#define NSColor ArkhamNSColor
#define NSPasteboard ArkhamNSPasteboard
#define NSRunningApplication ArkhamNSRunningApplication
#define NSWorkspace ArkhamNSWorkspace
#define NSAppleEventManager ArkhamNSAppleEventManager
#define WebFrame ArkhamWebFrame
#define WebView ArkhamWebView
#define WebUndefined ArkhamWebUndefined
#define NSTableCellView ArkhamNSTableCellView
#define NSWindowController ArkhamNSWindowController
#define NSAlert ArkhamNSAlert

// Set by the app shell from UIScreen before the game starts: the screen in points, landscape, and its scale.
double rt_ios_screen_w = 874, rt_ios_screen_h = 402, rt_ios_screen_scale = 3;
static CGRect screen_rect(void) { return CGRectMake(0, 0, rt_ios_screen_w, rt_ios_screen_h); }

// Messages the stand-ins don't know end up UNHANDLED in the bridge's log (ARKHAM_TRACE_OBJC=1) and answer 0, which is how to find what to add here.

@interface NSResponder : NSObject {
    id _nextResponder;
}
@end
@implementation NSResponder
- (id)nextResponder { return _nextResponder; }
- (void)setNextResponder:(id)r { _nextResponder = r; }
- (BOOL)acceptsFirstResponder { return NO; }
- (BOOL)becomeFirstResponder { return YES; }
- (BOOL)resignFirstResponder { return YES; }
- (void)keyDown:(id)e {}
- (void)keyUp:(id)e {}
- (void)flagsChanged:(id)e {}
- (void)mouseDown:(id)e {}
- (void)mouseUp:(id)e {}
- (void)mouseMoved:(id)e {}
- (void)mouseDragged:(id)e {}
- (void)rightMouseDown:(id)e {}
- (void)rightMouseUp:(id)e {}
- (void)otherMouseDown:(id)e {}
- (void)otherMouseUp:(id)e {}
- (void)scrollWheel:(id)e {}
- (BOOL)performKeyEquivalent:(id)e { return NO; }
- (void)interpretKeyEvents:(NSArray *)events {}
- (void)doCommandBySelector:(SEL)s {}
// A message none of these knows, sent by system or runtime code rather than through the bridge (which answers those with 0 itself), does nothing and
// answers nothing, as AppKit's would have for this game's purposes, instead of ending the game. Each such selector is logged once.
- (NSMethodSignature *)methodSignatureForSelector:(SEL)sel {
    NSMethodSignature *s = [super methodSignatureForSelector:sel];
    if (s) return s;
    static NSMutableSet *told;
    @synchronized([ArkhamNSResponder class]) {
        if (!told) told = [NSMutableSet new];
        NSString *name = NSStringFromSelector(sel);
        if (![told containsObject:name]) {
            [told addObject:name];
            fprintf(stderr, "[ios] %s has no %s on the phone: nothing done\n", object_getClassName(self), name.UTF8String);
        }
    }
    return [NSMethodSignature signatureWithObjCTypes:"@@:"];
}
- (void)forwardInvocation:(NSInvocation *)inv {
    id nothing = nil;
    if (inv.methodSignature.methodReturnLength == sizeof(id)) [inv setReturnValue:&nothing];
}
@end

@class NSWindow;
@interface NSView : NSResponder {
@public
    CGRect _frame;
    NSView *_superview;
    NSWindow *_window;
    NSMutableArray *_subviews;
    BOOL _wantsLayer, _hidden;
    NSUInteger _autoresizing;
}
@end
@implementation NSView
- (instancetype)initWithFrame:(CGRect)f {
    if ((self = [super init])) {
        _frame = f;
        _subviews = [NSMutableArray new];
    }
    return self;
}
- (instancetype)init { return [self initWithFrame:CGRectZero]; }
- (void)dealloc {
    [_subviews release];
    [super dealloc];
}
- (CGRect)frame { return _frame; }
- (void)setFrame:(CGRect)f { _frame = f; }
- (void)setFrameSize:(CGSize)s { _frame.size = s; }
- (void)setFrameOrigin:(CGPoint)p { _frame.origin = p; }
- (CGRect)bounds { return CGRectMake(0, 0, _frame.size.width, _frame.size.height); }
- (NSView *)superview { return _superview; }
- (NSWindow *)window { return _window; }
- (NSArray *)subviews { return _subviews; }
- (void)setSubviews:(NSArray *)views {
    for (NSView *v in [[_subviews copy] autorelease]) [v removeFromSuperview];
    for (NSView *v in views) [self addSubview:v];
}
- (void)setWindowRecursively:(NSWindow *)w {
    _window = w;
    for (NSView *v in _subviews) [v setWindowRecursively:w];
}
- (void)addSubview:(NSView *)v {
    if (!v || [_subviews containsObject:v]) return;
    [v removeFromSuperview];
    [_subviews addObject:v];
    v->_superview = self;
    [v setWindowRecursively:_window];
}
- (void)addSubview:(NSView *)v positioned:(NSInteger)place relativeTo:(NSView *)other { [self addSubview:v]; }
- (void)removeFromSuperview {
    if (!_superview) return;
    NSView *s = _superview;
    _superview = nil;
    [self setWindowRecursively:nil];
    [s->_subviews removeObject:self];
}
- (void)setAutoresizingMask:(NSUInteger)m { _autoresizing = m; }
- (NSUInteger)autoresizingMask { return _autoresizing; }
- (void)setAutoresizesSubviews:(BOOL)b {}
- (void)setWantsLayer:(BOOL)b { _wantsLayer = b; }
- (BOOL)wantsLayer { return _wantsLayer; }
- (id)layer { return nil; }
- (void)setHidden:(BOOL)h { _hidden = h; }
- (BOOL)isHidden { return _hidden; }
- (BOOL)isFlipped { return NO; }
- (BOOL)isOpaque { return YES; }
- (void)setNeedsDisplay:(BOOL)b {}
- (void)display {}
- (void)setPostsFrameChangedNotifications:(BOOL)b {}
- (void)addTrackingArea:(id)a {}
- (void)removeTrackingArea:(id)a {}
- (NSArray *)trackingAreas { return @[]; }
- (void)updateTrackingAreas {}
- (id)hitTest:(CGPoint)p { return self; }
- (CGPoint)convertPoint:(CGPoint)p fromView:(NSView *)v { return p; }
- (CGPoint)convertPoint:(CGPoint)p toView:(NSView *)v { return p; }
- (CGRect)convertRect:(CGRect)r fromView:(NSView *)v { return r; }
- (CGRect)convertRect:(CGRect)r toView:(NSView *)v { return r; }
- (CGRect)convertRectToBacking:(CGRect)r {
    return CGRectMake(r.origin.x * rt_ios_screen_scale, r.origin.y * rt_ios_screen_scale, r.size.width * rt_ios_screen_scale, r.size.height * rt_ios_screen_scale);
}
- (void)setWantsBestResolutionOpenGLSurface:(BOOL)b {}
- (void)resetCursorRects {}
- (void)discardCursorRects {}
- (void)addCursorRect:(CGRect)r cursor:(id)c {}
@end

@interface NSScreen : NSObject
@end
@implementation NSScreen
+ (NSScreen *)mainScreen {
    static NSScreen *s;
    if (!s) s = [NSScreen new];
    return s;
}
// The same objects each time, as AppKit gives: the game asks for these every frame, and each new object would take one of the bridge's handles.
+ (NSArray *)screens {
    static NSArray *a;
    if (!a) a = [@[[self mainScreen]] retain];
    return a;
}
- (CGRect)frame { return screen_rect(); }
- (CGRect)visibleFrame { return screen_rect(); }
- (CGFloat)backingScaleFactor { return rt_ios_screen_scale; }
- (NSDictionary *)deviceDescription {
    static NSDictionary *d;
    static CGSize made;
    CGSize now = CGSizeMake(rt_ios_screen_w, rt_ios_screen_h);
    if (!d || !CGSizeEqualToSize(made, now)) {
        [d release];
        d = [@{@"NSScreenNumber": @1, @"NSDeviceSize": [NSValue valueWithBytes:&now objCType:@encode(CGSize)]} retain];
        made = now;
    }
    return d;
}
- (NSString *)localizedName { return @"iPhone"; }
@end

@interface NSWindow : NSResponder {
@public
    CGRect _frame;
    NSUInteger _style, _collection;
    NSView *_content;
    id _delegate, _first;
    CGSize _min, _max;
    BOOL _visible;
}
@end
static NSMutableArray *all_windows;
static NSUInteger event_type(id e);  // after NSEvent, below
@implementation NSWindow
+ (CGRect)frameRectForContentRect:(CGRect)r styleMask:(NSUInteger)m { return r; }
+ (CGRect)contentRectForFrameRect:(CGRect)r styleMask:(NSUInteger)m { return r; }
- (instancetype)initWithContentRect:(CGRect)r styleMask:(NSUInteger)style backing:(NSUInteger)backing defer:(BOOL)defer {
    if ((self = [super init])) {
        _frame = r;
        _style = style;
        _max = CGSizeMake(1e6, 1e6);
        _content = [[NSView alloc] initWithFrame:CGRectMake(0, 0, r.size.width, r.size.height)];
        [_content setWindowRecursively:self];
        if (!all_windows) all_windows = [NSMutableArray new];
        [all_windows addObject:[NSValue valueWithNonretainedObject:self]];
    }
    return self;
}
- (instancetype)initWithContentRect:(CGRect)r styleMask:(NSUInteger)style backing:(NSUInteger)backing defer:(BOOL)defer screen:(id)screen {
    return [self initWithContentRect:r styleMask:style backing:backing defer:defer];
}
- (void)dealloc {
    for (NSValue *v in [[all_windows copy] autorelease])
        if (v.nonretainedObjectValue == self) [all_windows removeObject:v];
    [_content release];
    [super dealloc];
}
- (CGRect)frame { return _frame; }
- (void)setFrame:(CGRect)f display:(BOOL)d {
    _frame = f;
    [_content setFrame:CGRectMake(0, 0, f.size.width, f.size.height)];
}
- (void)setFrame:(CGRect)f display:(BOOL)d animate:(BOOL)a { [self setFrame:f display:d]; }
- (void)setFrameOrigin:(CGPoint)p { _frame.origin = p; }
- (void)setContentSize:(CGSize)s { [self setFrame:CGRectMake(_frame.origin.x, _frame.origin.y, s.width, s.height) display:NO]; }
- (CGRect)contentRectForFrameRect:(CGRect)r { return r; }
- (CGRect)frameRectForContentRect:(CGRect)r { return r; }
- (CGRect)convertRectToScreen:(CGRect)r { return CGRectOffset(r, _frame.origin.x, _frame.origin.y); }
- (CGRect)convertRectFromScreen:(CGRect)r { return CGRectOffset(r, -_frame.origin.x, -_frame.origin.y); }
- (CGPoint)convertBaseToScreen:(CGPoint)p { return CGPointMake(p.x + _frame.origin.x, p.y + _frame.origin.y); }
- (CGPoint)convertScreenToBase:(CGPoint)p { return CGPointMake(p.x - _frame.origin.x, p.y - _frame.origin.y); }
- (NSUInteger)styleMask { return _style; }
- (void)setStyleMask:(NSUInteger)m { _style = m; }
- (NSUInteger)collectionBehavior { return _collection; }
- (void)setCollectionBehavior:(NSUInteger)b { _collection = b; }
- (NSView *)contentView { return _content; }
- (void)setContentView:(NSView *)v {
    if (v == _content) return;
    [_content setWindowRecursively:nil];
    [_content release];
    _content = [v retain];
    [_content setFrame:CGRectMake(0, 0, _frame.size.width, _frame.size.height)];
    [_content setWindowRecursively:self];
}
- (id)delegate { return _delegate; }
- (void)setDelegate:(id)d { _delegate = d; }
- (BOOL)isVisible { return _visible; }
- (BOOL)isKeyWindow { return _visible; }
- (BOOL)isMainWindow { return _visible; }
- (BOOL)isMiniaturized { return NO; }
- (BOOL)isZoomed { return YES; }
- (BOOL)isOnActiveSpace { return YES; }
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }
- (void)deminiaturize:(id)s {}
- (void)miniaturize:(id)s {}
- (void)makeKeyAndOrderFront:(id)s { _visible = YES; }
- (void)makeKeyWindow { _visible = YES; }
- (void)makeMainWindow {}
- (void)orderFront:(id)s { _visible = YES; }
- (void)orderFrontRegardless { _visible = YES; }
- (void)orderBack:(id)s {}
- (void)orderOut:(id)s { _visible = NO; }
- (void)close { _visible = NO; }
- (void)center {}
- (BOOL)makeFirstResponder:(id)r {
    _first = r;
    return YES;
}
- (id)firstResponder { return _first ? _first : self; }
- (void)setInitialFirstResponder:(id)r { _first = r; }
- (void)setTitle:(NSString *)t {}
- (NSString *)title { return @""; }
- (void)setLevel:(NSInteger)l {}
- (NSInteger)level { return 0; }
- (void)setMovable:(BOOL)b {}
- (void)setHasShadow:(BOOL)b {}
- (void)setOpaque:(BOOL)b {}
- (void)setAlphaValue:(CGFloat)a {}
- (void)setIgnoresMouseEvents:(BOOL)b {}
- (void)setBackgroundColor:(id)c {}
- (void)setAcceptsMouseMovedEvents:(BOOL)b {}
- (void)setReleasedWhenClosed:(BOOL)b {}
- (void)setRestorable:(BOOL)b {}
- (void)setHidesOnDeactivate:(BOOL)b {}
- (void)setMinSize:(CGSize)s { _min = s; }
- (void)setMaxSize:(CGSize)s { _max = s; }
- (CGSize)minSize { return _min; }
- (CGSize)maxSize { return _max; }
- (void)setContentMinSize:(CGSize)s {}
- (void)setContentMaxSize:(CGSize)s {}
- (void)setContentAspectRatio:(CGSize)s {}
- (id)standardWindowButton:(NSUInteger)b { return nil; }
// The phone is always full screen.
- (void)toggleFullScreen:(id)s {
    _style |= 1u << 14;
    if ([_delegate respondsToSelector:@selector(windowDidEnterFullScreen:)]) [_delegate performSelector:@selector(windowDidEnterFullScreen:) withObject:nil];
}
- (id)screen { return [NSScreen mainScreen]; }
- (CGFloat)backingScaleFactor { return rt_ios_screen_scale; }
- (NSInteger)windowNumber { return 1; }
- (void)invalidateCursorRectsForView:(id)v {}
- (void)resetCursorRects {}
- (void)disableCursorRects {}
- (void)enableCursorRects {}
- (BOOL)validateMenuItem:(id)item { return YES; }
// AppKit hands a mouse event to the view under it, and up the responder chain to the window; the game's FeralWindow takes them itself.
- (void)sendEvent:(id)e {
    switch (event_type(e)) {
        case 1: [self mouseDown:e]; break;
        case 2: [self mouseUp:e]; break;
        case 5: [self mouseMoved:e]; break;
        case 6: [self mouseDragged:e]; break;
        default: break;
    }
}
- (void)display {}
- (void)displayIfNeeded {}
- (void)update {}
- (id)graphicsContext { return nil; }
- (BOOL)isReleasedWhenClosed { return NO; }
- (void)addChildWindow:(id)w ordered:(NSInteger)place {}
- (void)removeChildWindow:(id)w {}
@end

@interface NSEvent : NSObject {
@public
    NSUInteger _type, _flags;
    CGPoint _location;
    double _time;
    id _window;
    NSInteger _clicks, _number;
    float _pressure;
}
@end
@implementation NSEvent
+ (NSEvent *)mouseEventWithType:(NSUInteger)type location:(CGPoint)loc modifierFlags:(NSUInteger)flags timestamp:(double)t windowNumber:(NSInteger)wn
                        context:(id)ctx eventNumber:(NSInteger)en clickCount:(NSInteger)clicks pressure:(float)pressure {
    NSEvent *e = [[NSEvent new] autorelease];
    e->_type = type;
    e->_flags = flags;
    e->_location = loc;
    e->_time = t;
    e->_clicks = clicks;
    e->_number = en;
    e->_pressure = pressure;
    return e;
}
- (float)pressure { return _pressure; }
- (NSInteger)eventNumber { return _number; }
- (NSInteger)windowNumber { return _window ? 1 : 0; }
+ (NSEvent *)otherEventWithType:(NSUInteger)type location:(CGPoint)loc modifierFlags:(NSUInteger)flags timestamp:(double)t windowNumber:(NSInteger)wn context:(id)ctx
                        subtype:(short)sub data1:(NSInteger)d1 data2:(NSInteger)d2 {
    NSEvent *e = [[NSEvent new] autorelease];
    e->_type = type;
    e->_flags = flags;
    e->_location = loc;
    e->_time = t;
    return e;
}
+ (NSUInteger)modifierFlags { return 0; }
+ (NSUInteger)pressedMouseButtons { return 0; }
+ (CGPoint)mouseLocation { return CGPointZero; }
- (NSUInteger)type { return _type; }
- (NSUInteger)modifierFlags { return _flags; }
- (CGPoint)locationInWindow { return _location; }
- (double)timestamp { return _time; }
- (unsigned short)keyCode { return 0; }
- (NSInteger)buttonNumber { return 0; }
- (NSInteger)clickCount { return _clicks; }
- (BOOL)isARepeat { return NO; }
- (NSString *)characters { return @""; }
- (NSString *)charactersIgnoringModifiers { return @""; }
- (id)window { return _window; }
- (CGFloat)deltaX { return 0; }
- (CGFloat)deltaY { return 0; }
- (short)subtype { return 0; }
@end

static NSUInteger event_type(id e) { return [e isKindOfClass:[NSEvent class]] ? ((NSEvent *)e)->_type : 0; }

@interface NSMenuItem : NSObject {
@public
    NSString *_title, *_key;
    SEL _action;
    id _target, _submenu, _menu;
    NSInteger _tag, _state;
    NSUInteger _mask;
    BOOL _enabled, _separator;
}
@end
@interface NSMenu : NSObject {
    NSMutableArray *_items;
    NSString *_title;
}
@end
@implementation NSMenuItem
+ (NSMenuItem *)separatorItem {
    NSMenuItem *i = [[NSMenuItem new] autorelease];
    i->_separator = YES;
    return i;
}
- (instancetype)initWithTitle:(NSString *)t action:(SEL)a keyEquivalent:(NSString *)k {
    if ((self = [super init])) {
        _title = [t copy];
        _key = [k copy];
        _action = a;
        _enabled = YES;
    }
    return self;
}
- (void)dealloc {
    [_title release];
    [_key release];
    [_submenu release];
    [super dealloc];
}
- (NSString *)title { return _title ? _title : @""; }
- (void)setTitle:(NSString *)t {
    [_title release];
    _title = [t copy];
}
- (SEL)action { return _action; }
- (void)setAction:(SEL)a { _action = a; }
- (id)target { return _target; }
- (void)setTarget:(id)t { _target = t; }
- (NSInteger)tag { return _tag; }
- (void)setTag:(NSInteger)t { _tag = t; }
- (NSInteger)state { return _state; }
- (void)setState:(NSInteger)s { _state = s; }
- (BOOL)isEnabled { return _enabled; }
- (void)setEnabled:(BOOL)e { _enabled = e; }
- (BOOL)isHidden { return NO; }
- (void)setHidden:(BOOL)h {}
- (NSString *)keyEquivalent { return _key ? _key : @""; }
- (void)setKeyEquivalent:(NSString *)k {
    [_key release];
    _key = [k copy];
}
- (NSUInteger)keyEquivalentModifierMask { return _mask; }
- (void)setKeyEquivalentModifierMask:(NSUInteger)m { _mask = m; }
- (id)submenu { return _submenu; }
- (void)setSubmenu:(id)m {
    [_submenu release];
    _submenu = [m retain];
}
- (BOOL)hasSubmenu { return _submenu != nil; }
- (id)menu { return _menu; }
- (BOOL)isSeparatorItem { return _separator; }
@end
@implementation NSMenu
- (instancetype)initWithTitle:(NSString *)t {
    if ((self = [super init])) {
        _title = [t copy];
        _items = [NSMutableArray new];
    }
    return self;
}
- (instancetype)init { return [self initWithTitle:@""]; }
- (void)dealloc {
    [_items release];
    [_title release];
    [super dealloc];
}
- (NSString *)title { return _title; }
- (void)setTitle:(NSString *)t {
    [_title release];
    _title = [t copy];
}
- (void)addItem:(NSMenuItem *)i {
    [_items addObject:i];
    i->_menu = self;
}
- (void)insertItem:(NSMenuItem *)i atIndex:(NSInteger)n {
    [_items insertObject:i atIndex:(NSUInteger)MIN(MAX(n, 0), (NSInteger)_items.count)];
    i->_menu = self;
}
- (void)removeItem:(NSMenuItem *)i { [_items removeObject:i]; }
- (void)removeItemAtIndex:(NSInteger)n {
    if (n >= 0 && n < (NSInteger)_items.count) [_items removeObjectAtIndex:(NSUInteger)n];
}
- (void)removeAllItems { [_items removeAllObjects]; }
- (NSMenuItem *)itemAtIndex:(NSInteger)n { return n >= 0 && n < (NSInteger)_items.count ? _items[(NSUInteger)n] : nil; }
- (NSInteger)indexOfItem:(NSMenuItem *)i {
    NSUInteger n = [_items indexOfObject:i];
    return n == NSNotFound ? -1 : (NSInteger)n;
}
- (NSInteger)indexOfItemWithTag:(NSInteger)t {
    for (NSUInteger n = 0; n < _items.count; n++)
        if (((NSMenuItem *)_items[n])->_tag == t) return (NSInteger)n;
    return -1;
}
- (NSMenuItem *)itemWithTag:(NSInteger)t {
    NSInteger n = [self indexOfItemWithTag:t];
    return n < 0 ? nil : _items[(NSUInteger)n];
}
- (NSInteger)numberOfItems { return (NSInteger)_items.count; }
- (NSArray *)itemArray { return _items; }
- (void)setAutoenablesItems:(BOOL)b {}
- (void)update {}
@end

void rt_run_guest_main_queue(void);  // shims_threads.c: what the game sent to its main queue
@interface NSApplication : NSResponder {
    id _delegate, _menu;
    _Atomic int _stopped;
    BOOL _running, _launched;
@public
    NSMutableArray *_events;  // posted, for nextEventMatchingMask: to hand out: the phone's taps, as clicks (rt_ios_click)
}
@end
@implementation NSApplication
+ (NSApplication *)sharedApplication {
    static NSApplication *app;
    if (!app) app = [NSApplication new];
    return app;
}
- (id)delegate { return _delegate; }
- (void)setDelegate:(id)d { _delegate = d; }
- (id)mainMenu { return _menu; }
- (void)setMainMenu:(id)m {
    [_menu release];
    _menu = [m retain];
}
- (BOOL)setActivationPolicy:(NSInteger)p { return YES; }
- (NSInteger)activationPolicy { return 0; }
- (void)activateIgnoringOtherApps:(BOOL)b {}
- (BOOL)isActive { return YES; }
- (BOOL)isHidden { return NO; }
- (void)hide:(id)s {}
- (void)unhide:(id)s {}
- (NSUInteger)presentationOptions { return 0; }
- (void)setPresentationOptions:(NSUInteger)o {}
- (id)currentEvent { return nil; }
- (void)setApplicationIconImage:(id)i {}
- (void)updateWindows {}
- (id)targetForAction:(SEL)a to:(id)t from:(id)f { return nil; }
- (BOOL)validateMenuItem:(id)item { return YES; }
- (NSArray *)windows {
    static NSArray *last;
    NSMutableArray *now = [NSMutableArray array];
    for (NSValue *v in all_windows) [now addObject:v.nonretainedObjectValue];
    if (![last isEqualToArray:now]) {
        [last release];
        last = [now copy];
    }
    return last;
}
- (id)keyWindow {
    for (NSValue *v in all_windows)
        if (((NSWindow *)v.nonretainedObjectValue)->_visible) return v.nonretainedObjectValue;
    return nil;
}
- (id)mainWindow { return [self keyWindow]; }
- (void)postEvent:(id)e atStart:(BOOL)s {
    if (!e) return;
    @synchronized(self) {
        if (!_events) _events = [NSMutableArray new];
        if (s) [_events insertObject:e atIndex:0];
        else [_events addObject:e];
    }
    extern struct __CFRunLoop *rt_guest_main_loop(void);
    CFRunLoopWakeUp(rt_guest_main_loop());
}
- (NSEvent *)takeEventMatching:(unsigned long long)mask {
    @synchronized(self) {
        for (NSUInteger i = 0; i < _events.count; i++) {
            NSEvent *e = _events[i];
            if (mask & (1ull << e->_type)) {
                [[e retain] autorelease];
                [_events removeObjectAtIndex:i];
                return e;
            }
        }
    }
    return nil;
}
- (void)sendEvent:(id)e {
    id w = [(NSEvent *)e window];
    if (w) [w sendEvent:e];
}
- (void)stop:(id)s { atomic_store(&_stopped, 1); }
- (void)terminate:(id)s { exit(0); }
- (void)replyToApplicationShouldTerminate:(BOOL)b {}
- (void)finishLaunching {
    if (_launched) return;
    _launched = YES;
    for (NSString *name in @[@"applicationWillFinishLaunching:", @"applicationDidFinishLaunching:", @"applicationDidBecomeActive:"]) {
        SEL s = NSSelectorFromString(name);
        if ([_delegate respondsToSelector:s]) [_delegate performSelector:s withObject:nil];
    }
}
// The game's events, one pass of this thread's run loop: timers and performSelector:afterDelay:, which is how the game starts its main loop, fire here.
// There are no AppKit events on the phone, so there's never one to return.
// AppKit waits here until an event comes or the date passes, and the runtime's lock is free all that time. Returning at once would have the
// game's main thread take the lock back thousands of times a second, and the lock isn't fair: the game's other threads could wait forever.
// Work sent to the game's main queue wakes the wait (shims_threads.c).
- (id)nextEventMatchingMask:(unsigned long long)mask untilDate:(NSDate *)date inMode:(NSString *)mode dequeue:(BOOL)dequeue {
    rt_run_guest_main_queue();
    NSEvent *e = [self takeEventMatching:mask];
    if (e) return e;
    double wait = date ? [date timeIntervalSinceNow] : 0;
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, wait < 0 ? 0 : wait > 0.05 ? 0.05 : wait, true);
    rt_run_guest_main_queue();
    return [self takeEventMatching:mask];
}
// The first call is NSApplicationMain's: launching, then this thread's run loop until stop:. A run inside it is how Feral's code pumps events
// once on a Mac (stop:, then run, which handles one event and returns), so here it is one pass: the main queue and the run loop.
- (void)run {
    if (_running) {
        rt_run_guest_main_queue();
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.002, true);  // a short wait, as above; on a Mac it is the event's handling that takes the time
        rt_run_guest_main_queue();
        atomic_store(&_stopped, 0);
        return;
    }
    _running = YES;
    [self finishLaunching];
    while (!atomic_exchange(&_stopped, 0)) {
        @autoreleasepool {
            rt_run_guest_main_queue();
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
        }
    }
    _running = NO;
}
- (NSInteger)runModalForWindow:(id)w { return 0; }
- (void)stopModal {}
- (void)abortModal {}
- (void)orderFrontStandardAboutPanel:(id)s {}
- (NSInteger)requestUserAttention:(NSUInteger)t { return 0; }
- (void)setWindowsNeedUpdate:(BOOL)b {}
@end

@interface NSCursor : NSObject
@end
@implementation NSCursor
+ (NSCursor *)arrowCursor {
    static NSCursor *c;
    if (!c) c = [NSCursor new];
    return c;
}
+ (void)hide {}
+ (void)unhide {}
+ (void)setHiddenUntilMouseMoves:(BOOL)b {}
- (instancetype)initWithImage:(id)image hotSpot:(CGPoint)p { return [self init]; }
- (void)set {}
- (void)push {}
- (void)pop {}
@end

@interface NSImage : NSObject {
    CGImageRef _image;
    CGSize _size;
}
@end
@implementation NSImage
+ (id)imageNamed:(NSString *)name { return nil; }
- (instancetype)initWithCGImage:(CGImageRef)image size:(CGSize)s {
    if ((self = [super init])) {
        _image = CGImageRetain(image);
        _size = s;
    }
    return self;
}
- (instancetype)initWithSize:(CGSize)s {
    if ((self = [super init])) _size = s;
    return self;
}
- (void)dealloc {
    CGImageRelease(_image);
    [super dealloc];
}
- (CGImageRef)CGImageForProposedRect:(CGRect *)r context:(id)ctx hints:(NSDictionary *)hints { return _image; }
- (CGSize)size { return _size; }
- (void)setSize:(CGSize)s { _size = s; }
- (void)addRepresentation:(id)rep {}
- (void)lockFocus {}
- (void)unlockFocus {}
@end

@interface NSBitmapImageRep : NSObject {
    CGImageRef _image;
}
@end
@implementation NSBitmapImageRep
- (instancetype)initWithCGImage:(CGImageRef)image {
    if ((self = [super init])) _image = CGImageRetain(image);
    return self;
}
- (void)dealloc {
    CGImageRelease(_image);
    [super dealloc];
}
- (CGImageRef)CGImage { return _image; }
@end

@interface NSGraphicsContext : NSObject
@end
@implementation NSGraphicsContext
+ (id)currentContext { return nil; }
+ (id)graphicsContextWithWindow:(id)w { return nil; }
+ (void)setCurrentContext:(id)c {}
+ (void)saveGraphicsState {}
+ (void)restoreGraphicsState {}
@end

// Feral's Direct3D layer still makes an OpenGL context before the Metal layer takes over: these let it, and draw nothing.
// The game asks a legacy and a core context the same questions and gets different answers (ios_gl.c), so the pixel format remembers which it asked for.
@interface NSOpenGLPixelFormat : NSObject {
@public
    BOOL _core;
}
@end
@implementation NSOpenGLPixelFormat
- (instancetype)initWithAttributes:(const uint32_t *)attrs {
    if ((self = [super init]))
        for (int i = 0; attrs && attrs[i] && i < 64; i++)
            if (attrs[i] == 99 /* NSOpenGLPFAOpenGLProfile */ && (attrs[i + 1] == 0x3200 || attrs[i + 1] == 0x4100)) _core = YES;
    return self;
}
- (void *)CGLPixelFormatObj { return self; }
@end
extern int rt_ios_gl_core;
@interface NSOpenGLContext : NSObject {
@public
    id _view;
    BOOL _core;
}
@end
static NSOpenGLContext *current_gl;
int rt_ios_gl_context_is_core(void *ctx) {
    id o = (id)ctx;
    return [o isKindOfClass:[NSOpenGLContext class]] && ((NSOpenGLContext *)o)->_core;
}
@implementation NSOpenGLContext
+ (id)currentContext { return current_gl; }
+ (void)clearCurrentContext { current_gl = nil; }
- (instancetype)initWithFormat:(id)format shareContext:(id)share {
    if ((self = [super init])) _core = [format isKindOfClass:[NSOpenGLPixelFormat class]] && ((NSOpenGLPixelFormat *)format)->_core;
    return self;
}
- (void)makeCurrentContext {
    current_gl = self;
    rt_ios_gl_core = _core;
}
- (void)clearDrawable {}
- (void)setView:(id)v { _view = v; }
- (id)view { return _view; }
- (void)update {}
- (void)flushBuffer {}
// Its own stand-in: CGLSetCurrentContext (ios_gl.c) is handed this back.
- (void *)CGLContextObj { return self; }
- (void)setValues:(const int *)vals forParameter:(NSInteger)p {}
- (void)getValues:(int *)vals forParameter:(NSInteger)p {
    if (vals) *vals = 0;
}
@end

@interface NSColor : NSObject
@end
@implementation NSColor
+ (id)colorWithDeviceRed:(CGFloat)r green:(CGFloat)g blue:(CGFloat)b alpha:(CGFloat)a { return [[NSColor new] autorelease]; }
+ (id)colorWithCalibratedRed:(CGFloat)r green:(CGFloat)g blue:(CGFloat)b alpha:(CGFloat)a { return [[NSColor new] autorelease]; }
+ (id)blackColor { return [[NSColor new] autorelease]; }
+ (id)clearColor { return [[NSColor new] autorelease]; }
+ (id)whiteColor { return [[NSColor new] autorelease]; }
- (void)set {}
- (void)setFill {}
@end

@interface NSPasteboard : NSObject
@end
@implementation NSPasteboard
+ (id)generalPasteboard {
    static NSPasteboard *p;
    if (!p) p = [NSPasteboard new];
    return p;
}
- (NSString *)stringForType:(NSString *)t { return nil; }
- (NSInteger)clearContents { return 0; }
- (BOOL)setString:(NSString *)s forType:(NSString *)t { return NO; }
@end

@interface NSRunningApplication : NSObject
@end
@implementation NSRunningApplication
+ (id)currentApplication {
    static NSRunningApplication *a;
    if (!a) a = [NSRunningApplication new];
    return a;
}
- (NSURL *)bundleURL {
    extern const char *rt_game_root(void);
    static NSURL *u;
    if (!u) u = [[NSURL fileURLWithPath:[NSString stringWithFormat:@"%s/Batman Arkham Asylum GOTY.app", rt_game_root()]] retain];
    return u;
}
- (NSString *)bundleIdentifier { return @"com.feralinteractive.bmaa"; }
- (int)processIdentifier { return getpid(); }
- (BOOL)activateWithOptions:(NSUInteger)o { return YES; }
- (BOOL)isActive { return YES; }
@end

@interface NSWorkspace : NSObject
@end
@implementation NSWorkspace
+ (id)sharedWorkspace {
    static NSWorkspace *w;
    if (!w) w = [NSWorkspace new];
    return w;
}
- (NSArray *)runningApplications {
    static NSArray *a;
    if (!a) a = [@[[NSRunningApplication currentApplication]] retain];
    return a;
}
- (NSNotificationCenter *)notificationCenter {
    static NSNotificationCenter *c;
    if (!c) c = [NSNotificationCenter new];
    return c;
}
- (BOOL)openURL:(NSURL *)u { return NO; }
@end

@interface NSAppleEventManager : NSObject
@end
@implementation NSAppleEventManager
+ (id)sharedAppleEventManager {
    static NSAppleEventManager *m;
    if (!m) m = [NSAppleEventManager new];
    return m;
}
- (void)setEventHandler:(id)h andSelector:(SEL)s forEventClass:(uint32_t)c andEventID:(uint32_t)e {}
- (void)removeEventHandlerForEventClass:(uint32_t)c andEventID:(uint32_t)e {}
@end

// The launcher's web view: built, never shown, since the phone skips the launcher (rt_launcher_skip_install).
@interface WebFrame : NSObject
@end
@implementation WebFrame
- (NSString *)name { return @""; }
- (id)provisionalDataSource { return nil; }
- (id)dataSource { return nil; }
- (void)loadRequest:(id)r {}
@end
@interface WebView : NSView {
    WebFrame *_frameObj;
}
@end
@implementation WebView
- (instancetype)initWithFrame:(CGRect)f frameName:(NSString *)fn groupName:(NSString *)gn {
    if ((self = [super initWithFrame:f])) _frameObj = [WebFrame new];
    return self;
}
- (void)dealloc {
    [_frameObj release];
    [super dealloc];
}
- (void)setFrameLoadDelegate:(id)d {}
- (void)setPolicyDelegate:(id)d {}
- (void)setUIDelegate:(id)d {}
- (void)setDrawsBackground:(BOOL)b {}
- (void)setApplicationNameForUserAgent:(NSString *)s {}
- (id)mainFrame { return _frameObj; }
- (void)setMainFrameURL:(NSString *)u {}
- (id)windowScriptObject { return nil; }
- (NSString *)stringByEvaluatingJavaScriptFromString:(NSString *)s { return nil; }
- (void)close {}
@end
@interface WebUndefined : NSObject
@end
@implementation WebUndefined
+ (id)undefined {
    static WebUndefined *u;
    if (!u) u = [WebUndefined new];
    return u;
}
@end

// Superclasses of the game's Game Center window, which the game never shows here.
@interface NSTableCellView : NSView
@end
@implementation NSTableCellView
@end
@interface NSWindowController : NSResponder
@end
@implementation NSWindowController
- (instancetype)initWithWindow:(id)w { return [self init]; }
- (id)window { return nil; }
- (void)showWindow:(id)s {}
- (void)close {}
@end

// Feral's first-run and error dialogs: answered with their first button, as a player would most of the time.
@interface NSAlert : NSObject {
    NSString *_message, *_info;
}
@end
@implementation NSAlert
- (void)dealloc {
    [_message release];
    [_info release];
    [super dealloc];
}
- (void)setMessageText:(NSString *)t {
    [_message release];
    _message = [t copy];
}
- (void)setInformativeText:(NSString *)t {
    [_info release];
    _info = [t copy];
}
- (NSString *)messageText { return _message; }
- (NSString *)informativeText { return _info; }
- (id)addButtonWithTitle:(NSString *)t { return nil; }
- (void)setAlertStyle:(NSUInteger)s {}
- (void)setShowsSuppressionButton:(BOOL)b {}
- (id)suppressionButton { return nil; }
- (id)window { return nil; }
- (NSInteger)runModal {
    fprintf(stderr, "[ios] a dialog, answered with its first button: %s / %s\n", _message.UTF8String ?: "", _info.UTF8String ?: "");
    return 1000;
}
@end

// objc_getClass for the bridge (shims_objc.c): an AppKit name gives its stand-in here.
Class rt_ios_class(const char *name) {
    static const char *const ak[] = {
#define AK_NAME(n) #n,
        AK_CLASSES(AK_NAME)
    };
    for (unsigned i = 0; i < sizeof ak / sizeof *ak; i++)
        if (!strcmp(name, ak[i])) {
            char b[64];
            snprintf(b, sizeof b, "Arkham%s", name);
            return objc_getClass(b);
        }
    return objc_getClass(name);
}

// A tap on the phone's screen as a click of the left button there, for the game's menus (ios/app/TouchControls.m), in the window's points from its
// top left. The cursor goes there first, and the button comes up a little later, as a finger would let go.
void rt_touch_cursor(double x, double y);  // shims_sys.c
void rt_ios_click(double x, double y) {
    NSApplication *app = [NSApplication sharedApplication];
    NSWindow *w = [app keyWindow];
    if (!w) return;
    rt_touch_cursor(x, y);
    CGPoint at = CGPointMake(x, w->_frame.size.height - y);
    double now = CFAbsoluteTimeGetCurrent();
    NSEvent *moved = [NSEvent mouseEventWithType:5 location:at modifierFlags:0 timestamp:now windowNumber:1 context:nil eventNumber:0 clickCount:0 pressure:0];
    NSEvent *down = [NSEvent mouseEventWithType:1 location:at modifierFlags:0 timestamp:now windowNumber:1 context:nil eventNumber:1 clickCount:1 pressure:1];
    moved->_window = down->_window = w;
    [app postEvent:moved atStart:NO];
    [app postEvent:down atStart:NO];
    [[w retain] autorelease];
    NSWindow *kept = [w retain];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        NSEvent *up = [NSEvent mouseEventWithType:2 location:at modifierFlags:0 timestamp:CFAbsoluteTimeGetCurrent() windowNumber:1 context:nil eventNumber:2 clickCount:1 pressure:0];
        up->_window = kept;
        [app postEvent:up atStart:NO];
        [kept release];
    });
}
