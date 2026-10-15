// SDK 的影片库（subsdk0 = NintendoSDK_movie，Engage）：Extractor 拆封装，Decoder 用 NVIDIA 的硬件解码器（NVOMX → NvRmChannelOpen 开 nvdrv 设备）。
// 宿主没有 nvdrv：解码线程开设备时 SDK 中止。Extractor::SetDataSource 回 0x1000（SDK 自己在没有实现对象时回的同一个码）；Engage 的 Unity 自己拆 WebM，不走它。
// Decoder 由宿主接管（下面，VP9 用 libvpx 解）：Engage 的影片是 RomFS/Data/StreamingAssets/Movie 下的 103 个 VP9 WebM（2.7 GB，无音轨），开场动画 S01F 不放完游戏不往下走。
// The SDK's movie library (subsdk0 = NintendoSDK_movie, Engage): Extractor demuxes, Decoder uses NVIDIA's hardware decoder (NVOMX → NvRmChannelOpen opens the nvdrv device).
// The host has no nvdrv: the SDK aborts when the decoder thread opens the device. Extractor::SetDataSource returns 0x1000 (the same code the SDK returns when it has no implementation object); Engage's Unity demuxes WebM itself and does not use it.
// The host takes over Decoder (below; VP9 is decoded with libvpx): Engage's movies are 103 VP9 WebM files under RomFS/Data/StreamingAssets/Movie (2.7 GB, no audio track), and the game does not go on until the opening movie S01F has played.
#include "hle.h"
#include <arm_neon.h>
#include <pthread.h>
#include <unistd.h>
#include "vpx/vpx_decoder.h"
#include "vpx/vp8dx.h"

#if 0
HLE_FORCE("_ZN5movie9Extractor13SetDataSourceEPKc", "影片拆封装：宿主还不放影片，开片失败（0x1000）", int, h_movie_SetSource, void *self, const char *path) {
    (void)self; static int red; if (!red++) LOG("【红】影片：宿主不拆封装，开片报失败（首个 %s）", path ? path : "(null)");
    return 0x1000;
}
#endif


// ---------- 解码器 | Decoder ----------
// SDK 的 Decoder 是 Android MediaCodec 的薄封装（DecoderImpl 包 android::MediaCodec）：
//   · 客户端注册 4 个 nn::os 事件 DecoderEvents {输入有空、输出就绪、格式变了、出错}，有事就 SignalEvent；
//   · 输入：AcquireInputBufferIndex 取空输入号（没有回 0x1011）→ GetInputBuffer 把数据区交给客户端的 movie::Buffer → SendInputBufferForDecode(号, 偏移, 长, 时间 µs, 标志)；
//   · 输出：AcquireOutputBufferIndex(号, 时间, 标志, 余数)（没有回 0x100e）→ GetOutputBufferFormat / GetOutputBuffer 把一帧 NV12 拷进客户端自己分配的缓冲 → ReleaseOutputBufferIndex。
//   Unity（mode 1、输出格式 4 = NV12）按 GetOutputBufferSize 分配输出缓冲，从 MediaData 读 width / height / crop / nv12-y-stride / nv12-y-offset / nv12-uv-offset / slice-height。
// 宿主实现同一协议，VP9 用 libvpx 解（dm/third_party/libvpx）。输入包按到达顺序排队；有空输出槽就取队头解一包、还输入号，出了画面就进就绪输出队列，时间与标志照抄
// （流结束标志随最后一帧传回）。输出槽都被客户端占着时包留在队里、输入号不还（同 MediaCodec：解码器只在有空输出时消耗输入），客户端还一个输出号就再解一包。
// 尺寸取 Configure 的 MediaData，没有时取首个关键帧的 VP9 帧头。Unity 按宽 × 高 × 1.5 自己分配输出缓冲（不调 GetOutputBufferSize），所以行距 = 宽、平面高 = 高，不对齐。
// 解码在每个解码器自己的宿主线程上做，送包 / 还输出号的客体线程等它做完：客体线程跑在 SDK 给的客体栈上，libvpx 的栈帧大；事件仍由客体线程发（SignalEvent 是客体代码）。
// 对 Buffer / MediaData / 事件的操作都调 SDK 自己的函数（guest_sym），对象布局不用宿主知道。
// The SDK's Decoder is a thin wrapper over Android's MediaCodec (DecoderImpl wraps android::MediaCodec):
//   · the client registers 4 nn::os events DecoderEvents {input available, output ready, format changed, error}, signalled with SignalEvent;
//   · input: AcquireInputBufferIndex takes a free input index (none: 0x1011) → GetInputBuffer hands its data area to the client's movie::Buffer → SendInputBufferForDecode(index, offset, size, time µs, flags);
//   · output: AcquireOutputBufferIndex(index, time, flags, remaining) (none: 0x100e) → GetOutputBufferFormat / GetOutputBuffer copy one NV12 frame into a buffer the client allocated → ReleaseOutputBufferIndex.
//   Unity (mode 1, output format 4 = NV12) allocates its output buffers by GetOutputBufferSize and reads width / height / crop / nv12-y-stride / nv12-y-offset / nv12-uv-offset / slice-height from the MediaData.
// The host implements the same protocol and decodes VP9 with libvpx (dm/third_party/libvpx). Input packets queue in arrival order; whenever an output slot is free, the head packet is decoded and its input index returned, and a picture goes to the ready-output queue with its time and flags copied
// (the end-of-stream flag comes back with the last frame). While the client holds every output slot, packets stay queued and their input indices are not returned (as with MediaCodec: a decoder only consumes input when it has a free output); each output index the client returns lets one more packet be decoded.
// The size comes from Configure's MediaData, or else from the VP9 header of the first key frame. Unity allocates its output buffers itself as width × height × 1.5 (it does not call GetOutputBufferSize), so the stride is the width and the plane height the height, unaligned.
// Decoding runs on each decoder's own host thread, and the guest thread that sends a packet or returns an output index waits for it: guest threads run on guest stacks given by the SDK, and libvpx's stack frames are large. Events are still signalled by guest threads (SignalEvent is guest code).
// Buffer / MediaData / event operations call the SDK's own functions (guest_sym), so the host needs no knowledge of their layouts.

#define MD_NIN 4                 // 输入缓冲数 | input buffers
#define MD_INCAP (4u << 20)      // 每个输入缓冲 4 MB（1080p VP9 一包远小于此） | 4 MB per input buffer (a 1080p VP9 packet is far smaller)
#define MD_NOUT 8                // 输出槽数 | output slots
#define MD_NO_INPUT 0x1011       // SDK：没有空输入 | SDK: no free input
#define MD_NO_OUTPUT 0x100e      // SDK：没有就绪输出 | SDK: no output ready
#define MD_FMT_NV12 4
#define MD_FMT_ABGR 3            // 4 字节 RGBA（内存序 R G B A），只配原生贴图模式 | 4-byte RGBA (memory order R G B A), only with the native texture mode
#define MD_MODE_TEX 2            // 原生贴图模式：解码器把帧写进客户端的 NVN 贴图 | native texture mode: the decoder writes frames into the client's NVN texture
#define MD_TYPE_VP9 3

typedef struct {
    pthread_mutex_t mu;                          // 护下面的队列与槽 | guards the queues and slots below
    int type, fmt, mode;
    void *ev[4];                                 // DecoderEvents 的副本：{输入, 输出, 格式, 出错} 的 EventType*。客户端的结构不一定常驻（银河战士 生存恐惧第二段影片时已被改写，Start 发信号崩），登记时抄下 | a copy of DecoderEvents: {input, output, format, error} EventType*. The client's struct need not stay alive (by Metroid Dread's second movie it had been overwritten and Start crashed signalling), so it is copied at registration
    int w, h, fmt_sent, started;
    uint8_t *in[MD_NIN]; int infree[MD_NIN], ninfree;                     // 空输入号队列 | queue of free input indices
    struct { int used, pic; int64_t pts; uint32_t flags; uint8_t *frm; size_t cap; } out[MD_NOUT];   // pic：frm 里有解出的画面；cap：frm 的字节数 | pic: frm holds a decoded picture; cap: bytes in frm
    int outq[MD_NOUT], noutq;                                             // 就绪输出号队列 | queue of ready output indices
    struct { int in, off, size; int64_t pts; uint32_t flags; } pend[MD_NIN]; int npend;   // 等解码的输入包（按到达顺序） | input packets waiting to be decoded (in arrival order)
    // 解码：dmu 让同一时刻只有一个客体线程在取包解码（包按顺序进 libvpx）；wmu / wcv 把一件活交给宿主线程并等它做完
    // Decoding: dmu lets only one guest thread take and decode packets at a time (packets enter libvpx in order); wmu / wcv hand one job to the host thread and wait for it
    pthread_mutex_t dmu, wmu; pthread_cond_t wcv; pthread_t wk;
    int job; const uint8_t *jp; int jn, jslot, jgot;
    vpx_codec_ctx_t vpx; int vpx_on, red_dec, red_img;
    uint64_t dec_n, dec_ns, dec_max;            // 解码计时（宿主线程上，每包） | decode timing (on the host thread, per packet)
    unsigned long ncall[24];
} mdec_t;

static struct {
    void (*signal)(void *ev);
    void (*set_data)(void *buf, void *data, size_t cap);
    void (*set_range)(void *buf, int off, int size);
    void *(*data)(const void *buf);
    size_t (*cap)(const void *buf);
    void (*set_i32)(void *md, const char *k, int v);
    void (*set_rect)(void *md, const char *k, int l, int t, int r, int b);
    int (*find_i32)(const void *md, const char *k, int *v);
    void *(*nvn_sync)(const void *buf);   // 只有原生贴图模式用；Engage 的 subsdk0 也有，但不强求 | used only by the native texture mode; Engage's subsdk0 has it too, but it is not required
} G;

static void *msym(const char *n) { void *p = guest_sym(n); if (!p) die("影片：缺 SDK 符号 %s", n); return p; }
static void md_syms(void) {
    if (G.signal) return;
    G.set_data = msym("_ZN5movie6Buffer18SetDataAndCapacityEPvm");
    G.set_range = msym("_ZN5movie6Buffer8SetRangeEii");
    G.data = msym("_ZNK5movie6Buffer4DataEv");
    G.cap = msym("_ZNK5movie6Buffer8CapacityEv");
    G.set_i32 = msym("_ZN5movie9MediaData8SetInt32EPKci");
    G.set_rect = msym("_ZN5movie9MediaData7SetRectEPKciiii");
    G.find_i32 = msym("_ZNK5movie9MediaData9FindInt32EPKcPi");
    G.signal = msym("_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
}
static mdec_t *md_of(void **self) { return self ? (mdec_t *)self[1] : NULL; }
static void md_event(mdec_t *d, int i) { if (d->ev[i]) G.signal(d->ev[i]); }   // 0 输入 1 输出 2 格式 3 出错 | 0 input 1 output 2 format 3 error
// 延后发的事件：Start / Flush 之后的「有输入」。SDK 的解码器从自己的编解码线程异步发它；银河战士 生存恐惧在 RegisterClientEvents 之前就 InitializeMultiWaitHolder，
// 之后才把等待项挂进自己的 MultiWait。Start 里同步发信号时等待项还没挂上，SignalEvent 进 MultiWaitImpl::NotifyAndWakeupThread 用空的 MultiWait（0x20）崩
// （同一块内存第二次建解码器时才出现）。改由一条客体线程约 2 ms 后发（SignalEvent 是客体代码，要在客体线程上调）；析构时在同一把锁下撤销该解码器的待发项。
// 这次崩溃与工作线程在 main+0x225338 的崩溃，根子都是宿主没有同核抢占（真机上游戏的分发、工作线程会抢占主线程）；hle_thread.c 现已近似实现（svc_thread、dm_yield_to_core_peers）
// Deferred events: "input available" after Start / Flush. The SDK's decoder signals it asynchronously from its own codec thread; Metroid Dread calls InitializeMultiWaitHolder before RegisterClientEvents
// and only later links the holders into its MultiWait. Signalled synchronously inside Start, the holders were not linked yet and SignalEvent crashed in MultiWaitImpl::NotifyAndWakeupThread on a null MultiWait (0x20)
// (it showed only when a decoder was built a second time in the same memory). A guest thread now signals it about 2 ms later (SignalEvent is guest code and must be called on a guest thread); the destructor drops that decoder's pending signals under the same lock.
// The root of that crash and of the worker crash at main+0x225338 was missing same-core preemption (the game's dispatcher and worker preempt the main thread on hardware); hle_thread.c now approximates it (svc_thread, dm_yield_to_core_peers)
void *dm_guest_thread_spawn(void (*fn)(void *), void *arg, int prio, size_t stack_size);
static pthread_mutex_t g_dfmu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_dfcv = PTHREAD_COND_INITIALIZER;
static struct { void *d; int ev; uint64_t due; } g_dfq[64]; static int g_ndfq, g_dfthr;
static void md_defer_body(void *arg);
static void md_defer(void *d, int ev) {
    pthread_mutex_lock(&g_dfmu);
    if (!g_dfthr) { g_dfthr = 1; dm_guest_thread_spawn(md_defer_body, NULL, 16, 64 << 10); }
    if (g_ndfq < 64) g_dfq[g_ndfq++] = (__typeof__(g_dfq[0])){d, ev, now_ns() + 2000000};
    else LOG("【红】影片：延后事件队列满，丢一个");
    pthread_cond_signal(&g_dfcv); pthread_mutex_unlock(&g_dfmu);
}
static void md_defer_drop(void *d) {
    pthread_mutex_lock(&g_dfmu);
    int n = 0; for (int i = 0; i < g_ndfq; i++) if (g_dfq[i].d != d) g_dfq[n++] = g_dfq[i];
    g_ndfq = n; pthread_mutex_unlock(&g_dfmu);
}
// 持锁发信号：析构先在此锁下撤销待发项，所以发信号与析构不会交错 | signal under the lock: the destructor first drops pending entries under it, so a signal never interleaves with destruction
static void md_defer_body(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_dfmu);
    for (;;) {
        uint64_t now = now_ns(), next = 0; int k = -1;
        for (int i = 0; i < g_ndfq && k < 0; i++) { if (g_dfq[i].due <= now) k = i; else if (!next || g_dfq[i].due < next) next = g_dfq[i].due; }
        if (k >= 0) {
            mdec_t *d = g_dfq[k].d; int e = g_dfq[k].ev;
            memmove(g_dfq + k, g_dfq + k + 1, (size_t)(--g_ndfq - k) * sizeof *g_dfq);
            if (d->started) md_event(d, e);   // 停止后不发：游戏在 Stop 之后就拆事件（Flush 后 2 ms 内 Stop 时，延后的信号打进正在拆的事件崩） | not after Stop: the game takes its events apart right after Stop (a Stop within 2 ms of a Flush let the deferred signal hit events being torn down)
            continue;
        }
        if (!next) pthread_cond_wait(&g_dfcv, &g_dfmu);
        else { uint64_t w = next - now; struct timespec ts = { (time_t)(w / 1000000000u), (long)(w % 1000000000u) }; pthread_cond_timedwait_relative_np(&g_dfcv, &g_dfmu, &ts); }
    }
}
// 银河战士 生存恐惧 2.1.0 的影片播放器：主线程把等待项「挂上 / 摘下」请求投进分发线程的请求环（main+0x222cc8：环 +0x368 写指针、+0x370 待处理数，
// 分发线程在 main+0x222d94 里逐项挂上或摘下并 Finalize 等待项与事件、把待处理数减到 0），投完不等就往下走：摘下后随即释放包装对象、在原处建新对象，挂上后马上 Start。
// 真机上分发线程（优先级 30，只许核 0）立即抢占主线程（优先级 44，核 0），请求先处理完；宿主各线程并行，旧等待项还挂在分发线程的 MultiWait 上时内存已被重用，
// WaitAny / SignalEvent 走到已释放的等待项崩（同核抢占近似 core_preempt 管不到：分发线程正忙时投请求不叫醒任何人，主线程一路没有 SVC）。
// 改写器在两处「投完一项」后的指令上插观测门（patch_nso.py OBS 19 / 20，只在这一构建），这里等待处理数归零，最多 3 s。
// 100 ms 在 Mac 满载时不够（同时在出 iOS 着色器库）：等到点仍有一项没处理，随即 WaitAny 崩。超过 50 ms 的等待记前 8 次
// Metroid Dread 2.1.0's movie player: the main thread posts "link / unlink holders" requests into the dispatcher thread's request ring (main+0x222cc8: ring +0x368 write pointer, +0x370 pending count;
// the dispatcher links or unlinks each entry in main+0x222d94, finalizes the holders and events, and counts the pending number down to 0) and goes on without waiting: after an unlink it frees the wrapper object and builds a new one in place, after a link it calls Start at once.
// On hardware the dispatcher thread (priority 30, core 0 only) preempts the main thread (priority 44, core 0) and the requests are done first; on the host the threads run in parallel, the memory was reused while the old holders were still linked into the dispatcher's MultiWait,
// and WaitAny / SignalEvent walked into freed holders (the core_preempt approximation does not cover it: with the dispatcher busy, posting wakes nobody, and the main thread makes no SVC on the way).
// The rewriter puts observation gates on the instruction after each "one request posted" (patch_nso.py OBS 19 / 20, for this build only); here we wait for the pending count to reach zero, for at most 3 s.
// 100 ms was too short once the Mac was saturated (an iOS shader build in parallel): the wait ran out with one request pending and WaitAny crashed right after. Waits over 50 ms are logged (the first eight)
static void movie_ring_wait(const uint64_t *regs) {
    const volatile uint64_t *pending = (const volatile uint64_t *)(regs[19] + 0x370);
    uint64_t t0 = now_ns();
    while (*pending) {
        if (now_ns() - t0 > 3000000000ull) { static int r; if (!r++) LOG("【红】影片：播放器请求环 3 s 没处理完（待处理 %llu），照常放行", (unsigned long long)*pending); return; }
        struct timespec ts = {0, 50000}; nanosleep(&ts, 0);
    }
    uint64_t dt = now_ns() - t0; static _Atomic int nlong; if (dt > 50000000ull && nlong++ < 8) LOG("[影片] 播放器请求环等了 %.1f ms", dt / 1e6);
}
__attribute__((constructor)) static void movie_ring_gates(void) { gate_obs_set(OBS_MOVIE_RING_LINK, movie_ring_wait); gate_obs_set(OBS_MOVIE_RING_UNLINK, movie_ring_wait); }
// 前 12 次逐条记，之后每 2^k 次记一次 | log the first 12 calls one by one, then every 2^k-th
#define MD_TRACE(d, slot, ...) do { unsigned long n_ = ++(d)->ncall[slot]; if (n_ <= 12 || !(n_ & (n_ - 1))) LOG("[影片] " __VA_ARGS__); } while (0)

// VP9 关键帧的未压缩帧头 → 宽高（VP9 规范 6.2：frame_marker、profile、show_existing_frame、frame_type、show_frame、error_resilient_mode、同步码、颜色配置、frame_size）
// VP9 key frame uncompressed header → width and height (VP9 spec 6.2: frame_marker, profile, show_existing_frame, frame_type, show_frame, error_resilient_mode, sync code, color config, frame_size)
static int vp9_size(const uint8_t *p, int n, int *w, int *h) {
    int bit = 0;
    #define RB(k) ({ unsigned v_ = 0; for (int i_ = 0; i_ < (k); i_++, bit++) { if (bit >> 3 >= n) return 0; v_ = v_ << 1 | (p[bit >> 3] >> (7 - (bit & 7)) & 1); } v_; })
    if (RB(2) != 2) return 0;
    unsigned lo = RB(1), hi = RB(1), prof = hi << 1 | lo;
    if (prof == 3) RB(1);
    if (RB(1)) return 0;                       // show_existing_frame
    if (RB(1) != 0) return 0;                  // 不是关键帧 | not a key frame
    RB(2);                                     // show_frame, error_resilient_mode
    if (RB(24) != 0x498342) return 0;
    if (prof >= 2) RB(1);
    if (RB(3) != 7) { RB(1); if (prof == 1 || prof == 3) RB(3); }
    else if (prof == 1 || prof == 3) RB(1);
    *w = (int)RB(16) + 1; *h = (int)RB(16) + 1;
    #undef RB
    return 1;
}
static void md_dims(const mdec_t *d, int *w, int *h, int *stride, int *slice) {
    *w = d->w > 0 ? d->w : 1920; *h = d->h > 0 ? d->h : 1080;
    *stride = *w; *slice = *h;
}

// ---------- 宿主解码线程 | Host decoding thread ----------
enum { MDJ_NONE, MDJ_DECODE, MDJ_CLOSE, MDJ_QUIT };
static int md_threads(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n >= 8 ? 4 : 2; }
// I420 → NV12（行距 = 宽、平面高 = 高）；图比槽小的部分补黑，大的裁掉 | I420 → NV12 (stride = width, plane height = height); a smaller picture is padded with black, a larger one cropped
static void md_nv12(const vpx_image_t *im, uint8_t *dst, int w, int h) {
    int cw = (int)im->d_w < w ? (int)im->d_w : w, ch = (int)im->d_h < h ? (int)im->d_h : h;
    if (cw < w || ch < h) { memset(dst, 16, (size_t)w * h); memset(dst + (size_t)w * h, 128, (size_t)w * h / 2); }
    for (int y = 0; y < ch; y++) memcpy(dst + (size_t)y * w, im->planes[0] + (size_t)y * im->stride[0], cw);
    int cw2 = (cw + 1) / 2, ch2 = (ch + 1) / 2;
    for (int y = 0; y < ch2 && y < h / 2; y++) {
        const uint8_t *u = im->planes[1] + (size_t)y * im->stride[1], *v = im->planes[2] + (size_t)y * im->stride[2];
        uint8_t *o = dst + (size_t)w * h + (size_t)y * w;
        int x = 0;
        for (; x + 16 <= cw2; x += 16) { uint8x16x2_t t = { { vld1q_u8(u + x), vld1q_u8(v + x) } }; vst2q_u8(o + 2 * x, t); }
        for (; x < cw2 && 2 * x + 1 < w; x++) { o[2 * x] = u[x]; o[2 * x + 1] = v[x]; }
    }
}
// I420 → RGBA（原生贴图模式）：按画面标的色彩空间与范围（VP9 色彩配置；未标时 720 行以上按 BT.709，以下 BT.601），16 位定点；图比槽小的部分补黑，大的裁掉
// I420 → RGBA (native texture mode): by the colour space and range the picture declares (VP9 colour config; when undeclared BT.709 from 720 lines up, else BT.601), 16-bit fixed point; a smaller picture is padded with black, a larger one cropped
static void md_rgba(const vpx_image_t *im, uint8_t *dst, int w, int h) {
    int full = im->range == VPX_CR_FULL_RANGE, b709 = im->cs == VPX_CS_BT_709 || (im->cs == VPX_CS_UNKNOWN && h >= 720);
    double ky = full ? 1.0 : 255.0 / 219.0, kc = full ? 1.0 : 255.0 / 224.0;
    double rv = b709 ? 1.5748 : 1.402, gu = b709 ? 0.1873 : 0.3441, gv = b709 ? 0.4681 : 0.7141, bu = b709 ? 1.8556 : 1.772;
    int Y = (int)(ky * 65536), RV = (int)(rv * kc * 65536), GU = (int)(gu * kc * 65536), GV = (int)(gv * kc * 65536), BU = (int)(bu * kc * 65536), y0 = full ? 0 : 16;
    int cw = (int)im->d_w < w ? (int)im->d_w : w, ch = (int)im->d_h < h ? (int)im->d_h : h;
    if (cw < w || ch < h) for (size_t i = 0; i < (size_t)w * h; i++) ((uint32_t *)dst)[i] = 0xff000000u;
    #define CL(v) ((v) < 0 ? 0 : (v) > 255 ? 255 : (v))
    for (int y = 0; y < ch; y++) {
        const uint8_t *py = im->planes[0] + (size_t)y * im->stride[0], *pu = im->planes[1] + (size_t)(y >> 1) * im->stride[1], *pv = im->planes[2] + (size_t)(y >> 1) * im->stride[2];
        uint8_t *o = dst + (size_t)y * w * 4;
        for (int x = 0; x < cw; x++) {
            int l = (py[x] - y0) * Y, u = pu[x >> 1] - 128, v = pv[x >> 1] - 128;
            int r = (l + RV * v + 32768) >> 16, g = (l - GU * u - GV * v + 32768) >> 16, b = (l + BU * u + 32768) >> 16;
            o[4 * x] = (uint8_t)CL(r); o[4 * x + 1] = (uint8_t)CL(g); o[4 * x + 2] = (uint8_t)CL(b); o[4 * x + 3] = 255;
        }
    }
    #undef CL
}
// 解一包进输出槽 slot：1 = 出了一帧，0 = 没出（隐藏帧或出错） | decode one packet into output slot slot: 1 = a picture came out, 0 = none (hidden frame or error)
static int md_vp9(mdec_t *d, const uint8_t *p, int n, int slot) {
    if (!d->vpx_on) {
        vpx_codec_dec_cfg_t cfg = { .threads = (unsigned)md_threads(), .w = (unsigned)d->w, .h = (unsigned)d->h };
        vpx_codec_err_t e = vpx_codec_dec_init(&d->vpx, vpx_codec_vp9_dx(), &cfg, 0);
        if (e) { LOG("【红】影片：libvpx 建 VP9 解码器失败：%s", vpx_codec_err_to_string(e)); return 0; }
        vpx_codec_control(&d->vpx, VP9D_SET_ROW_MT, 1);
        d->vpx_on = 1;
        LOG("[影片] libvpx %s：VP9 解码器 %d 线程", vpx_codec_version_str(), cfg.threads);
    }
    if (vpx_codec_decode(&d->vpx, p, (unsigned)n, NULL, 0)) {
        if (!d->red_dec++) LOG("【红】影片：VP9 解码出错（首例，%d 字节）：%s %s", n, vpx_codec_error(&d->vpx), vpx_codec_error_detail(&d->vpx) ? vpx_codec_error_detail(&d->vpx) : "");
        return 0;
    }
    vpx_codec_iter_t it = NULL; vpx_image_t *im, *last = NULL;
    while ((im = vpx_codec_get_frame(&d->vpx, &it))) last = im;
    if (!last) return 0;
    if (last->fmt != VPX_IMG_FMT_I420) { if (!d->red_img++) LOG("【红】影片：VP9 画面格式 %#x 不是 I420（4:2:0 8 位），出黑帧", last->fmt); return 0; }
    if ((int)last->d_w != d->w || (int)last->d_h != d->h) { if (!d->red_img++) LOG("【红】影片：VP9 画面 %u×%u 与输出 %d×%d 不符（首例），裁剪 / 补黑", last->d_w, last->d_h, d->w, d->h); }
    int rgba = d->fmt == MD_FMT_ABGR;
    size_t need = (size_t)d->w * d->h * (rgba ? 4 : 3) / (rgba ? 1 : 2);
    if (d->out[slot].cap < need) { free(d->out[slot].frm); d->out[slot].frm = malloc(need); d->out[slot].cap = need; }   // Reset 后再 Configure 可能换了尺寸 | a Configure after Reset may change the size
    if (rgba) md_rgba(last, d->out[slot].frm, d->w, d->h); else md_nv12(last, d->out[slot].frm, d->w, d->h);
    return 1;
}
static void md_close(mdec_t *d) { if (d->vpx_on) { vpx_codec_destroy(&d->vpx); d->vpx_on = 0; } }
static void *md_worker(void *arg) {
    mdec_t *d = arg;
    pthread_setname_np("dm 影片解码 | movie decoder");
    pthread_mutex_lock(&d->wmu);
    for (;;) {
        while (d->job == MDJ_NONE) pthread_cond_wait(&d->wcv, &d->wmu);
        int job = d->job;
        pthread_mutex_unlock(&d->wmu);
        if (job == MDJ_DECODE) {
            uint64_t t0 = now_ns();
            d->jgot = md_vp9(d, d->jp, d->jn, d->jslot);
            uint64_t dt = now_ns() - t0; d->dec_n++; d->dec_ns += dt; if (dt > d->dec_max) d->dec_max = dt;
        }
        else md_close(d);
        pthread_mutex_lock(&d->wmu);
        d->job = MDJ_NONE; pthread_cond_broadcast(&d->wcv);
        if (job == MDJ_QUIT) break;
    }
    pthread_mutex_unlock(&d->wmu);
    return NULL;
}
// 把一件活交给宿主线程并等它做完（调用方持 dmu） | hand one job to the host thread and wait for it (the caller holds dmu)
static void md_job(mdec_t *d, int job) {
    pthread_mutex_lock(&d->wmu);
    d->job = job; pthread_cond_broadcast(&d->wcv);
    while (d->job != MDJ_NONE) pthread_cond_wait(&d->wcv, &d->wmu);
    pthread_mutex_unlock(&d->wmu);
}
// 有包排队又有空输出槽就解队头，直到队空或槽满；按结果发输入 / 输出事件 | while packets are queued and an output slot is free, decode the head packet, until the queue is empty or every slot is taken; signal the input / output events by the result
static void md_pump(mdec_t *d) {
    pthread_mutex_lock(&d->dmu);
    for (;;) {
        pthread_mutex_lock(&d->mu);
        int slot = -1;
        if (d->npend) for (int k = 0; k < MD_NOUT; k++) if (!d->out[k].used) { slot = k; break; }
        if (slot < 0) { pthread_mutex_unlock(&d->mu); break; }
        __typeof__(d->pend[0]) pk = d->pend[0];
        memmove(d->pend, d->pend + 1, --d->npend * sizeof *d->pend);
        d->out[slot].used = 1;
        pthread_mutex_unlock(&d->mu);
        int got = 0;
        if (pk.size > 0 && d->type == MD_TYPE_VP9) { d->jp = d->in[pk.in] + pk.off; d->jn = pk.size; d->jslot = slot; md_job(d, MDJ_DECODE); got = d->jgot; }
        int eos = (pk.flags & 4) != 0;   // MediaCodec BUFFER_FLAG_END_OF_STREAM
        pthread_mutex_lock(&d->mu);
        d->infree[d->ninfree++] = pk.in;
        int ready = got || eos || d->type != MD_TYPE_VP9;   // 没解出画面的流结束包也要出一个输出，把标志带回去；AVC / VP8 没有解码器，出黑帧 | an end-of-stream packet without a picture still yields an output to carry the flag back; AVC / VP8 have no decoder and give black frames
        if (ready) { d->out[slot].pic = got; d->out[slot].pts = pk.pts; d->out[slot].flags = pk.flags; d->outq[d->noutq++] = slot; }
        else d->out[slot].used = 0;
        pthread_mutex_unlock(&d->mu);
        md_event(d, 0);
        if (ready) md_event(d, 1);
    }
    pthread_mutex_unlock(&d->dmu);
}

HLE_FORCE("_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", "影片解码器：宿主实现（不建 NVIDIA 硬件解码器，VP9 用 libvpx）",
          void, h_movie_Decoder, void **self, int type, int fmt, int mode, int prio) {
    static void *vt; if (!vt) { uint8_t *t = guest_sym("_ZTVN5movie7DecoderE"); if (!t) die("影片：缺 movie::Decoder 的虚表"); vt = t + 0x10; }
    md_syms();
    self[0] = vt; self[1] = NULL;
    // 只做视频（AVC / VP8 / VP9 = 1..3）：NV12 输出（Engage 的 Unity，CPU 缓冲），或 RGBA 输出进客户端 NVN 贴图（模式 2，银河战士 生存恐惧）；
    // 别的（音频）impl 留空，各方法回 SDK 的失败码。只有 VP9 真解码，AVC / VP8 出黑帧
    // Only video (AVC / VP8 / VP9 = 1..3): NV12 output (Engage's Unity, CPU buffers), or RGBA output into the client's NVN texture (mode 2, Metroid Dread);
    // anything else (audio) keeps a null impl and every method returns the SDK's failure code. Only VP9 is really decoded; AVC / VP8 give black frames
    if (type < 1 || type > 3 || (fmt != MD_FMT_NV12 && !(fmt == MD_FMT_ABGR && mode == MD_MODE_TEX))) { LOG("【红】影片：解码器类型 %d 输出 %d 模式 %d 宿主不支持，各方法回失败", type, fmt, mode); return; }
    if (type != MD_TYPE_VP9) LOG("【红】影片：解码器类型 %d（AVC / VP8）宿主不解码，出黑帧", type);
    mdec_t *d = calloc(1, sizeof *d);
    pthread_mutex_init(&d->mu, NULL); pthread_mutex_init(&d->dmu, NULL); pthread_mutex_init(&d->wmu, NULL); pthread_cond_init(&d->wcv, NULL);
    d->type = type; d->fmt = fmt; d->mode = mode;
    for (int i = 0; i < MD_NIN; i++) { d->in[i] = malloc(MD_INCAP); d->infree[i] = i; }
    d->ninfree = MD_NIN;
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 1 << 20);
    if (pthread_create(&d->wk, &a, md_worker, d)) die("影片：建解码线程失败");
    pthread_attr_destroy(&a);
    self[1] = d;
    LOG("[影片] 解码器 %p：类型 %d 输出 %d 模式 %d 优先级 %d", (void *)self, type, fmt, mode, prio);
}
static void md_free(void **self) {
    mdec_t *d = md_of(self); if (!d) return;
    md_defer_drop(d);
    self[1] = NULL;
    LOG("[影片] 解码器 %p 析构：送入 %lu 包，取出 %lu 帧；VP9 解 %llu 包，每包均 %.2f ms、最长 %.2f ms", (void *)self, d->ncall[7], d->ncall[8],
        (unsigned long long)d->dec_n, d->dec_n ? d->dec_ns / 1e6 / d->dec_n : 0.0, d->dec_max / 1e6);
    pthread_mutex_lock(&d->dmu); md_job(d, MDJ_QUIT); pthread_mutex_unlock(&d->dmu);
    pthread_join(d->wk, NULL);
    for (int i = 0; i < MD_NIN; i++) free(d->in[i]);
    for (int k = 0; k < MD_NOUT; k++) free(d->out[k].frm);
    pthread_mutex_destroy(&d->mu); pthread_mutex_destroy(&d->dmu); pthread_mutex_destroy(&d->wmu); pthread_cond_destroy(&d->wcv); free(d);
}
HLE_FORCE("_ZN5movie7DecoderD2Ev", "影片解码器析构：释放宿主对象", void, h_movie_DecD2, void **self) { md_free(self); }
// 完整对象析构（D1）：银河战士 生存恐惧直接调它（main+0x225a08）；没接管时 SDK 把宿主对象当 DecoderImpl 析构而崩 | complete-object destructor (D1): Metroid Dread calls it directly (main+0x225a08); without the take-over the SDK destroys the host object as a DecoderImpl and crashes
HLE_FORCE("_ZN5movie7DecoderD1Ev", "影片解码器析构（完整对象）：释放宿主对象", void, h_movie_DecD1, void **self) { md_free(self); }
// 删除式析构：Unity 用自己的分配器建对象、先调 D2 再自己释放，这里不释放对象本身 | deleting destructor: Unity allocates the object with its own allocator, calls D2 and frees it itself; the object itself is not freed here
HLE_FORCE("_ZN5movie7DecoderD0Ev", "影片解码器删除式析构：释放宿主对象", void, h_movie_DecD0, void **self) { LOG("【红】影片：删除式析构（对象本身未释放）"); md_free(self); }

HLE_FORCE("_ZN5movie7Decoder9ConfigureEPNS_9MediaDataE", "影片解码器配置", int, h_movie_Configure, void **self, void *md) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    int w = 0, h = 0; if (md) { G.find_i32(md, "width", &w); G.find_i32(md, "height", &h); }
    pthread_mutex_lock(&d->mu); if (w > 0 && h > 0 && !d->fmt_sent) { d->w = w; d->h = h; } pthread_mutex_unlock(&d->mu);
    MD_TRACE(d, 0, "Configure：MediaData 宽 %d 高 %d", w, h);
    return 0;
}
HLE_FORCE("_ZN5movie7Decoder20RegisterClientEventsEPNS_13DecoderEventsE", "影片解码器：登记客户端事件", int, h_movie_RegEv, void **self, void **ev) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    for (int i = 0; i < 4; i++) d->ev[i] = ev ? ev[i] : NULL;
    MD_TRACE(d, 1, "RegisterClientEvents：%p %p %p %p", ev ? ev[0] : 0, ev ? ev[1] : 0, ev ? ev[2] : 0, ev ? ev[3] : 0);
    return 0;
}
HLE_FORCE("_ZN5movie7Decoder5StartEv", "影片解码器开始：输入全空，通知客户端", int, h_movie_Start, void **self) {
    mdec_t *d = md_of(self); if (!d) return 0x2002;
    pthread_mutex_lock(&d->mu); d->started = 1; pthread_mutex_unlock(&d->mu);
    MD_TRACE(d, 2, "Start");
    md_defer(d, 0);
    return 0;
}
// 清空队列与槽；close 时连 libvpx 解码器也关掉（下一包重建） | clear the queues and slots; with close, the libvpx decoder is closed too (rebuilt at the next packet)
static void md_reset(mdec_t *d, int close) {
    pthread_mutex_lock(&d->dmu);   // 等正在解的那包做完 | wait for the packet being decoded
    if (close) md_job(d, MDJ_CLOSE);
    pthread_mutex_lock(&d->mu);
    for (int i = 0; i < MD_NIN; i++) d->infree[i] = i;
    d->ninfree = MD_NIN; d->noutq = 0; d->npend = 0;
    for (int i = 0; i < MD_NOUT; i++) d->out[i].used = d->out[i].pic = 0;
    pthread_mutex_unlock(&d->mu);
    pthread_mutex_unlock(&d->dmu);
}
HLE_FORCE("_ZN5movie7Decoder4StopEv", "影片解码器停止", int, h_movie_Stop, void **self) {
    mdec_t *d = md_of(self); if (!d) return 0x2003;
    MD_TRACE(d, 3, "Stop");
    pthread_mutex_lock(&g_dfmu); pthread_mutex_lock(&d->mu); d->started = 0; pthread_mutex_unlock(&d->mu); pthread_mutex_unlock(&g_dfmu);   // 延后线程持 g_dfmu 发信号：拿到它即保证此后不再发 | the deferred thread signals holding g_dfmu: taking it guarantees no signal afterwards
    md_defer_drop(d);
    md_reset(d, 1); return 0;
}
HLE_FORCE("_ZN5movie7Decoder5ResetEv", "影片解码器复位", int, h_movie_Reset, void **self) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    MD_TRACE(d, 4, "Reset"); md_reset(d, 1); d->fmt_sent = 0; return 0;
}
// 清空：输入输出都还回去，libvpx 保留（跳转后的下一包是关键帧）。不发「有输入」：同 MediaCodec，清空后要等再 Start 才通知。
// 银河战士 生存恐惧片尾是 Flush、Flush、Stop、析构，清空时发的信号让「有输入」处理把包装对象投进消息队列，对象随即析构，工作线程读到已释放对象崩；循环播放时它 Flush 后会再 Start
// flush: inputs and outputs all go back; libvpx is kept (the next packet after a seek is a key frame). No "input available": as with MediaCodec, nothing is announced after a flush until the next Start.
// Metroid Dread ends a movie with Flush, Flush, Stop, destroy; a signal raised at the flush let its "input available" handler post the wrapper object to the message queue, the object was destroyed right after, and the worker thread read the freed object; when looping it calls Start after the Flush
HLE_FORCE("_ZN5movie7Decoder5FlushEv", "影片解码器清空：输入输出都还回去", int, h_movie_Flush, void **self) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    MD_TRACE(d, 5, "Flush"); md_defer_drop(d); md_reset(d, 0); return 0;
}
HLE_FORCE("_ZN5movie7Decoder23AcquireInputBufferIndexEPiS1_", "影片解码器：取空输入号", int, h_movie_AcqIn, void **self, int *idx, int *left) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    if (!idx || !left) return 0x100b;
    // 停止后不再给输入号（同 MediaCodec）：银河战士 生存恐惧 的「有输入」处理（main+0x222f14）取到号就把解码器包装对象投进播放管理器的消息队列；
    // Stop 把输入号全还回来后，一个迟到的输入事件仍能取到号，包装对象随后析构，工作线程（main+0x223bd4）读到已释放的对象崩
    // No input indices after Stop (as with MediaCodec): Metroid Dread's "input available" handler (main+0x222f14) posts the decoder wrapper object to the player manager's message queue once it gets an index;
    // after Stop returned every input index, a late input event could still get one, the wrapper was destroyed afterwards, and the worker thread (main+0x223bd4) read the freed object and crashed
    pthread_mutex_lock(&d->mu);
    int r = MD_NO_INPUT; *idx = -1; *left = 0;
    if (d->ninfree && d->started) { *idx = d->infree[0]; memmove(d->infree, d->infree + 1, --d->ninfree * sizeof *d->infree); *left = d->ninfree; r = 0; }
    pthread_mutex_unlock(&d->mu);
    MD_TRACE(d, 6, "AcquireInputBufferIndex → %#x 号 %d 余 %d", r, *idx, *left);
    return r;
}
HLE_FORCE("_ZN5movie7Decoder14GetInputBufferEiPNS_6BufferE", "影片解码器：交出输入缓冲", int, h_movie_GetIn, void **self, int i, void *buf) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    if (!buf || i < 0 || i >= MD_NIN) return 0x100b;
    G.set_data(buf, d->in[i], MD_INCAP); G.set_range(buf, 0, (int)MD_INCAP);
    return 0;
}
HLE_FORCE("_ZN5movie7Decoder24SendInputBufferForDecodeEiiilj", "影片解码器：送一包解码（VP9 用 libvpx）", int, h_movie_SendIn,
          void **self, int i, int off, int size, int64_t pts, uint32_t flags) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    if (i < 0 || i >= MD_NIN || off < 0 || size < 0 || (uint32_t)off + (uint32_t)size > MD_INCAP) return 0x100b;
    pthread_mutex_lock(&d->mu);
    int fmt_now = 0;
    if (!d->w && size > 0 && vp9_size(d->in[i] + off, size, &d->w, &d->h)) LOG("[影片] VP9 关键帧：%d×%d", d->w, d->h);
    if (!d->fmt_sent) { d->fmt_sent = 1; fmt_now = 1; if (!d->w) { d->w = 1920; d->h = 1080; } }   // 尺寸从此定下（解码按它分配槽） | the size is fixed from here on (slots are allocated by it)
    d->pend[d->npend].in = i; d->pend[d->npend].off = off; d->pend[d->npend].size = size; d->pend[d->npend].pts = pts; d->pend[d->npend].flags = flags; d->npend++;
    int queued = d->npend;
    pthread_mutex_unlock(&d->mu);
    MD_TRACE(d, 7, "SendInputBufferForDecode 号 %d 偏移 %d 长 %d 时间 %lld µs 标志 %#x（队里 %d 包）", i, off, size, (long long)pts, flags, queued);
    if (fmt_now) md_event(d, 2);
    md_pump(d);
    return 0;
}

HLE_FORCE("_ZN5movie7Decoder24AcquireOutputBufferIndexEPiPlPjS1_", "影片解码器：取就绪输出号", int, h_movie_AcqOut,
          void **self, int *idx, int64_t *pts, uint32_t *flags, int *left) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    if (!idx || !pts || !flags || !left) return 0x100b;
    pthread_mutex_lock(&d->mu);
    int r = MD_NO_OUTPUT; *idx = -1; *left = 0;
    if (d->noutq && d->started) {
        int s = d->outq[0]; memmove(d->outq, d->outq + 1, --d->noutq * sizeof *d->outq);
        *idx = s; *pts = d->out[s].pts; *flags = d->out[s].flags; *left = d->noutq; r = 0;
    }
    pthread_mutex_unlock(&d->mu);
    if (!r) MD_TRACE(d, 8, "AcquireOutputBufferIndex → 号 %d 时间 %lld 标志 %#x 余 %d", *idx, (long long)*pts, *flags, *left);
    return r;
}
static void md_format(mdec_t *d, void *md) {
    int w, h, stride, slice; md_dims(d, &w, &h, &stride, &slice);
    G.set_i32(md, "width", w); G.set_i32(md, "height", h);
    G.set_rect(md, "crop", 0, 0, w - 1, h - 1);
    G.set_i32(md, "stride", stride); G.set_i32(md, "slice-height", slice);
    G.set_i32(md, "nv12-y-stride", stride); G.set_i32(md, "nv12-y-offset", 0); G.set_i32(md, "nv12-uv-offset", stride * slice);
}
HLE_FORCE("_ZN5movie7Decoder15GetOutputFormatEPNS_9MediaDataE", "影片解码器：输出格式（NV12）", int, h_movie_OutFmt, void **self, void *md) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    if (!md) return 0x100b;
    md_format(d, md); MD_TRACE(d, 9, "GetOutputFormat");
    return 0;
}
HLE_FORCE("_ZN5movie7Decoder21GetOutputBufferFormatEiPNS_9MediaDataE", "影片解码器：输出帧格式（NV12）", int, h_movie_BufFmt, void **self, int i, void *md) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    if (!md) return 0x100b;
    md_format(d, md); MD_TRACE(d, 10, "GetOutputBufferFormat 号 %d", i);
    return 0;
}
HLE_FORCE("_ZN5movie7Decoder19GetOutputBufferSizeEPiS1_", "影片解码器：输出缓冲大小（NV12 一帧）", int, h_movie_OutSize, void **self, int *size, int *mode) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    if (!size || !mode) return 0x100b;
    int w, h, stride, slice; md_dims(d, &w, &h, &stride, &slice);
    *size = stride * slice * 3 / 2; *mode = d->mode;   // SDK 第二个出参填解码器模式 | the SDK fills the second output with the decoder mode
    MD_TRACE(d, 11, "GetOutputBufferSize → %d（%d×%d）", *size, w, h);
    return 0;
}
// 把一帧写进客户端缓冲：解出的画面，没有画面（流结束包、AVC / VP8）时是黑帧（视频范围 Y 16、UV 128）
// Write one frame into the client buffer: the decoded picture, or a black frame (video range Y 16, UV 128) when there is none (end-of-stream packet, AVC / VP8)
// 原生贴图模式：客户端 Buffer 的数据指针就是 NVNtexture 对象本身（容量写 8；银河战士 生存恐惧 main+0x226c98：贴图池项 +0x18 处内嵌的贴图对象），外加 NVN 设备与同步对象（SetNvnDev / SetNvnSync）。
// SDK 里由 NnCodecConsumer 用 GPU 把解出的帧画进这张贴图、完成时发同步对象；宿主在 CPU 上写贴图存储（同 WriteTexels，回放层按脏页重传），写完同步对象即算完成
// Native texture mode: the client Buffer's data pointer is the NVNtexture object itself (capacity given as 8; Metroid Dread main+0x226c98: the texture object embedded at +0x18 of a texture pool entry), plus the NVN device and a sync object (SetNvnDev / SetNvnSync).
// In the SDK, NnCodecConsumer draws the decoded frame into that texture on the GPU and signals the sync when done; the host writes the texture storage on the CPU (as WriteTexels; the replay layer re-uploads dirty pages), and the sync counts as done once written
int nvn_host_write_texture(const void *nvntex, const void *src, int w, int h, size_t row, int *tw, int *th);
int nvn_host_signal_sync(void *nvnsync);
static int md_out_tex(mdec_t *d, int i, void *buf) {
    if (!G.nvn_sync) G.nvn_sync = msym("_ZNK5movie6Buffer10GetNvnSyncEv");
    uint8_t *p = G.data(buf); size_t cap = G.cap(buf);
    if (!p || cap < 8) { LOG("【红】影片：原生贴图模式的客户端缓冲 %zu 字节，要 8（贴图对象）", cap); return 0x100b; }
    void *tex = p; int tw = 0, th = 0, f = nvn_host_write_texture(tex, NULL, 0, 0, 0, &tw, &th);
    if (f < 0) { LOG("【红】影片：原生贴图模式的缓冲数据 %p 不是贴图对象", tex); return 0x100b; }
    if (f != 0x25 && f != 0x38 && f != 0x6d && f != 0x37 && !d->red_img++) LOG("【红】影片：客户端贴图格式 %#x 不是 RGBA8 / RGBX8（含 sRGB），按 RGBA 字节序写", f);   // 银河战士 生存恐惧：RGBX8_SRGB | Metroid Dread: RGBX8_SRGB
    int w = d->w < tw ? d->w : tw, h = d->h < th ? d->h : th;
    if (d->out[i].pic && d->out[i].frm) nvn_host_write_texture(tex, d->out[i].frm, w, h, (size_t)d->w * 4, NULL, NULL);
    else {   // 没有画面：黑帧 | no picture: a black frame
        size_t n = (size_t)tw * th; uint32_t *k = malloc(n * 4); if (!k) die("影片：黑帧分配失败");
        for (size_t j = 0; j < n; j++) k[j] = 0xff000000u;
        nvn_host_write_texture(tex, k, tw, th, (size_t)tw * 4, NULL, NULL); free(k);
    }
    void *sy = G.nvn_sync(buf); if (sy && nvn_host_signal_sync(sy) && !d->red_dec++) LOG("【红】影片：缓冲带的同步对象 %p 不是 NVNsync", sy);
    MD_TRACE(d, 12, "GetOutputBuffer 号 %d → 贴图 %p（%d×%d 格式 %#x）%s", i, tex, tw, th, f, d->out[i].pic ? "" : "（黑帧）");
    return 0;
}
HLE_FORCE("_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferE", "影片解码器：取一帧", int, h_movie_GetOut, void **self, int i, void *buf) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    if (!buf || i < 0 || i >= MD_NOUT) return 0x100b;
    if (d->mode == MD_MODE_TEX) return md_out_tex(d, i, buf);
    int w, h, stride, slice; md_dims(d, &w, &h, &stride, &slice);
    size_t need = (size_t)stride * slice * 3 / 2, cap = G.cap(buf);
    uint8_t *p = G.data(buf);
    if (!p || cap < need) { LOG("【红】影片：客户端输出缓冲 %zu 字节，一帧要 %zu", cap, need); G.set_range(buf, 0, 0); return 0x100b; }
    if (d->out[i].pic && d->out[i].frm) memcpy(p, d->out[i].frm, need);
    else { memset(p, 16, (size_t)stride * slice); memset(p + (size_t)stride * slice, 128, need - (size_t)stride * slice); }
    G.set_range(buf, 0, (int)need);
    MD_TRACE(d, 12, "GetOutputBuffer 号 %d → %zu 字节%s", i, need, d->out[i].pic ? "" : "（黑帧）");
    return 0;
}
HLE_FORCE("_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferEPNS_9MediaDataE", "影片解码器：取一帧连格式", int, h_movie_GetOutFmt, void **self, int i, void *buf, void *md) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    if (md) md_format(d, md);
    return h_movie_GetOut(self, i, buf);
}
HLE_FORCE("_ZN5movie7Decoder24ReleaseOutputBufferIndexEi", "影片解码器：还输出号（再解排队的包）", int, h_movie_RelOut, void **self, int i) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    if (i < 0 || i >= MD_NOUT) return 0x100b;
    pthread_mutex_lock(&d->mu);
    d->out[i].used = 0; d->out[i].pic = 0;
    int queued = d->npend;
    pthread_mutex_unlock(&d->mu);
    MD_TRACE(d, 13, "ReleaseOutputBufferIndex 号 %d（队里 %d 包）", i, queued);
    if (queued) md_pump(d);
    return 0;
}
HLE_FORCE("_ZN5movie7Decoder12GetLastErrorEv", "影片解码器：最近错误（无）", int, h_movie_LastErr, void **self) { (void)self; return 0; }
