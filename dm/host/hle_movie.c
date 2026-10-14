// SDK 的影片库（subsdk0 = NintendoSDK_movie，Engage）：Extractor 拆封装，Decoder 用 NVIDIA 的硬件解码器（NVOMX → NvRmChannelOpen 开 nvdrv 设备）。
// 宿主没有 nvdrv：解码线程开设备时 SDK 中止。Extractor::SetDataSource 回 0x1000（SDK 自己在没有实现对象时回的同一个码）；Engage 的 Unity 自己拆 WebM，不走它。
// Decoder 由宿主接管（下面）：Engage 的影片是 RomFS/Data/StreamingAssets/Movie 下的 103 个 VP9 WebM（2.7 GB，无音轨），开场动画 S01F 不放完游戏不往下走。
// The SDK's movie library (subsdk0 = NintendoSDK_movie, Engage): Extractor demuxes, Decoder uses NVIDIA's hardware decoder (NVOMX → NvRmChannelOpen opens the nvdrv device).
// The host has no nvdrv: the SDK aborts when the decoder thread opens the device. Extractor::SetDataSource returns 0x1000 (the same code the SDK returns when it has no implementation object); Engage's Unity demuxes WebM itself and does not use it.
// The host takes over Decoder (below): Engage's movies are 103 VP9 WebM files under RomFS/Data/StreamingAssets/Movie (2.7 GB, no audio track), and the game does not go on until the opening movie S01F has played.
#include "hle.h"
#include <pthread.h>

HLE_FORCE("_ZN5movie9Extractor13SetDataSourceEPKc", "影片拆封装：宿主还不放影片，开片失败（0x1000）", int, h_movie_SetSource, void *self, const char *path) {
    (void)self; static int red; if (!red++) LOG("【红】影片：宿主不拆封装，开片报失败（首个 %s）", path ? path : "(null)");
    return 0x1000;
}

// ---------- 解码器 | Decoder ----------
// SDK 的 Decoder 是 Android MediaCodec 的薄封装（DecoderImpl 包 android::MediaCodec）：
//   · 客户端注册 4 个 nn::os 事件 DecoderEvents {输入有空、输出就绪、格式变了、出错}，有事就 SignalEvent；
//   · 输入：AcquireInputBufferIndex 取空输入号（没有回 0x1011）→ GetInputBuffer 把数据区交给客户端的 movie::Buffer → SendInputBufferForDecode(号, 偏移, 长, 时间 µs, 标志)；
//   · 输出：AcquireOutputBufferIndex(号, 时间, 标志, 余数)（没有回 0x100e）→ GetOutputBufferFormat / GetOutputBuffer 把一帧 NV12 拷进客户端自己分配的缓冲 → ReleaseOutputBufferIndex。
//   Unity（mode 1、输出格式 4 = NV12）按 GetOutputBufferSize 分配输出缓冲，从 MediaData 读 width / height / crop / nv12-y-stride / nv12-y-offset / nv12-uv-offset / slice-height。
// 宿主实现同一协议：每个输入包出一帧，时间与标志照抄（流结束标志随最后一帧传回）。输出槽都被客户端占着时，输入包先挂起、不还输入号（同 MediaCodec：解码器只在有空输出时消耗输入），
// 客户端还一个输出号就处理一个挂起的包。画面暂为黑帧（宿主还没有 VP9 解码）；尺寸取 Configure 的 MediaData，没有时取首个关键帧的 VP9 帧头。
// Unity 按宽 × 高 × 1.5 自己分配输出缓冲（不调 GetOutputBufferSize），所以行距 = 宽、平面高 = 高，不对齐。
// 对 Buffer / MediaData / 事件的操作都调 SDK 自己的函数（guest_sym），对象布局不用宿主知道。
// The SDK's Decoder is a thin wrapper over Android's MediaCodec (DecoderImpl wraps android::MediaCodec):
//   · the client registers 4 nn::os events DecoderEvents {input available, output ready, format changed, error}, signalled with SignalEvent;
//   · input: AcquireInputBufferIndex takes a free input index (none: 0x1011) → GetInputBuffer hands its data area to the client's movie::Buffer → SendInputBufferForDecode(index, offset, size, time µs, flags);
//   · output: AcquireOutputBufferIndex(index, time, flags, remaining) (none: 0x100e) → GetOutputBufferFormat / GetOutputBuffer copy one NV12 frame into a buffer the client allocated → ReleaseOutputBufferIndex.
//   Unity (mode 1, output format 4 = NV12) allocates its output buffers by GetOutputBufferSize and reads width / height / crop / nv12-y-stride / nv12-y-offset / nv12-uv-offset / slice-height from the MediaData.
// The host implements the same protocol: one frame per input packet, with its time and flags copied (the end-of-stream flag comes back with the last frame). While the client holds every output slot, input packets wait and their input indices are not returned (as with MediaCodec: a decoder only consumes input when it has a free output),
// and each output index the client returns lets one waiting packet through. The picture is black for now (the host has no VP9 decoder yet); the size comes from Configure's MediaData, or else from the VP9 header of the first key frame.
// Unity allocates its output buffers itself as width × height × 1.5 (it does not call GetOutputBufferSize), so the stride is the width and the plane height the height, unaligned.
// Buffer / MediaData / event operations call the SDK's own functions (guest_sym), so the host needs no knowledge of their layouts.

#define MD_NIN 4                 // 输入缓冲数 | input buffers
#define MD_INCAP (4u << 20)      // 每个输入缓冲 4 MB（1080p VP9 一包远小于此） | 4 MB per input buffer (a 1080p VP9 packet is far smaller)
#define MD_NOUT 8                // 输出槽数 | output slots
#define MD_NO_INPUT 0x1011       // SDK：没有空输入 | SDK: no free input
#define MD_NO_OUTPUT 0x100e      // SDK：没有就绪输出 | SDK: no output ready
#define MD_FMT_NV12 4

typedef struct {
    pthread_mutex_t mu;
    int type, fmt, mode;
    void **ev;                                   // DecoderEvents*：{输入, 输出, 格式, 出错} 的 EventType* | {input, output, format, error} EventType*
    int w, h, fmt_sent, started;
    uint8_t *in[MD_NIN]; int infree[MD_NIN], ninfree;                     // 空输入号队列 | queue of free input indices
    struct { int used; int64_t pts; uint32_t flags; } out[MD_NOUT];
    int outq[MD_NOUT], noutq;                                             // 就绪输出号队列 | queue of ready output indices
    struct { int in; int64_t pts; uint32_t flags; } pend[MD_NIN]; int npend;   // 等空输出槽的输入包 | input packets waiting for a free output slot
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
static void md_event(mdec_t *d, int i) { if (d->ev && d->ev[i]) G.signal(d->ev[i]); }   // 0 输入 1 输出 2 格式 3 出错 | 0 input 1 output 2 format 3 error
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

HLE_FORCE("_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", "影片解码器：宿主实现（不建 NVIDIA 硬件解码器）",
          void, h_movie_Decoder, void **self, int type, int fmt, int mode, int prio) {
    static void *vt; if (!vt) { uint8_t *t = guest_sym("_ZTVN5movie7DecoderE"); if (!t) die("影片：缺 movie::Decoder 的虚表"); vt = t + 0x10; }
    md_syms();
    self[0] = vt; self[1] = NULL;
    // 只做视频（AVC / VP8 / VP9 = 1..3）NV12 输出；别的（音频）impl 留空，各方法回 SDK 的失败码
    // Only video (AVC / VP8 / VP9 = 1..3) with NV12 output; anything else (audio) keeps a null impl and every method returns the SDK's failure code
    if (type < 1 || type > 3 || fmt != MD_FMT_NV12) { LOG("【红】影片：解码器类型 %d 输出 %d 模式 %d 宿主不支持，各方法回失败", type, fmt, mode); return; }
    mdec_t *d = calloc(1, sizeof *d); pthread_mutex_init(&d->mu, NULL);
    d->type = type; d->fmt = fmt; d->mode = mode;
    for (int i = 0; i < MD_NIN; i++) { d->in[i] = malloc(MD_INCAP); d->infree[i] = i; }
    d->ninfree = MD_NIN;
    self[1] = d;
    LOG("[影片] 解码器 %p：类型 %d 输出 %d 模式 %d 优先级 %d（宿主黑帧解码）", (void *)self, type, fmt, mode, prio);
}
static void md_free(void **self) {
    mdec_t *d = md_of(self); if (!d) return;
    self[1] = NULL;
    LOG("[影片] 解码器 %p 析构：送入 %lu 包，取出 %lu 帧", (void *)self, d->ncall[7], d->ncall[8]);
    for (int i = 0; i < MD_NIN; i++) free(d->in[i]);
    pthread_mutex_destroy(&d->mu); free(d);
}
HLE_FORCE("_ZN5movie7DecoderD2Ev", "影片解码器析构：释放宿主对象", void, h_movie_DecD2, void **self) { md_free(self); }
// 删除式析构：Unity 用自己的分配器建对象、先调 D2 再自己释放，这里不释放对象本身 | deleting destructor: Unity allocates the object with its own allocator, calls D2 and frees it itself; the object itself is not freed here
HLE_FORCE("_ZN5movie7DecoderD0Ev", "影片解码器删除式析构：释放宿主对象", void, h_movie_DecD0, void **self) { LOG("【红】影片：删除式析构（对象本身未释放）"); md_free(self); }

HLE_FORCE("_ZN5movie7Decoder9ConfigureEPNS_9MediaDataE", "影片解码器配置", int, h_movie_Configure, void **self, void *md) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    int w = 0, h = 0; if (md) { G.find_i32(md, "width", &w); G.find_i32(md, "height", &h); }
    pthread_mutex_lock(&d->mu); if (w > 0 && h > 0) { d->w = w; d->h = h; } pthread_mutex_unlock(&d->mu);
    MD_TRACE(d, 0, "Configure：MediaData 宽 %d 高 %d", w, h);
    return 0;
}
HLE_FORCE("_ZN5movie7Decoder20RegisterClientEventsEPNS_13DecoderEventsE", "影片解码器：登记客户端事件", int, h_movie_RegEv, void **self, void **ev) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    d->ev = ev; MD_TRACE(d, 1, "RegisterClientEvents：%p %p %p %p", ev ? ev[0] : 0, ev ? ev[1] : 0, ev ? ev[2] : 0, ev ? ev[3] : 0);
    return 0;
}
HLE_FORCE("_ZN5movie7Decoder5StartEv", "影片解码器开始：输入全空，通知客户端", int, h_movie_Start, void **self) {
    mdec_t *d = md_of(self); if (!d) return 0x2002;
    pthread_mutex_lock(&d->mu); d->started = 1; pthread_mutex_unlock(&d->mu);
    MD_TRACE(d, 2, "Start");
    md_event(d, 0);
    return 0;
}
static void md_reset(mdec_t *d) {
    pthread_mutex_lock(&d->mu);
    for (int i = 0; i < MD_NIN; i++) d->infree[i] = i;
    d->ninfree = MD_NIN; d->noutq = 0; d->npend = 0;
    for (int i = 0; i < MD_NOUT; i++) d->out[i].used = 0;
    pthread_mutex_unlock(&d->mu);
}
HLE_FORCE("_ZN5movie7Decoder4StopEv", "影片解码器停止", int, h_movie_Stop, void **self) {
    mdec_t *d = md_of(self); if (!d) return 0x2003;
    MD_TRACE(d, 3, "Stop"); md_reset(d); d->started = 0; return 0;
}
HLE_FORCE("_ZN5movie7Decoder5ResetEv", "影片解码器复位", int, h_movie_Reset, void **self) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    MD_TRACE(d, 4, "Reset"); md_reset(d); d->fmt_sent = 0; return 0;
}
HLE_FORCE("_ZN5movie7Decoder5FlushEv", "影片解码器清空：输入输出都还回去", int, h_movie_Flush, void **self) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    MD_TRACE(d, 5, "Flush"); md_reset(d); md_event(d, 0); return 0;
}
HLE_FORCE("_ZN5movie7Decoder23AcquireInputBufferIndexEPiS1_", "影片解码器：取空输入号", int, h_movie_AcqIn, void **self, int *idx, int *left) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    if (!idx || !left) return 0x100b;
    pthread_mutex_lock(&d->mu);
    int r = MD_NO_INPUT; *idx = -1; *left = 0;
    if (d->ninfree) { *idx = d->infree[0]; memmove(d->infree, d->infree + 1, --d->ninfree * sizeof *d->infree); *left = d->ninfree; r = 0; }
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
// 一包占一个空输出槽、输入号还回去；返回输出槽号，没有空槽回 −1（调用方持锁） | one packet takes a free output slot and its input index is returned; returns the slot, or -1 when none is free (caller holds the lock)
static int md_emit(mdec_t *d, int in, int64_t pts, uint32_t flags) {
    int slot = -1;
    for (int k = 0; k < MD_NOUT; k++) if (!d->out[k].used) { slot = k; break; }
    if (slot < 0) return -1;
    d->out[slot].used = 1; d->out[slot].pts = pts; d->out[slot].flags = flags; d->outq[d->noutq++] = slot;
    d->infree[d->ninfree++] = in;
    return slot;
}
HLE_FORCE("_ZN5movie7Decoder24SendInputBufferForDecodeEiiilj", "影片解码器：送一包解码（宿主出黑帧）", int, h_movie_SendIn,
          void **self, int i, int off, int size, int64_t pts, uint32_t flags) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    if (i < 0 || i >= MD_NIN || off < 0 || size < 0 || (uint32_t)off + (uint32_t)size > MD_INCAP) return 0x100b;
    pthread_mutex_lock(&d->mu);
    int fmt_now = 0, slot = -1;
    if (!d->w && size > 0 && vp9_size(d->in[i] + off, size, &d->w, &d->h)) LOG("[影片] VP9 关键帧：%d×%d", d->w, d->h);
    if (!d->fmt_sent) { d->fmt_sent = 1; fmt_now = 1; }
    if (!d->npend) slot = md_emit(d, i, pts, flags);
    if (slot < 0) { d->pend[d->npend].in = i; d->pend[d->npend].pts = pts; d->pend[d->npend].flags = flags; d->npend++; }
    pthread_mutex_unlock(&d->mu);
    MD_TRACE(d, 7, "SendInputBufferForDecode 号 %d 偏移 %d 长 %d 时间 %lld µs 标志 %#x → 输出槽 %d", i, off, size, (long long)pts, flags, slot);
    if (fmt_now) md_event(d, 2);
    if (slot >= 0) { md_event(d, 0); md_event(d, 1); }
    return 0;
}
HLE_FORCE("_ZN5movie7Decoder24AcquireOutputBufferIndexEPiPlPjS1_", "影片解码器：取就绪输出号", int, h_movie_AcqOut,
          void **self, int *idx, int64_t *pts, uint32_t *flags, int *left) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    if (!idx || !pts || !flags || !left) return 0x100b;
    pthread_mutex_lock(&d->mu);
    int r = MD_NO_OUTPUT; *idx = -1; *left = 0;
    if (d->noutq) {
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
// 把一帧写进客户端缓冲：现在是黑帧（视频范围 Y 16、UV 128） | write one frame into the client buffer: a black frame for now (video range Y 16, UV 128)
HLE_FORCE("_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferE", "影片解码器：取一帧（宿主黑帧）", int, h_movie_GetOut, void **self, int i, void *buf) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    if (!buf || i < 0 || i >= MD_NOUT) return 0x100b;
    int w, h, stride, slice; md_dims(d, &w, &h, &stride, &slice);
    size_t need = (size_t)stride * slice * 3 / 2, cap = G.cap(buf);
    uint8_t *p = G.data(buf);
    if (!p || cap < need) { LOG("【红】影片：客户端输出缓冲 %zu 字节，一帧要 %zu", cap, need); G.set_range(buf, 0, 0); return 0x100b; }
    memset(p, 16, (size_t)stride * slice); memset(p + (size_t)stride * slice, 128, need - (size_t)stride * slice);
    G.set_range(buf, 0, (int)need);
    MD_TRACE(d, 12, "GetOutputBuffer 号 %d → %zu 字节", i, need);
    return 0;
}
HLE_FORCE("_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferEPNS_9MediaDataE", "影片解码器：取一帧连格式", int, h_movie_GetOutFmt, void **self, int i, void *buf, void *md) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    if (md) md_format(d, md);
    return h_movie_GetOut(self, i, buf);
}
HLE_FORCE("_ZN5movie7Decoder24ReleaseOutputBufferIndexEi", "影片解码器：还输出号", int, h_movie_RelOut, void **self, int i) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    if (i < 0 || i >= MD_NOUT) return 0x100b;
    pthread_mutex_lock(&d->mu);
    d->out[i].used = 0;
    int slot = -1;
    if (d->npend && (slot = md_emit(d, d->pend[0].in, d->pend[0].pts, d->pend[0].flags)) >= 0) memmove(d->pend, d->pend + 1, --d->npend * sizeof *d->pend);
    pthread_mutex_unlock(&d->mu);
    MD_TRACE(d, 13, "ReleaseOutputBufferIndex 号 %d（挂起包进输出槽 %d）", i, slot);
    if (slot >= 0) { md_event(d, 0); md_event(d, 1); }
    return 0;
}
HLE_FORCE("_ZN5movie7Decoder12GetLastErrorEv", "影片解码器：最近错误（无）", int, h_movie_LastErr, void **self) { (void)self; return 0; }
