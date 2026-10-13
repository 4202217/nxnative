// 虚拟手柄（标准手柄布局，任天堂键位）+ 实体手柄（GameController）。两路合并后经 dm_input_set 交给宿主（hle_hid.c，键位见 dm_input.h）。
//   左：摇杆（左下区域任意处按下即以按下点为中心）、十字键（左下角）；右：A（右）B（下）X（上）Y（左）菱形，手指在键间滑动会换键；
//   上角：ZL / L、ZR / R；顶部中间：− / +。接上实体手柄时虚拟手柄隐藏，拔掉后重现。
//   实体手柄按位置对应：下 = B、右 = A、左 = Y、上 = X（与 Switch 手柄的键位位置一致）。
// Virtual gamepad (standard controller layout, Nintendo button positions) + physical controllers (GameController). Both are merged and handed to the host through dm_input_set (hle_hid.c, bit layout in dm_input.h).
//   Left: stick (press anywhere in the lower-left area and that point becomes the centre), D-pad (lower-left corner); right: A (right) B (bottom) X (top) Y (left) diamond, sliding a finger between keys switches keys;
//   top corners: ZL / L, ZR / R; top middle: − / +. The virtual gamepad hides while a physical controller is connected and comes back when it is removed.
//   Physical controllers map by position: bottom = B, right = A, left = Y, top = X (same positions as on a Switch controller).
#import <UIKit/UIKit.h>
#import <GameController/GameController.h>
#include <pthread.h>
#include "dm_input.h"

enum { B_A = 0, B_B, B_X, B_Y, B_LS, B_RS, B_L, B_R, B_ZL, B_ZR, B_PLUS, B_MINUS, B_LEFT, B_UP, B_RIGHT, B_DOWN };
enum { C_NONE = 0, C_STICK, C_DPAD, C_FACE, C_BTN };   // 触点占用的控件 | which control a touch holds

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_touch_btn, g_pad_btn;
static int32_t g_touch_lx, g_touch_ly, g_pad_lx, g_pad_ly, g_pad_rx, g_pad_ry;
static void publish(void) {
    pthread_mutex_lock(&g_mu);
    int32_t lx = g_touch_lx, ly = g_touch_ly;
    if ((int64_t)g_pad_lx * g_pad_lx + (int64_t)g_pad_ly * g_pad_ly > (int64_t)4000 * 4000) { lx = g_pad_lx; ly = g_pad_ly; }
    dm_input_set(g_touch_btn | g_pad_btn, lx, ly, g_pad_rx, g_pad_ry);
    pthread_mutex_unlock(&g_mu);
}

@interface DmGamepad : UIView
@end
static __weak DmGamepad *g_view;

@implementation DmGamepad {
    NSMapTable<UITouch *, NSNumber *> *_owner;     // 触点 → 控件（C_BTN 时附带键号 << 8） | touch → control (with the bit << 8 for C_BTN)
    UITouch *_stickTouch; CGPoint _stickOrigin, _stickNow;
    uint64_t _faceBits, _dpadBits, _btnBits;
    NSMapTable<UITouch *, NSNumber *> *_faceOf;    // 每个落在菱形区的触点各自按着哪些键 | which keys each touch on the diamond currently holds
}
- (instancetype)initWithFrame:(CGRect)f {
    if ((self = [super initWithFrame:f])) {
        self.multipleTouchEnabled = YES; self.opaque = NO; self.backgroundColor = UIColor.clearColor; self.contentMode = UIViewContentModeRedraw;
        _owner = [NSMapTable strongToStrongObjectsMapTable]; _faceOf = [NSMapTable strongToStrongObjectsMapTable];
        NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
        [nc addObserver:self selector:@selector(padsChanged) name:GCControllerDidConnectNotification object:nil];
        [nc addObserver:self selector:@selector(padsChanged) name:GCControllerDidDisconnectNotification object:nil];
        [self padsChanged];
    }
    return self;
}

// ---- 布局（点；按安全区内的矩形算） | Layout (points; computed inside the safe area) ----
- (CGRect)safe { return UIEdgeInsetsInsetRect(self.bounds, self.safeAreaInsets); }
- (CGFloat)u { return MIN(self.bounds.size.height / 430.0, 1.3); }   // 以 430 点高（iPhone Pro Max 横屏）为 1 | 1 = 430 points tall (iPhone Pro Max landscape)
- (CGPoint)faceCenter { CGRect s = self.safe; CGFloat u = self.u; return CGPointMake(CGRectGetMaxX(s) - 105 * u, CGRectGetMaxY(s) - 135 * u); }
- (CGFloat)faceGap { return 50 * self.u; }
- (CGFloat)faceR { return 27 * self.u; }
- (CGPoint)facePos:(int)b { CGPoint c = self.faceCenter; CGFloat g = self.faceGap;
    switch (b) { case B_A: return CGPointMake(c.x + g, c.y); case B_B: return CGPointMake(c.x, c.y + g); case B_X: return CGPointMake(c.x, c.y - g); default: return CGPointMake(c.x - g, c.y); } }
- (CGPoint)dpadCenter { CGRect s = self.safe; CGFloat u = self.u; return CGPointMake(CGRectGetMinX(s) + 150 * u, CGRectGetMaxY(s) - 55 * u); }
- (CGFloat)dpadR { return 46 * self.u; }
- (CGPoint)stickHome { CGRect s = self.safe; CGFloat u = self.u; return CGPointMake(CGRectGetMinX(s) + 95 * u, CGRectGetMaxY(s) - 175 * u); }
- (CGFloat)stickR { return 58 * self.u; }
- (CGRect)btnRect:(int)b { CGRect s = self.safe; CGFloat u = self.u, w = 86 * u, h = 38 * u, top = CGRectGetMinY(s) + 8 * u;
    switch (b) {
        case B_ZL: return CGRectMake(CGRectGetMinX(s) + 8 * u, top, w, h);
        case B_L: return CGRectMake(CGRectGetMinX(s) + 8 * u, top + h + 8 * u, w, h);
        case B_ZR: return CGRectMake(CGRectGetMaxX(s) - 8 * u - w, top, w, h);
        case B_R: return CGRectMake(CGRectGetMaxX(s) - 8 * u - w, top + h + 8 * u, w, h);
        case B_MINUS: return CGRectMake(CGRectGetMidX(s) - 70 * u - 22 * u, top, 44 * u, 34 * u);
        case B_PLUS: return CGRectMake(CGRectGetMidX(s) + 70 * u - 22 * u, top, 44 * u, 34 * u);
    }
    return CGRectZero; }
static const int k_btns[] = {B_ZL, B_L, B_ZR, B_R, B_MINUS, B_PLUS};

static CGFloat dist(CGPoint a, CGPoint b) { return hypot(a.x - b.x, a.y - b.y); }
- (uint64_t)faceAt:(CGPoint)p {   // 菱形区里手指下的键（两键之间可同时按） | keys under the finger in the diamond (between two keys both count)
    uint64_t m = 0; CGFloat r = self.faceR * 1.25;
    for (int b = B_A; b <= B_Y; b++) if (dist(p, [self facePos:b]) <= r) m |= 1ull << b;
    return m;
}
- (BOOL)inFace:(CGPoint)p { return dist(p, self.faceCenter) <= self.faceGap + self.faceR * 1.6; }
- (uint64_t)dpadAt:(CGPoint)p {
    CGPoint c = self.dpadCenter; CGFloat dx = p.x - c.x, dy = p.y - c.y;
    if (hypot(dx, dy) < self.dpadR * 0.25) return 0;
    double a = atan2(-dy, dx) * 180 / M_PI; uint64_t m = 0;   // 8 向：每向 45°，斜向按两键 | 8 directions: 45° each, diagonals press two keys
    if (a > -67.5 && a < 67.5) m |= 1ull << B_RIGHT;
    if (a > 22.5 && a < 157.5) m |= 1ull << B_UP;
    if (a > 112.5 || a < -112.5) m |= 1ull << B_LEFT;
    if (a < -22.5 && a > -157.5) m |= 1ull << B_DOWN;
    return m;
}
- (BOOL)inDpad:(CGPoint)p { return dist(p, self.dpadCenter) <= self.dpadR * 1.3; }
- (BOOL)inStickZone:(CGPoint)p { return p.x < self.bounds.size.width * 0.45 && p.y > self.bounds.size.height * 0.28; }

// ---- 触摸 | Touch ----
- (void)recompute {
    uint64_t face = 0; for (UITouch *t in _faceOf) face |= [_faceOf objectForKey:t].unsignedLongLongValue;
    uint64_t btn = 0, dpad = 0;
    for (UITouch *t in _owner) { uint64_t v = [_owner objectForKey:t].unsignedLongLongValue; int c = (int)(v & 0xff);
        if (c == C_BTN) btn |= 1ull << (v >> 8);
        else if (c == C_DPAD) dpad |= [self dpadAt:[t locationInView:self]]; }
    int32_t lx = 0, ly = 0;
    if (_stickTouch) {
        CGFloat dx = _stickNow.x - _stickOrigin.x, dy = _stickNow.y - _stickOrigin.y, R = self.stickR, d = hypot(dx, dy);
        if (d > R) { dx *= R / d; dy *= R / d; }
        lx = (int32_t)(dx / R * 32767); ly = (int32_t)(-dy / R * 32767);   // 宿主：上为正 | host: up is positive
    }
    BOOL changed = face != _faceBits || dpad != _dpadBits || btn != _btnBits;
    _faceBits = face; _dpadBits = dpad; _btnBits = btn;
    pthread_mutex_lock(&g_mu); g_touch_btn = face | dpad | btn; g_touch_lx = lx; g_touch_ly = ly; pthread_mutex_unlock(&g_mu);
    publish();
    if (changed || _stickTouch) [self setNeedsDisplay];
}
- (void)touchesBegan:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e {
    for (UITouch *t in ts) {
        CGPoint p = [t locationInView:self]; int hit = -1;
        for (int i = 0; i < 6; i++) if (CGRectContainsPoint(CGRectInset([self btnRect:k_btns[i]], -6, -6), p)) hit = k_btns[i];
        if (hit >= 0) [_owner setObject:@(C_BTN | (uint64_t)hit << 8) forKey:t];
        else if ([self inFace:p]) { [_owner setObject:@(C_FACE) forKey:t]; [_faceOf setObject:@([self faceAt:p]) forKey:t]; }
        else if ([self inDpad:p]) [_owner setObject:@(C_DPAD) forKey:t];
        else if (!_stickTouch && [self inStickZone:p]) { [_owner setObject:@(C_STICK) forKey:t]; _stickTouch = t; _stickOrigin = _stickNow = p; }
    }
    [self recompute];
}
- (void)touchesMoved:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e {
    for (UITouch *t in ts) {
        int c = (int)([_owner objectForKey:t].unsignedLongLongValue & 0xff); CGPoint p = [t locationInView:self];
        if (c == C_FACE) [_faceOf setObject:@([self faceAt:p]) forKey:t];
        else if (t == _stickTouch) _stickNow = p;
    }
    [self recompute];
}
- (void)endTouches:(NSSet<UITouch *> *)ts {
    for (UITouch *t in ts) { [_owner removeObjectForKey:t]; [_faceOf removeObjectForKey:t]; if (t == _stickTouch) _stickTouch = nil; }
    [self recompute];
}
- (void)touchesEnded:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e { [self endTouches:ts]; }
- (void)touchesCancelled:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e { [self endTouches:ts]; }
- (void)reset { [_owner removeAllObjects]; [_faceOf removeAllObjects]; _stickTouch = nil; [self recompute]; }

// ---- 绘制 | Drawing ----
static void circle(CGPoint c, CGFloat r, BOOL on, NSString *label, CGFloat fs) {
    UIBezierPath *p = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(c.x - r, c.y - r, 2 * r, 2 * r)];
    [[UIColor colorWithWhite:on ? 1 : 0.15 alpha:on ? 0.55 : 0.35] setFill]; [p fill];
    [[UIColor colorWithWhite:1 alpha:0.55] setStroke]; p.lineWidth = 1.5; [p stroke];
    if (label) { NSDictionary *a = @{NSFontAttributeName: [UIFont boldSystemFontOfSize:fs], NSForegroundColorAttributeName: [UIColor colorWithWhite:on ? 0 : 1 alpha:0.85]};
        CGSize s = [label sizeWithAttributes:a]; [label drawAtPoint:CGPointMake(c.x - s.width / 2, c.y - s.height / 2) withAttributes:a]; }
}
- (void)drawRect:(CGRect)r {
    CGFloat u = self.u; uint64_t all = _faceBits | _dpadBits | _btnBits;
    static NSString *const names[] = {@"A", @"B", @"X", @"Y"};
    for (int b = B_A; b <= B_Y; b++) circle([self facePos:b], self.faceR, (all >> b) & 1, names[b], 20 * u);
    CGPoint dc = self.dpadCenter; CGFloat a = self.dpadR * 0.38;   // 十字键：四个方块 | D-pad: four squares
    struct { int b; CGFloat dx, dy; NSString *s; } arms[] = {{B_UP, 0, -1, @"▲"}, {B_DOWN, 0, 1, @"▼"}, {B_LEFT, -1, 0, @"◀"}, {B_RIGHT, 1, 0, @"▶"}};
    for (int i = 0; i < 4; i++) circle(CGPointMake(dc.x + arms[i].dx * self.dpadR * 0.62, dc.y + arms[i].dy * self.dpadR * 0.62), a, (all >> arms[i].b) & 1, arms[i].s, 12 * u);
    CGPoint home = _stickTouch ? _stickOrigin : self.stickHome, knob = home;   // 摇杆：底座 + 手柄头 | stick: base + knob
    if (_stickTouch) { CGFloat dx = _stickNow.x - _stickOrigin.x, dy = _stickNow.y - _stickOrigin.y, d = hypot(dx, dy), R = self.stickR; if (d > R) { dx *= R / d; dy *= R / d; } knob = CGPointMake(home.x + dx, home.y + dy); }
    circle(home, self.stickR, NO, nil, 0); circle(knob, self.stickR * 0.45, _stickTouch != nil, nil, 0);
    static NSString *const bn[] = {@"ZL", @"L", @"ZR", @"R", @"−", @"+"};
    for (int i = 0; i < 6; i++) {
        CGRect rc = [self btnRect:k_btns[i]]; BOOL on = (all >> k_btns[i]) & 1;
        UIBezierPath *p = [UIBezierPath bezierPathWithRoundedRect:rc cornerRadius:rc.size.height / 2];
        [[UIColor colorWithWhite:on ? 1 : 0.15 alpha:on ? 0.55 : 0.35] setFill]; [p fill]; [[UIColor colorWithWhite:1 alpha:0.55] setStroke]; p.lineWidth = 1.5; [p stroke];
        NSDictionary *at = @{NSFontAttributeName: [UIFont boldSystemFontOfSize:16 * u], NSForegroundColorAttributeName: [UIColor colorWithWhite:on ? 0 : 1 alpha:0.85]};
        CGSize s = [bn[i] sizeWithAttributes:at]; [bn[i] drawAtPoint:CGPointMake(CGRectGetMidX(rc) - s.width / 2, CGRectGetMidY(rc) - s.height / 2) withAttributes:at];
    }
}
- (void)safeAreaInsetsDidChange { [super safeAreaInsetsDidChange]; [self setNeedsDisplay]; }
- (void)layoutSubviews { [super layoutSubviews]; [self setNeedsDisplay]; }

// ---- 实体手柄 | Physical controllers ----
static int32_t axis(float v) { return (int32_t)(fmaxf(-1.f, fminf(1.f, v)) * 32767); }
static void read_pad(GCExtendedGamepad *g) {
    uint64_t m = 0;
    #define K(btn, bit) if (btn.isPressed) m |= 1ull << bit
    K(g.buttonB, B_A); K(g.buttonA, B_B); K(g.buttonY, B_X); K(g.buttonX, B_Y);   // 按位置：右 A、下 B、上 X、左 Y | by position: right A, bottom B, top X, left Y
    K(g.leftShoulder, B_L); K(g.rightShoulder, B_R); K(g.leftTrigger, B_ZL); K(g.rightTrigger, B_ZR);
    K(g.buttonMenu, B_PLUS); if (g.buttonOptions) K(g.buttonOptions, B_MINUS);
    if (g.leftThumbstickButton) K(g.leftThumbstickButton, B_LS); if (g.rightThumbstickButton) K(g.rightThumbstickButton, B_RS);
    K(g.dpad.left, B_LEFT); K(g.dpad.up, B_UP); K(g.dpad.right, B_RIGHT); K(g.dpad.down, B_DOWN);
    #undef K
    pthread_mutex_lock(&g_mu);
    g_pad_btn = m; g_pad_lx = axis(g.leftThumbstick.xAxis.value); g_pad_ly = axis(g.leftThumbstick.yAxis.value);
    g_pad_rx = axis(g.rightThumbstick.xAxis.value); g_pad_ry = axis(g.rightThumbstick.yAxis.value);
    pthread_mutex_unlock(&g_mu);
    publish();
}
- (void)padsChanged {
    BOOL any = NO;
    for (GCController *c in GCController.controllers) if (c.extendedGamepad) {
        any = YES; c.extendedGamepad.valueChangedHandler = ^(GCExtendedGamepad *g, GCControllerElement *el) { (void)el; read_pad(g); };
        read_pad(c.extendedGamepad);
    }
    if (!any) { pthread_mutex_lock(&g_mu); g_pad_btn = 0; g_pad_lx = g_pad_ly = g_pad_rx = g_pad_ry = 0; pthread_mutex_unlock(&g_mu); publish(); }
    dispatch_async(dispatch_get_main_queue(), ^{ self.hidden = any; if (any) [self reset]; });
}
@end

UIView *dm_gamepad_view(CGRect frame) { DmGamepad *v = [[DmGamepad alloc] initWithFrame:frame]; g_view = v; return v; }
void dm_gamepad_reset(void) {   // 失活时：松开所有键（按着键切走、在后台松手，回来不会补「松开」） | On resign active: release everything (a key held while switching away and released in background gets no "release")
    dispatch_async(dispatch_get_main_queue(), ^{ [g_view reset]; });
    pthread_mutex_lock(&g_mu); g_pad_btn = 0; g_pad_lx = g_pad_ly = g_pad_rx = g_pad_ry = 0; pthread_mutex_unlock(&g_mu); publish();
}
