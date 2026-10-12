// nn::audio AudioRenderer（audren 服务）——第一版：静音渲染器。
// 声部 / 混音 / 效果的编辑（AcquireVoiceSlot、SetVoice*、Add*Effect、InitializeAudioRendererConfig …）都只改游戏自己内存里的
// AudioRendererConfig，留在 SDK 原样跑；这里只接走服务的入口：工作区大小、开 / 起 / 停 / 关、每帧更新、输出设备查询。
// 渲染器事件由宿主线程按 5 ms（音频帧）触发，游戏的音频线程照常转；RequestUpdate 不把配置送去混音，所以没有声音。
// 下一版：解析 RequestUpdate 的输入 / 输出缓冲（声部状态、波形缓冲消耗），接 Core Audio 出声。
// nn::audio AudioRenderer (audren service) -- first version: a silent renderer.
// Editing voices / mixes / effects (AcquireVoiceSlot, SetVoice*, Add*Effect, InitializeAudioRendererConfig ...) only changes the AudioRendererConfig in the game's
// own memory and keeps running in the SDK; only the entry points that go to the service are hooked here: work buffer size, open / start / stop / close, the per-frame update, output device queries.
// The renderer event is fired by a host thread every 5 ms (one audio frame) so the game's audio thread keeps turning; RequestUpdate does not send the config to a mixer, so there is no sound.
// Next version: parse RequestUpdate's input / output buffers (voice states, wave buffer consumption) and output through Core Audio.
#include "hle.h"
#include <stdatomic.h>
#include <time.h>

enum { AUDIO_FRAME_NS = 5000000 };   // 渲染器每帧 5 ms（240 采样 @48 kHz） | 5 ms per renderer frame (240 samples @48 kHz)
static void *g_ren_event; static _Atomic int g_ren_running; static _Atomic uint64_t g_ren_frames;
static void *ren_thread(void *ev) {
    uint64_t next = now_ns() + AUDIO_FRAME_NS;
    for (;;) {
        uint64_t t = now_ns(); if (next > t) { struct timespec ts = {0, (long)(next - t)}; nanosleep(&ts, NULL); }
        if (atomic_load(&g_ren_running)) { atomic_fetch_add(&g_ren_frames, 1); hle_signal_system_event(ev); }
        next += AUDIO_FRAME_NS; if (now_ns() > next + 20 * AUDIO_FRAME_NS) next = now_ns() + AUDIO_FRAME_NS;
    }
    return NULL;
}

// 工作区（交给服务的 transfer memory）：宿主不用，给一个固定大小，游戏照它分配、再原样交回
// Work buffer (transfer memory handed to the service): unused by the host; report a fixed size, the game allocates it and hands it back unchanged
HLE_FORCE("_ZN2nn5audio30GetAudioRendererWorkBufferSizeERKNS0_22AudioRendererParameterE", "audren 工作区大小；宿主不用，给 64 KiB", size_t, h_ar_WorkSize, const void *param) { (void)param; return 0x10000; }
HLE_FORCE("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm", "开渲染器：静音版，事件按 5 ms 触发",
          Result, h_ar_Open, void **handle, void *ev, const void *param, void *work, size_t sz) {
    (void)param; (void)work; (void)sz;
    if (g_ren_event) { LOG("【红】OpenAudioRenderer 第二个渲染器：宿主只支持一个"); return RES_FAIL; }
    *handle = (void *)(uintptr_t)0xA0D10001u;   // 非零即有效 | non-zero means valid
    if (ev) { hle_bind_system_event(ev, 1); g_ren_event = ev; pthread_t th; if (pthread_create(&th, NULL, ren_thread, ev)) die("渲染器线程起不来"); pthread_detach(th); }
    LOG("audren：OpenAudioRenderer（静音版，事件 %p 每 5 ms）", ev);
    return RES_OK;
}
HLE_FORCE("_ZN2nn5audio18StartAudioRendererENS0_19AudioRendererHandleE", "渲染器开始", Result, h_ar_Start, void *h) { (void)h; atomic_store(&g_ren_running, 1); return RES_OK; }
HLE_FORCE("_ZN2nn5audio17StopAudioRendererENS0_19AudioRendererHandleE", "渲染器停止", void, h_ar_Stop, void *h) { (void)h; atomic_store(&g_ren_running, 0); }
HLE_FORCE("_ZN2nn5audio18CloseAudioRendererENS0_19AudioRendererHandleE", "渲染器关闭", void, h_ar_Close, void *h) { (void)h; atomic_store(&g_ren_running, 0); }
// 每帧更新：静音版只计数（配置不送混音器、输出状态不回写）
// Per-frame update: the silent version only counts (the config is not sent to a mixer, output status is not written back)
HLE_FORCE("_ZN2nn5audio26RequestUpdateAudioRendererENS0_19AudioRendererHandleEPKNS0_19AudioRendererConfigE", "渲染器更新：静音版只计数", Result, h_ar_Update, void *h, const void *cfg) {
    (void)h; (void)cfg; static _Atomic uint64_t n; if (!atomic_fetch_add(&n, 1)) LOG("audren：第一次 RequestUpdateAudioRenderer（静音，不混音）"); return RES_OK;
}

// 输出设备：一个内置扬声器，立体声
// Output device: one built-in speaker, stereo
enum { DEVNAME = 0x100 };
static void out_devname(char *out) { memset(out, 0, DEVNAME); strlcpy(out, "AudioBuiltInSpeakerOutput", DEVNAME); }
HLE_FORCE("_ZN2nn5audio25ListAudioOutputDeviceNameEPNS0_15AudioDeviceNameEi", "输出设备列表：内置扬声器 1 个", int, h_ad_List, char *names, int n) { if (n <= 0 || !names) return 0; out_devname(names); return 1; }
HLE_FORCE("_ZN2nn5audio30GetActiveAudioOutputDeviceNameEPNS0_15AudioDeviceNameE", "当前输出设备：内置扬声器", void, h_ad_Active, char *name) { out_devname(name); }
HLE_FORCE("_ZN2nn5audio41GetActiveAudioDeviceChannelCountForOutputEv", "输出声道数：2", int, h_ad_Channels, void) { return 2; }
HLE_FORCE("_ZN2nn5audio26SetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameEf", "设备音量；宿主忽略", void, h_ad_SetVol, const char *name, float v) { (void)name; (void)v; }
HLE_FORCE("_ZN2nn5audio39AcquireAudioDeviceNotificationForOutputEPNS_2os11SystemEventE", "输出设备变化通知：绑定、永不触发", void, h_ad_Notify, void *ev) { hle_bind_system_event(ev, 0); }
