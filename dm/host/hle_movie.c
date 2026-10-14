// SDK 的影片库（subsdk0 = NintendoSDK_movie，Engage）：Extractor 拆封装，Decoder 用 NVIDIA 的硬件解码器（NVOMX → NvRmChannelOpen 开 nvdrv 设备）。
// 宿主没有 nvdrv：解码线程开设备时 SDK 中止。先让开片失败——Extractor::SetDataSource 回 0x1000（SDK 自己在没有实现对象时回的同一个码），
// 播放器报错、游戏跳过影片，起机能往下走。Engage 的影片是 RomFS/Data/StreamingAssets/Movie 下的 103 个 WebM（2.7 GB）；真要放，得在宿主用 VideoToolbox 解。
// The SDK's movie library (subsdk0 = NintendoSDK_movie, Engage): Extractor demuxes, Decoder uses NVIDIA's hardware decoder (NVOMX → NvRmChannelOpen opens the nvdrv device).
// The host has no nvdrv: the SDK aborts when the decoder thread opens the device. For now opening a movie fails -- Extractor::SetDataSource returns 0x1000 (the same code the SDK returns when it has no implementation object),
// the player reports an error, the game skips the movie, and bring-up can go on. Engage's movies are 103 WebM files under RomFS/Data/StreamingAssets/Movie (2.7 GB); actually playing them needs a VideoToolbox decoder in the host.
#include "hle.h"

HLE_FORCE("_ZN5movie9Extractor13SetDataSourceEPKc", "影片拆封装：宿主还不放影片，开片失败（0x1000）", int, h_movie_SetSource, void *self, const char *path) {
    (void)self; static int red; if (!red++) LOG("【红】影片：宿主还不放影片，开片报失败（首个 %s）", path ? path : "(null)");
    return 0x1000;
}

// Decoder = { 虚表, DecoderImpl* }；它的每个方法在 impl 为空时都回 0x100c（SDK 自己的失败码），析构也跳过空 impl。
// 原版构造里 new 一个 DecoderImpl，起 NVOMX 调度线程开 NVIDIA 硬件解码器（宿主没有 nvdrv，中止或在它的错误路径上崩）。Unity 起机就建解码器（不先开片）。
// 接管：只放虚表、impl 留空——解码器「建好了但什么都做不了」，游戏拿到的是 SDK 原有的失败码
// Decoder = { vtable, DecoderImpl* }; each of its methods returns 0x100c (the SDK's own failure code) when impl is null, and the destructor skips a null impl.
// The original constructor news a DecoderImpl, which starts an NVOMX scheduler thread and opens NVIDIA's hardware decoder (the host has no nvdrv: it aborts, or crashes on the decoder's error path). Unity creates a decoder at startup (before opening any movie).
// Takeover: only the vtable is set and impl stays null -- the decoder is "constructed but can do nothing", and the game gets the SDK's existing failure code
HLE_FORCE("_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", "影片解码器：宿主不建硬件解码器，impl 留空（各方法回 0x100c）",
          void, h_movie_Decoder, void **self, int type, int fmt, int mode, int flags) {
    static void *vt; if (!vt) { uint8_t *t = guest_sym("_ZTVN5movie7DecoderE"); if (!t) die("影片：缺 movie::Decoder 的虚表"); vt = t + 0x10; }
    self[0] = vt; self[1] = NULL;
    static int red; if (!red++) LOG("【红】影片：宿主不建硬件解码器（类型 %d 输出 %d 模式 %d 标志 %#x），解码器各方法回失败", type, fmt, mode, flags);
}
