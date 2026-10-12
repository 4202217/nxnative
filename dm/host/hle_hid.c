// nn::hid（手柄）：真机经 hid 服务读共享内存；宿主从前端写入的当前状态取（Mac 键盘 / 手柄，iOS 虚拟手柄 / 手柄）。
// 与游戏无关的通用版：一台标准手柄。DM_OPMODE=0（掌机）→ 掌机手柄（Npad 号 0x20）；否则 → 1P 全键手柄（号 0）。
// 震动、六轴、鼠标、键盘：宿主不提供（设备数 0 / 未连接）；手柄小程序直接报「1 人、已选当前手柄」。
// nn::hid (controller): on the real device shared memory is read through the hid service; the host takes the current state written by the front end (Mac keyboard / controller, iOS virtual gamepad / controller).
// Generic, game-independent version: one standard controller. DM_OPMODE=0 (handheld) → handheld controller (Npad id 0x20); otherwise → 1P full-key controller (id 0).
// Vibration, six-axis, mouse, keyboard: not provided by the host (device count 0 / not connected); the controller applet reports "1 player, current controller selected".
#include "hle.h"
#include "dm_input.h"
#include <stdatomic.h>
#include <unistd.h>
#include <strings.h>
#include <pthread.h>
#include <time.h>

enum { NPAD_NO1 = 0, NPAD_HANDHELD = 0x20 };
enum { STYLE_FULLKEY = 1u << 0, STYLE_HANDHELD = 1u << 1, STYLE_JOYDUAL = 1u << 2, STYLE_JOYLEFT = 1u << 3, STYLE_JOYRIGHT = 1u << 4 };
enum { ATTR_CONNECTED = 1u << 0, ATTR_WIRED = 1u << 1, ATTR_LEFT_CONN = 1u << 2, ATTR_LEFT_WIRED = 1u << 3, ATTR_RIGHT_CONN = 1u << 4, ATTR_RIGHT_WIRED = 1u << 5 };
// 摇杆方向伪键（NpadButton 16..23）：由摇杆值导出，阈值约半程
// Stick direction pseudo-buttons (NpadButton 16..23): derived from the stick values, threshold about half travel
enum { STICK_THRESH = 0x4000 };

// nn::hid::NpadFullKeyState / Handheld / JoyDual / JoyLeft / JoyRight 布局相同（0x28）
// nn::hid::NpadFullKeyState / Handheld / JoyDual / JoyLeft / JoyRight share one layout (0x28)
typedef struct { int64_t sampling; uint64_t buttons; int32_t lx, ly, rx, ry; uint32_t attributes, _pad; } NpadState;
_Static_assert(sizeof(NpadState) == 0x28, "NpadState");

// 前端发布：键盘 / 触屏一路（dm_input_set）与实体手柄一路（dm_pad_set）按位或；按下沿锁存到游戏下一次取，短按不漏
// Front-end publishing: the keyboard / touch path (dm_input_set) and the physical controller path (dm_pad_set) are ORed; press edges are latched until the game's next read so short presses are not lost
static _Atomic uint64_t g_btn, g_axes, g_latch, g_pad_btn, g_pad_axes, g_pad_latch;
static _Atomic int64_t g_sampling;
static uint64_t pack(int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    return (uint64_t)(uint16_t)lx | (uint64_t)(uint16_t)ly << 16 | (uint64_t)(uint16_t)rx << 32 | (uint64_t)(uint16_t)ry << 48;
}
void dm_input_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    uint64_t old = atomic_exchange(&g_btn, buttons); atomic_fetch_or(&g_latch, buttons & ~old); atomic_store(&g_axes, pack(lx, ly, rx, ry));
}
void dm_pad_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    uint64_t old = atomic_exchange(&g_pad_btn, buttons); atomic_fetch_or(&g_pad_latch, buttons & ~old); atomic_store(&g_pad_axes, pack(lx, ly, rx, ry));
}
uint64_t dm_input_samples(void) { return (uint64_t)atomic_load(&g_sampling); }
// DM_KEYS 脚本输入（测试用）："秒:键[+键][~按住秒],…"，例 "8:A,12:A,15:Down~0.5,16:A+B"。键名 A B X Y LS RS L R ZL ZR Plus Minus Left Up Right Down；
//   缺省按住 0.15 s。走手柄那一路（dm_pad_set），与键盘 / 触屏按位或，不互相覆盖。时间从宿主启动算起
// DM_KEYS scripted input (for tests): "seconds:key[+key][~hold seconds],...", e.g. "8:A,12:A,15:Down~0.5,16:A+B". Key names A B X Y LS RS L R ZL ZR Plus Minus Left Up Right Down;
//   held 0.15 s by default. Goes through the controller path (dm_pad_set), ORed with keyboard / touch, neither overrides the other. Times are counted from host start
typedef struct { double at, hold; uint64_t btn; } key_step;
static key_step g_steps[64]; static int g_nsteps;
static void *keys_main(void *arg) {
    (void)arg; struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < g_nsteps; i++) {
        for (;;) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); double el = (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9; if (el >= g_steps[i].at) break; usleep(10000); }
        LOG("DM_KEYS：%.2f s 按 %#llx（按住 %.2f s）", g_steps[i].at, (unsigned long long)g_steps[i].btn, g_steps[i].hold);
        dm_pad_set(g_steps[i].btn, 0, 0, 0, 0); usleep((useconds_t)(g_steps[i].hold * 1e6)); dm_pad_set(0, 0, 0, 0, 0);
    }
    return NULL;
}
void hid_keys_init(void) {
    const char *e = getenv("DM_KEYS"); if (!e || !*e) return;
    static const char *names[16] = {"A", "B", "X", "Y", "LS", "RS", "L", "R", "ZL", "ZR", "Plus", "Minus", "Left", "Up", "Right", "Down"};
    char buf[1024]; snprintf(buf, sizeof buf, "%s", e);
    for (char *tok = strtok(buf, ","); tok && g_nsteps < 64; tok = strtok(NULL, ",")) {
        char *c = strchr(tok, ':'); if (!c) { LOG("【红】DM_KEYS 步骤缺「秒:」：%s", tok); continue; }
        *c = 0; key_step st = {atof(tok), 0.15, 0};
        char *h = strchr(c + 1, '~'); if (h) { *h = 0; st.hold = atof(h + 1); }
        for (char *k = c + 1; k && *k;) { char *plus = strchr(k, '+'); if (plus) *plus = 0;
            int found = 0; for (int b = 0; b < 16; b++) if (!strcasecmp(k, names[b])) { st.btn |= 1ull << b; found = 1; }
            if (!found) LOG("【红】DM_KEYS 不认识的键名：%s", k);
            k = plus ? plus + 1 : NULL; }
        g_steps[g_nsteps++] = st;
    }
    pthread_t th; if (g_nsteps && !pthread_create(&th, NULL, keys_main, NULL)) { pthread_detach(th); LOG("DM_KEYS：%d 步", g_nsteps); }
}

static uint32_t connected_id(void) { return dm_opmode() == 0 ? NPAD_HANDHELD : NPAD_NO1; }
static uint32_t style_of(uint32_t id) { return id != connected_id() ? 0 : id == NPAD_HANDHELD ? STYLE_HANDHELD : STYLE_FULLKEY; }
static int32_t ax(uint64_t v, int i) { return (int16_t)(v >> (16 * i)); }
static int32_t pick(int32_t a, int32_t b) { return (a < 0 ? -a : a) >= (b < 0 ? -b : b) ? a : b; }   // 两路里偏得多的那个 | whichever of the two paths is deflected more

static void fill(NpadState *s, uint32_t id, uint32_t style) {
    memset(s, 0, sizeof *s);
    s->sampling = atomic_fetch_add(&g_sampling, 1) + 1;
    if (!(style_of(id) & style)) return;   // 这个号 / 样式没接手柄：attributes = 0 | no controller of this id / style: attributes = 0
    uint64_t b = atomic_load(&g_btn) | atomic_exchange(&g_latch, 0) | atomic_load(&g_pad_btn) | atomic_exchange(&g_pad_latch, 0);
    uint64_t k = atomic_load(&g_axes), p = atomic_load(&g_pad_axes);
    s->lx = pick(ax(k, 0), ax(p, 0)); s->ly = pick(ax(k, 1), ax(p, 1)); s->rx = pick(ax(k, 2), ax(p, 2)); s->ry = pick(ax(k, 3), ax(p, 3));
    b &= 0xffffull;
    if (s->lx < -STICK_THRESH) b |= 1u << 16;
    if (s->ly > STICK_THRESH) b |= 1u << 17;
    if (s->lx > STICK_THRESH) b |= 1u << 18;
    if (s->ly < -STICK_THRESH) b |= 1u << 19;
    if (s->rx < -STICK_THRESH) b |= 1u << 20;
    if (s->ry > STICK_THRESH) b |= 1u << 21;
    if (s->rx > STICK_THRESH) b |= 1u << 22;
    if (s->ry < -STICK_THRESH) b |= 1u << 23;
    s->buttons = b;
    s->attributes = ATTR_CONNECTED | (id == NPAD_HANDHELD ? ATTR_WIRED | ATTR_LEFT_CONN | ATTR_LEFT_WIRED | ATTR_RIGHT_CONN | ATTR_RIGHT_WIRED : ATTR_LEFT_CONN | ATTR_RIGHT_CONN);
}
// GetNpadStates(out, n, id)：返回写入条数，新的在前。宿主不保留历史：只给 1 条当前状态（调用方按返回数处理）
// GetNpadStates(out, n, id): returns the number written, newest first. The host keeps no history: only 1 current state (callers handle the returned count)
static int states(NpadState *out, int n, uint32_t id, uint32_t style) { if (n <= 0) return 0; fill(out, id, style); return 1; }

HLE_FORCE("_ZN2nn3hid14InitializeNpadEv", "连 hid 服务；宿主从前端状态取输入", void, h_hid_InitNpad, void) {}
HLE_FORCE("_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", "支持的手柄号；宿主固定", void, h_hid_SetIds, const uint32_t *ids, uint64_t n) { (void)ids; (void)n; }
HLE_FORCE("_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", "支持的手柄样式；宿主固定", void, h_hid_SetStyles, uint32_t styles) { (void)styles; }
HLE_FORCE("_ZN2nn3hid15GetNpadStyleSetERKj", "手柄样式查询；跟 DM_OPMODE 走", uint32_t, h_hid_GetStyle, const uint32_t *id) { return style_of(*id); }
HLE_FORCE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj", "全键手柄状态", int, h_hid_StatesFullKey, NpadState *s, int n, const uint32_t *id) { return states(s, n, *id, STYLE_FULLKEY); }
HLE_FORCE("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj", "掌机手柄状态", int, h_hid_StatesHandheld, NpadState *s, int n, const uint32_t *id) { return states(s, n, *id, STYLE_HANDHELD); }
HLE_FORCE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj", "双 Joy-Con 状态（宿主不接）", int, h_hid_StatesJoyDual, NpadState *s, int n, const uint32_t *id) { return states(s, n, *id, STYLE_JOYDUAL); }
HLE_FORCE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj", "左 Joy-Con 状态（宿主不接）", int, h_hid_StatesJoyLeft, NpadState *s, int n, const uint32_t *id) { return states(s, n, *id, STYLE_JOYLEFT); }
HLE_FORCE("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj", "右 Joy-Con 状态（宿主不接）", int, h_hid_StatesJoyRight, NpadState *s, int n, const uint32_t *id) { return states(s, n, *id, STYLE_JOYRIGHT); }
HLE_FORCE("_ZN2nn3hid14DisconnectNpadERKj", "断开手柄；宿主忽略", void, h_hid_Disconnect, const uint32_t *id) { (void)id; }
HLE_FORCE("_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE", "Joy-Con 握持方向；宿主忽略", void, h_hid_HoldType, int t) { (void)t; }
HLE_FORCE("_ZN2nn3hid20GetNpadJoyAssignmentERKj", "Joy-Con 分配：双持", int, h_hid_GetJoyAssign, const uint32_t *id) { (void)id; return 0; }
HLE_FORCE("_ZN2nn3hid30SetNpadJoyAssignmentModeSingleERKj", "Joy-Con 单持；宿主忽略", void, h_hid_JoySingle, const uint32_t *id) { (void)id; }
HLE_FORCE("_ZN2nn3hid18SwapNpadAssignmentERKjS2_", "交换手柄号；宿主忽略", void, h_hid_Swap, const uint32_t *a, const uint32_t *b) { (void)a; (void)b; }
HLE_FORCE("_ZN2nn3hid21StartLrAssignmentModeEv", "L/R 分配模式；宿主忽略", void, h_hid_LrStart, void) {}
HLE_FORCE("_ZN2nn3hid20StopLrAssignmentModeEv", "L/R 分配模式；宿主忽略", void, h_hid_LrStop, void) {}
// 手柄小程序（系统 UI）：直接报「1 人、选中当前手柄」。ControllerSupportResultInfo = { int playerCount; uint32_t selectedId; }
// Controller applet (system UI): report "1 player, current controller selected" directly. ControllerSupportResultInfo = { int playerCount; uint32_t selectedId; }
HLE_FORCE("_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE", "手柄小程序；宿主直接报已选好",
          Result, h_hid_ShowCtrl, int32_t *info, const void *arg) { (void)arg; if (info) { info[0] = 1; info[1] = (int32_t)connected_id(); } return RES_OK; }
// 六轴：给出句柄，但读不到状态（0 条）
// Six-axis: handles are handed out, but no states can be read (0 entries)
HLE_FORCE("_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", "六轴句柄",
          int, h_hid_SixHandles, uint32_t *h, int n, const uint32_t *id, uint32_t style) { (void)style; int k = n < 2 ? n : 2; for (int i = 0; i < k; i++) h[i] = (*id << 8) | (uint32_t)i; return k; }
HLE_FORCE("_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", "六轴；宿主不提供", void, h_hid_SixStart, const uint32_t *h) { (void)h; }
HLE_FORCE("_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE", "六轴状态；宿主不提供（0 条）",
          int, h_hid_SixStates, void *out, int n, const uint32_t *h) { (void)out; (void)n; (void)h; return 0; }
// 震动：没有设备
// Vibration: no devices
HLE_FORCE("_ZN2nn3hid20IsVibrationPermittedEv", "震动许可；宿主不支持", unsigned, h_hid_VibPermit, void) { return 0; }
HLE_FORCE("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", "震动设备：0 个",
          int, h_hid_VibHandles, void *h, int n, const uint32_t *id, uint32_t style) { (void)h; (void)n; (void)id; (void)style; return 0; }
HLE_FORCE("_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", "震动；宿主不支持", void, h_hid_InitVib, const void *h) { (void)h; }
HLE_FORCE("_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", "震动；宿主不支持", void, h_hid_SendVib, const void *h, const void *v) { (void)h; (void)v; }
// 鼠标 / 键盘：未连接
// Mouse / keyboard: not connected
HLE_FORCE("_ZN2nn3hid15InitializeMouseEv", "鼠标；宿主不提供", void, h_hid_InitMouse, void) {}
HLE_FORCE("_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi", "鼠标状态：0 条", int, h_hid_MouseStates, void *out, int n) { (void)out; (void)n; return 0; }
HLE_FORCE("_ZN2nn3hid18InitializeKeyboardEv", "键盘；宿主不提供", void, h_hid_InitKbd, void) {}
HLE_FORCE("_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE", "键盘状态：未连接（KeyboardState 0x30 字节清零）", void, h_hid_KbdState, void *s) {
    memset(s, 0, 0x30); ((int64_t *)s)[0] = atomic_fetch_add(&g_sampling, 1) + 1;
}
