// 较新 SDK（16.x）游戏用到、d2r-mobile 那一作没用到的系统服务接口：oe / time / pl（共享字体）/ vi 垂直同步 / diag。
// 真机上都经 Horizon 系统服务（IPC）；宿主直接给值或做成「不可用」。
// System service interfaces used by games on newer SDKs (16.x) that the d2r-mobile title did not use: oe / time / pl (shared font) / vi vsync / diag.
// On the real device all of these go through Horizon system services (IPC); the host gives values directly or makes them "unavailable".
#include "hle.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// ---------- oe ----------
HLE_FORCE("_ZN2nn2oe15SetCpuBoostModeENS0_12CpuBoostModeE", "CPU 加速档；宿主不调频", void, h_oe_CpuBoost, int m) { (void)m; }
// DisplayVersion = char[16]（应用的显示版本号）
// DisplayVersion = char[16] (the application's display version)
HLE_FORCE("_ZN2nn2oe17GetDisplayVersionEPNS0_14DisplayVersionE", "显示版本号；宿主给 1.0.0", void, h_oe_DispVer, char *v) { memset(v, 0, 16); strlcpy(v, "1.0.0", 16); }
HLE_FORCE("_ZN2nn2oe24GetExpectedVolumeBalanceEPfS1_", "系统 / 应用音量平衡；宿主恒 1:1", void, h_oe_GetVolBal, float *sys, float *app) { *sys = 1.0f; *app = 1.0f; }
HLE_FORCE("_ZN2nn2oe24SetExpectedVolumeBalanceEff", "音量平衡；宿主忽略", void, h_oe_SetVolBal, float sys, float app) { (void)sys; (void)app; }
// 程序累计运行时长（nn::TimeSpan，纳秒）：取宿主启动以来的单调时间
// Program total active time (nn::TimeSpan, nanoseconds): the host's monotonic time since startup
HLE_FORCE("_ZN2nn2oe25GetProgramTotalActiveTimeEv", "累计运行时长 → 宿主启动以来", int64_t, h_oe_ActiveTime, void) { return (int64_t)(now_ns() - g_t0_ns); }
HLE_FORCE("_ZN2nn2oe31EnterExitRequestHandlingSectionEv", "退出请求临界区；宿主的退出走 SIGTERM / 生命周期", void, h_oe_EnterExit, void) {}
HLE_FORCE("_ZN2nn2oe31LeaveExitRequestHandlingSectionEv", "退出请求临界区；宿主忽略", void, h_oe_LeaveExit, void) {}
HLE_FORCE("_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb", "自动休眠延长；宿主忽略", void, h_oe_Inactivity, unsigned on) { (void)on; }

// ---------- time ----------
// CalendarTime = { int16 年; int8 月, 日, 时, 分, 秒 }，按值返回（8 字节内，走 x0）
// CalendarTime = { int16 year; int8 month, day, hour, minute, second }, returned by value (within 8 bytes, in x0)
HLE_FORCE("_ZN2nn4time19ToCalendarTimeInUtcERKNS0_9PosixTimeE", "绝对秒 → UTC 日历（宿主 gmtime_r）", uint64_t, h_time_ToCalUtc, const int64_t *t) {
    time_t v = (time_t)*t; struct tm tm; gmtime_r(&v, &tm);
    return (uint64_t)(uint16_t)(tm.tm_year + 1900) | (uint64_t)(uint8_t)(tm.tm_mon + 1) << 16 | (uint64_t)(uint8_t)tm.tm_mday << 24 |
           (uint64_t)(uint8_t)tm.tm_hour << 32 | (uint64_t)(uint8_t)tm.tm_min << 40 | (uint64_t)(uint8_t)tm.tm_sec << 48;
}
// LocationName = char[0x24]（时区名）：跟宿主系统时区
// LocationName = char[0x24] (time zone name): follows the host's system time zone
HLE_FORCE("_ZN2nn4time21GetDeviceLocationNameEPNS0_12LocationNameE", "设备时区名 → 宿主时区", Result, h_time_LocName, char *out) {
    memset(out, 0, 0x24); tzset(); strlcpy(out, tzname[0] && *tzname[0] ? tzname[0] : "UTC", 0x24); return RES_OK;
}
HLE_FORCE("_ZN2nn4time26StandardNetworkSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", "网络时钟（离线，不可用）", Result, h_time_NetNow, int64_t *out) { (void)out; return RES_FAIL; }
HLE_FORCE("_ZN2nn4time8FinalizeEv", "与 Initialize 配对；宿主无会话", Result, h_time_Finalize, void) { return RES_OK; }
// 「确保网络时钟可用」异步任务：不发请求，完成事件立即触发、结果失败。对象本身不动（构造 / 析构空操作），事件放宿主静态区
// "Ensure network clock availability" async task: no request is sent; the finish event fires immediately and the result is failure. The object itself is untouched (constructor / destructor no-ops), the event lives in host static storage
static _Alignas(16) uint8_t g_ev_netclock[0x100];
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTaskC1Ev", "网络时钟任务构造；宿主不建会话", void, h_time_NcCtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTaskD1Ev", "网络时钟任务析构；无会话可关", void, h_time_NcDtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask10InitializeENS_2os14EventClearModeE", "网络时钟任务初始化（离线）", Result, h_time_NcInit, void *self, int mode) { (void)self; (void)mode; return RES_OK; }
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask9StartTaskEv", "网络时钟任务开跑：立即以失败结束", Result, h_time_NcStart, void *self) { (void)self; hle_signal_system_event(g_ev_netclock); return RES_OK; }
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask14GetFinishEventEv", "网络时钟任务完成事件：已触发", void *, h_time_NcEvent, void *self) { (void)self; hle_signal_system_event(g_ev_netclock); return g_ev_netclock; }
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask9GetResultEv", "网络时钟任务结果：离线，失败", Result, h_time_NcResult, void *self) { (void)self; return RES_FAIL; }

// ---------- pl：共享字体 ----------
// 系统共享字体（标准 / 简中 / 繁中 / 韩文 / 任天堂扩展）宿主没有原版；一律给一份宿主字体（DM_SHARED_FONT 指定 TTF，缺省 macOS 的 Arial Unicode）。
// 游戏自己的文字用 RomFS 里的字体，共享字体只在系统名字等少数地方用。
// 格式：原版共享内存里是 BFTTF——8 字节头 + 按字异或混淆的 TTF；游戏拿到地址后先调 nn::fontll::ScalableFontEngineHelper::Decode(地址, 大小)
//   （SMBW sdk +0x3093c0）就地解混淆并返回地址 + 8。Decode 先看头一个字：已是「解过」标记（小端 0x18029a7f）就直接返回地址 + 8、不动数据。
//   所以宿主给「解过」标记 + 原样 TTF；第二个字照原版写混淆后的长度（键 0x49621806：存的是 bswap(长度 ^ 键)）。给裸 TTF 会被当密文异或成乱码，
//   字体引擎随后用野指针崩（SMBW main +0x107dbc0）。
// The game's own text uses the fonts in its RomFS; shared fonts are only used in a few places such as system names.
// Format: the original shared memory holds BFTTF - an 8-byte header + a TTF obfuscated by per-word XOR; after getting the address the game calls nn::fontll::ScalableFontEngineHelper::Decode(address, size)
//   (SMBW sdk +0x3093c0), which deobfuscates in place and returns address + 8. Decode first checks the first word: if it is already the "decoded" marker (little-endian 0x18029a7f) it returns address + 8 without touching the data.
//   So the host provides the "decoded" marker + the TTF as is; the second word holds the obfuscated length as in the original (key 0x49621806: stored as bswap(length ^ key)). A raw TTF would be XORed into garbage as if it were ciphertext,
//   and the font engine then crashes on a wild pointer (SMBW main +0x107dbc0).
static uint8_t *g_font; static size_t g_font_size;
static void font_load(void) {
    if (g_font) return;
    const char *p = getenv("DM_SHARED_FONT"); if (!p) p = "/System/Library/Fonts/Supplemental/Arial Unicode.ttf";
    int fd = open(p, O_RDONLY); struct stat st;
    if (fd < 0 || fstat(fd, &st)) { LOG("【红】共享字体打不开：%s（DM_SHARED_FONT 指一个 TTF）", p); if (fd >= 0) close(fd); return; }
    size_t n = (size_t)st.st_size;
    uint8_t *m = mmap(NULL, n + 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) { LOG("【红】共享字体内存分配失败（%zu 字节）", n + 8); close(fd); return; }
    size_t got = 0; while (got < n) { ssize_t r = pread(fd, m + 8 + got, n - got, (off_t)got); if (r <= 0) break; got += (size_t)r; }
    close(fd);
    if (got != n) { LOG("【红】共享字体读不全：%s（%zu / %zu 字节）", p, got, n); munmap(m, n + 8); return; }
    uint32_t done = 0x18029a7fu, len = __builtin_bswap32((uint32_t)n ^ 0x49621806u);
    memcpy(m, &done, 4); memcpy(m + 4, &len, 4);
    g_font_size = n + 8; g_font = m; LOG("共享字体：%s（%zu 字节，各类型共用；BFTTF「已解」头 + 原样 TTF）", p, n);
}
HLE_FORCE("_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE", "共享字体装载状态：已装（1）", int, h_pl_LoadState, int type) { (void)type; font_load(); return g_font ? 1 : 0; }
HLE_FORCE("_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE", "共享字体地址 → BFTTF 头（宿主字体）", const void *, h_pl_Addr, int type) { (void)type; font_load(); return g_font; }
HLE_FORCE("_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE", "共享字体大小 → 头 8 字节 + 宿主字体文件大小", size_t, h_pl_Size, int type) { (void)type; font_load(); return g_font_size; }

// ---------- vi：垂直同步事件 ----------
// 游戏给一个 SystemEventType，宿主绑定后由一条宿主线程按 60 Hz 触发（DM_VSYNC_HZ 可改）
// ---------- vi: vsync event ----------
// The game passes a SystemEventType; after binding, a host thread fires it at 60 Hz (DM_VSYNC_HZ can change it)
static void *vsync_thread(void *ev) {
    const char *e = getenv("DM_VSYNC_HZ"); double hz = e ? atof(e) : 60.0; if (hz <= 0) hz = 60.0;
    uint64_t period = (uint64_t)(1e9 / hz), next = now_ns() + period;
    for (;;) {
        uint64_t t = now_ns(); if (next > t) { struct timespec ts = {(time_t)((next - t) / 1000000000ull), (long)((next - t) % 1000000000ull)}; nanosleep(&ts, NULL); }
        hle_signal_system_event(ev); next += period; if (now_ns() > next + 4 * period) next = now_ns() + period;   // 落后太多不追帧 | do not try to catch up when far behind
    }
    return NULL;
}
HLE_FORCE("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", "垂直同步事件：宿主线程按 60 Hz 触发", Result, h_vi_VsyncEvent, void *ev, void *disp) {
    (void)disp; hle_bind_system_event(ev, 1);
    pthread_t th; if (pthread_create(&th, NULL, vsync_thread, ev)) die("垂直同步线程起不来"); pthread_detach(th);
    LOG("vi::GetDisplayVsyncEvent：%p 由宿主 60 Hz 触发", ev); return RES_OK;
}

// ---------- diag ----------
// LogMetaData 之后的文本（NN_LOG）直接进宿主日志；中止打出调用点后报红退出
// The text after LogMetaData (NN_LOG) goes straight into the host log; an abort prints the call site and exits with a failure
HLE_FORCE("_ZN2nn4diag6detail7PutImplERKNS0_11LogMetaDataEPKcm", "游戏日志 → 宿主日志", void, h_diag_Put, const void *meta, const char *s, size_t n) {
    (void)meta; while (n && (s[n - 1] == '\n' || s[n - 1] == '\0')) n--; if (n) LOG("[game] %.*s", (int)n, s);
}
// 中止：报出调用点（模块 + 偏移）与客体回溯再退出
// Abort: report the call site (module + offset) and the guest backtrace, then exit
static void diag_abort(uint32_t res, void *lr) {
    module_t *m = module_of(lr);
    LOG("游戏调用 nn::diag 中止：Result %#x，调用点 %s+%#llx", res, m ? m->name : "?", m ? (unsigned long long)((uint8_t *)lr - m->base) : (unsigned long long)(uintptr_t)lr);
    dm_backtrace((uint64_t)__builtin_frame_address(1));
    die("游戏中止");
}
HLE_FORCE("_ZN2nn4diag6detail9AbortImplEv", "游戏中止", void, h_diag_Abort0, void) { diag_abort(0, __builtin_return_address(0)); }
HLE_FORCE("_ZN2nn4diag6detail9AbortImplEPKNS_6ResultE", "游戏中止（带 Result）", void, h_diag_AbortR, const uint32_t *r) { diag_abort(r ? *r : 0, __builtin_return_address(0)); }
HLE_FORCE("_ZN2nn4diag12GetBacktraceEPmi", "回溯；宿主不提供（0 帧）", int, h_diag_Bt, uint64_t *out, int n) { (void)out; (void)n; return 0; }
HLE_FORCE("_ZN2nn4diag13GetSymbolNameEPcmm", "符号名；宿主不提供（空串）", uint64_t, h_diag_SymName, char *buf, size_t n, uint64_t addr) { (void)addr; if (buf && n) buf[0] = 0; return 0; }

// ---------- settings：语言枚举 ↔ 语言代码 ----------
// 真机 MakeLanguageCode 问 settings 服务；表是公开固定的（nn::settings::Language 顺序）。LanguageCode = char[8]
// ---------- settings: language enum <-> language code ----------
// On the real device MakeLanguageCode asks the settings service; the table is public and fixed (nn::settings::Language order). LanguageCode = char[8]
static const char *const k_lang_code[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
HLE_FORCE("_ZN2nn8settingseqERKNS0_8LanguageERKNS0_12LanguageCodeE", "语言枚举与语言代码比较（返回 bool）", unsigned, h_set_LangEq, const int32_t *lang, const char *code) {
    if ((unsigned)*lang >= sizeof k_lang_code / sizeof *k_lang_code) return 0;
    return strncmp(k_lang_code[*lang], code, 8) == 0;
}
