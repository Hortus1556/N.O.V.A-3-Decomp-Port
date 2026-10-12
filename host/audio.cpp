#include "audio.h"
#include "tls.h"

#include "jni.h"
#include "os.h"
#include "runtime.h"

#include "sdl.h"

#ifdef __SWITCH__
#include <malloc.h>
#include <switch.h>
#endif
#ifdef __3DS__
#include <3ds.h>
#endif
#ifdef __GAMECUBE__
#include <gccore.h>
#include <malloc.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace audio {
namespace {

// android.media.AudioTrack constants
constexpr int kChannelOutMono = 4, kChannelOutStereo = 12;          // CHANNEL_OUT_*
constexpr int kChannelConfMono = 2, kChannelConfStereo = 3;         // deprecated CHANNEL_CONFIGURATION_*
constexpr int kEncodingPcm16 = 2, kEncodingPcm8 = 3;                // AudioFormat.ENCODING_PCM_*
constexpr int kStateStopped = 1, kStatePaused = 2, kStatePlaying = 3;  // PLAYSTATE_*

// Output starvation, counted where the device asks for data (the SDL stream's get callback, the
// Switch feeder) once the track has been fed; logged from write().
std::atomic<int> g_underruns{0};
std::atomic<int64_t> g_underrun_bytes{0};  // in the track's format
std::atomic<bool> g_fed{false};            // data was queued since play/stop: starvation counts

// The output: an SDL3 audio stream bound to the default device on the PC. On the Switch,
// libnx's AudioOut (48 kHz stereo): a feeder thread keeps three 21 ms buffers queued and mixes
// the open tracks into each released one (converted to 48 kHz stereo on the way in, the volume
// applied in the mix); silence when nothing is queued, so the device never stops. (SDL2's
// Switch audio thread stalled now and then: the queue stopped draining for good.) The 3DS does
// the same on one ndsp channel at the DSP's own rate (32728 Hz, which Vox then mixes at, so
// nothing is resampled), three 16 ms buffers, refilled when the DSP signals a frame.
#if defined(__SWITCH__) || defined(__3DS__) || defined(__GAMECUBE__)
#ifdef __SWITCH__
constexpr int kOutRate = 48000, kOutFrames = 1024, kOutBuffers = 3;
#elif defined(__GAMECUBE__)
constexpr int kOutRate = 32000, kOutFrames = 512, kOutBuffers = 3;  // 32 kHz: a third less mixing than 48
#else
constexpr int kOutRate = 32728, kOutFrames = 512, kOutBuffers = 3;
#endif
constexpr size_t kRingSamples = size_t(kOutRate) * 2;  // 1 s of 48 kHz stereo: far above any queue limit
struct Output {
    int rate = kOutRate, channels = 2;
    bool pcm8 = false, paused = true;
    float gain = 1;
    std::vector<int16_t> ring = std::vector<int16_t>(kRingSamples);  // 48 kHz stereo, interleaved
    size_t head = 0, size = 0;  // read position and fill, in samples
    double step = 1, pos = 0;   // resampling: input frames per output frame, position in the input
    int16_t last[2] = {};       // the previous input frame (pos runs from it)
    void push(int16_t l, int16_t r) {
        if (size + 2 > ring.size()) return;  // cannot happen below the queue limit
        ring[(head + size) % ring.size()] = l;
        ring[(head + size + 1) % ring.size()] = r;
        size += 2;
    }
};
std::mutex g_out_lock;
std::vector<Output *> g_outputs;

// Mixes the open tracks' queued sound into `out` (kOutFrames stereo frames): silence where
// nothing is queued, counted as an underrun once the track was fed.
void mix_into(int16_t *out) {
    static std::vector<int32_t> mix(kOutFrames * 2);
    std::fill(mix.begin(), mix.end(), 0);
    {
        std::lock_guard<std::mutex> lock(g_out_lock);
        for (Output *o : g_outputs) {
            if (o->paused) continue;
            const size_t n = std::min(mix.size(), o->size), cap = o->ring.size();
            for (size_t i = 0; i < n; i++) mix[i] += int32_t(float(o->ring[(o->head + i) % cap]) * o->gain);
            o->head = (o->head + n) % cap;
            o->size -= n;
            if (n < mix.size() && g_fed.load(std::memory_order_relaxed)) {
                g_underruns.fetch_add(1, std::memory_order_relaxed);
                const int64_t frames = int64_t(mix.size() - n) / 2 * o->rate / kOutRate;
                g_underrun_bytes.fetch_add(frames * o->channels * (o->pcm8 ? 1 : 2), std::memory_order_relaxed);
            }
        }
    }
    for (size_t i = 0; i < mix.size(); i++) out[i] = int16_t(std::clamp<int32_t>(mix[i], -32768, 32767));
}

#ifdef __SWITCH__
void feeder(void *) {
    static AudioOutBuffer bufs[kOutBuffers];
    const size_t size = (kOutFrames * 4 + 0xfff) & ~size_t(0xfff);
    for (auto &b : bufs) {
        b.next = nullptr;
        b.buffer = memalign(0x1000, size);
        std::memset(b.buffer, 0, size);
        b.buffer_size = size;
        b.data_size = kOutFrames * 4;
        b.data_offset = 0;
        audoutAppendAudioOutBuffer(&b);
    }
    for (;;) {
        AudioOutBuffer *done = nullptr;
        u32 count = 0;
        if (R_FAILED(audoutWaitPlayFinish(&done, &count, UINT64_MAX)) || !done) {
            svcSleepThread(5'000'000);  // never spin: this thread is above the game's on its core
            continue;
        }
        mix_into(static_cast<int16_t *>(done->buffer));
        armDCacheFlush(done->buffer, done->buffer_size);
        audoutAppendAudioOutBuffer(done);
    }
}

bool start_output() {
    if (R_FAILED(audoutInitialize()) || R_FAILED(audoutStartAudioOut())) {
        rt::log("audio: AudioOut unavailable");
        return false;
    }
    static Thread thread;
    // core 0, above every guest thread there (it only waits on the device)
    if (R_FAILED(threadCreate(&thread, feeder, nullptr, nullptr, 0x8000, 0x2a, 0)) || R_FAILED(threadStart(&thread)))
        return false;
    rt::log("audio: AudioOut %u Hz, %u channel(s)", audoutGetSampleRate(), audoutGetChannelCount());
    return true;
}
#elif defined(__GAMECUBE__)
// The GameCube: the audio interface plays 16-bit stereo (big-endian: the host's order) by DMA from
// MEM1, three 16 ms buffers in turn. Its interrupt comes as a buffer starts: it queues the next
// one (mixed earlier) and wakes the feeder, which mixes the one after (mix_into takes a mutex, so
// not in the interrupt).
int16_t *g_bufs[kOutBuffers];
std::atomic<int> g_to_fill{-1};
lwpq_t g_wake = LWP_TQUEUE_NULL;
int g_playing = 0;
std::atomic<uint32_t> g_feeder_wakes{0};

void dma_started() {
    g_playing = (g_playing + 1) % kOutBuffers;
    AUDIO_InitDMA(u32(g_bufs[g_playing]), kOutFrames * 4);
    g_to_fill = (g_playing + 1) % kOutBuffers;
    LWP_ThreadSignal(g_wake);
}

void gc_feeder(void *) {
    for (;;) {
        LWP_ThreadSleep(g_wake);
        g_feeder_wakes.fetch_add(1, std::memory_order_relaxed);
        const int i = g_to_fill.exchange(-1);
        if (i < 0) continue;
        mix_into(g_bufs[i]);
        DCFlushRange(g_bufs[i], kOutFrames * 4);
    }
}

bool start_output() {
    for (auto &b : g_bufs) {
        b = static_cast<int16_t *>(memalign(32, kOutFrames * 4));
        std::memset(b, 0, kOutFrames * 4);
        DCFlushRange(b, kOutFrames * 4);
    }
    LWP_InitQueue(&g_wake);
    rt::os::start_background(gc_feeder, nullptr);
    AUDIO_Init(nullptr);
    AUDIO_SetDSPSampleRate(AI_SAMPLERATE_32KHZ);
    AUDIO_RegisterDMACallback(dma_started);
    AUDIO_InitDMA(u32(g_bufs[0]), kOutFrames * 4);
    AUDIO_StartDMA();
    rt::log("audio: AI DMA, %d Hz stereo, %d buffers of %d frames", kOutRate, kOutBuffers, kOutFrames);
    return true;
}
}  // namespace

void stop_output() { AUDIO_StopDMA(); }
uint32_t feeder_wakes() { return g_feeder_wakes.load(std::memory_order_relaxed); }
namespace {
#else
// The 3DS has two ways out: ndsp, which needs the DSP firmware (sdmc:/3ds/dspfirm.cdc;
// host/3ds/dspfirm.cpp extracts it from the HOME Menu at the first start), else CSND, the
// older sound service, which needs none (the firmware could not be had; NOVA3_AUDIO=csnd in a
// test script's env takes it anyway). Both play the same mix at the same rate.
LightEvent g_dsp_frame;  // signalled by ndsp after each DSP frame (about every 5 ms)
std::atomic<bool> g_ndsp{false};  // started: stop_output ends it
std::atomic<uint32_t> g_feeder_wakes{0};  // (hang reports: about 200 a second)

void ndsp_feeder(void *) {
    static ndspWaveBuf bufs[kOutBuffers];
    for (auto &b : bufs) {
        b = ndspWaveBuf{};
        b.data_pcm16 = static_cast<int16_t *>(linearAlloc(kOutFrames * 4));
        b.nsamples = kOutFrames;
        std::memset(b.data_pcm16, 0, kOutFrames * 4);
        DSP_FlushDataCache(b.data_pcm16, kOutFrames * 4);
        ndspChnWaveBufAdd(0, &b);
    }
    for (;;) {
        LightEvent_Wait(&g_dsp_frame);
        g_feeder_wakes.fetch_add(1, std::memory_order_relaxed);
        for (auto &b : bufs) {
            if (b.status != NDSP_WBUF_DONE) continue;
            mix_into(b.data_pcm16);
            DSP_FlushDataCache(b.data_pcm16, kOutFrames * 4);
            ndspChnWaveBufAdd(0, &b);
        }
    }
}

bool start_ndsp() {
    if (R_FAILED(ndspInit())) return false;
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_NONE);  // already at the DSP's rate
    ndspChnSetRate(0, float(kOutRate));
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    float mix[12] = {1, 1};  // front left and right at full volume
    ndspChnSetMix(0, mix);
    LightEvent_Init(&g_dsp_frame, RESET_ONESHOT);
    ndspSetCallback([](void *) { LightEvent_Signal(&g_dsp_frame); }, nullptr);
    // the main core, above the guest threads (it only waits on the DSP)
    if (!threadCreate(ndsp_feeder, nullptr, 0x4000, 0x18, 0, true)) return false;
    g_ndsp = true;
    rt::log("audio: ndsp %d Hz stereo", kOutRate);
    return true;
}

// CSND: two hardware channels, left and right (CSND plays mono), each looping over a ring of
// kCsndChunks chunks of kOutFrames samples in linear memory. Nothing tells when a chunk has
// played, so the feeder follows the play position by the system clock: the sound timer at this
// rate counts 2048 ticks of 67 MHz a sample (CSND_TIMER(32728): 32728.5 Hz, the DSP's rate),
// and the ARM11's tick runs off the same clock at four times that, so 8192 ticks are a sample
// and the two never drift apart. Every 4 ms it mixes the chunks that have become free: up to
// three chunks (47 ms, as ndsp's three buffers) ahead of the chunk playing, less a margin for
// the channels' start. Suspended (HOME, sleep), the channels stop and the ring starts again on
// return; they are stopped before the process ends (stop_output), or they would loop on.
constexpr int kCsndChunks = 4;
constexpr int64_t kTicksPerSample = 8192, kStartMargin = 128;  // samples
struct Csnd {
    std::mutex lock;
    int16_t *ring[2] = {};  // left, right
    u32 chn[2] = {};
    u64 t0 = 0;             // the system tick at the channels' start
    int64_t written = 0;    // chunks mixed since then
    bool playing = false;
    uint64_t late = 0;      // chunks the feeder was too late for (the channel played the old ones)
} g_csnd;

void csnd_start() {  // g_csnd.lock held
    for (int c = 0; c < 2; c++) {
        std::memset(g_csnd.ring[c], 0, kCsndChunks * kOutFrames * 2);
        GSPGPU_FlushDataCache(g_csnd.ring[c], kCsndChunks * kOutFrames * 2);
    }
    const u32 timer = CSND_TIMER(kOutRate);
    for (int c = 0; c < 2; c++) {
        const u32 flags = SOUND_CHANNEL(g_csnd.chn[c]) | SOUND_FORMAT_16BIT | SOUND_REPEAT | SOUND_ENABLE | timer << 16;
        const u32 vol = CSND_VOL(1.0f, c ? 1.0f : -1.0f), at = osConvertVirtToPhys(g_csnd.ring[c]);
        CSND_SetChnRegs(flags, at, at, kCsndChunks * kOutFrames * 2, vol, vol);
    }
    csndExecCmds(true);  // (both in one batch: they start together)
    g_csnd.t0 = svcGetSystemTick();
    g_csnd.written = 1;  // chunk 0 is playing (silence)
    g_csnd.playing = true;
}
void csnd_stop() {  // g_csnd.lock held
    if (!g_csnd.playing) return;
    for (u32 c : g_csnd.chn) CSND_SetPlayState(c, 0);
    csndExecCmds(true);
    g_csnd.playing = false;
}

void csnd_feeder(void *) {
    static std::vector<int16_t> mix(kOutFrames * 2);
    for (;;) {
        svcSleepThread(4'000'000);
        std::lock_guard<std::mutex> lock(g_csnd.lock);
        if (!g_csnd.playing) continue;
        const int64_t played = int64_t(svcGetSystemTick() - g_csnd.t0) / kTicksPerSample - kStartMargin;
        const int64_t playing = played < 0 ? 0 : played / kOutFrames;  // the chunk playing (or about to)
        if (g_csnd.written <= playing) {  // the channels are past what was mixed: catch up
            static uint64_t logged;
            g_csnd.late += uint64_t(playing + 1 - g_csnd.written);
            g_csnd.written = playing + 1;
            if (g_csnd.late <= 5 || g_csnd.late / 100 != logged / 100)  // the first five, then every 100th
                rt::log("audio: CSND feeder late (%llu chunks in all)", (unsigned long long)(logged = g_csnd.late));
        }
        bool wrote = false;
        for (; g_csnd.written < playing + kCsndChunks; g_csnd.written++, wrote = true) {
            mix_into(mix.data());
            const int64_t at = g_csnd.written % kCsndChunks * kOutFrames;
            for (int i = 0; i < kOutFrames; i++) g_csnd.ring[0][at + i] = mix[2 * i], g_csnd.ring[1][at + i] = mix[2 * i + 1];
        }
        if (wrote)
            for (int c = 0; c < 2; c++) GSPGPU_FlushDataCache(g_csnd.ring[c], kCsndChunks * kOutFrames * 2);
    }
}

bool start_csnd() {
    if (Result rc = csndInit(); R_FAILED(rc)) {
        rt::log("audio: CSND unavailable either (%08lx): no sound", (unsigned long)rc);
        return false;
    }
    int n = 0;
    for (u32 c = 0; c < CSND_NUM_CHANNELS && n < 2; c++)
        if (csndChannels & BIT(c)) g_csnd.chn[n++] = c;
    for (auto &r : g_csnd.ring) r = static_cast<int16_t *>(linearAlloc(kCsndChunks * kOutFrames * 2));
    if (n < 2 || !g_csnd.ring[0] || !g_csnd.ring[1]) {
        rt::log("audio: CSND has no two channels or no linear memory: no sound");
        csndExit();
        return false;
    }
    static aptHookCookie hook;
    aptHook(&hook, [](APT_HookType type, void *) {
        std::lock_guard<std::mutex> lock(g_csnd.lock);
        if (type == APTHOOK_ONSUSPEND || type == APTHOOK_ONSLEEP || type == APTHOOK_ONEXIT) csnd_stop();
        else if ((type == APTHOOK_ONRESTORE || type == APTHOOK_ONWAKEUP) && !g_csnd.playing) csnd_start();
    }, nullptr);
    {
        std::lock_guard<std::mutex> lock(g_csnd.lock);
        csnd_start();
    }
    if (!threadCreate(csnd_feeder, nullptr, 0x4000, 0x18, 0, true)) return false;
    rt::log("audio: CSND %d Hz stereo (channels %lu and %lu)", kOutRate, (unsigned long)g_csnd.chn[0], (unsigned long)g_csnd.chn[1]);
    return true;
}

}  // namespace

void stop_output() {
    // the DSP stopped and its program unloaded (it would play on, and the HOME Menu's sound needs it)
    if (g_ndsp.exchange(false)) ndspExit();
    std::unique_lock<std::mutex> lock(g_csnd.lock, std::try_to_lock);  // (a fatal error may come from inside)
    if (lock.owns_lock()) csnd_stop();
}

uint32_t feeder_wakes() { return g_feeder_wakes.load(std::memory_order_relaxed); }

namespace {
bool start_output() {
    const char *force = std::getenv("NOVA3_AUDIO");
    const bool csnd = force && !std::strcmp(force, "csnd");
    if (!csnd && start_ndsp()) return true;
    rt::log(csnd ? "audio: CSND chosen (NOVA3_AUDIO)" : "audio: ndsp unavailable (no DSP firmware, sdmc:/3ds/dspfirm.cdc): CSND instead");
    return start_csnd();
}
#endif

Output *out_open(int rate, int channels, bool pcm8) {
    static const bool started = start_output();
    if (!started) return nullptr;
    auto *o = new Output;
    o->rate = rate;
    o->channels = channels;
    o->pcm8 = pcm8;
    o->step = double(rate) / kOutRate;
    std::lock_guard<std::mutex> lock(g_out_lock);
    g_outputs.push_back(o);
    return o;
}
int out_queued(Output *o) {
    std::lock_guard<std::mutex> lock(g_out_lock);
    const int64_t frames = int64_t(o->size / 2) * o->rate / kOutRate;
    return int(frames * o->channels * (o->pcm8 ? 1 : 2));
}
void out_put(Output *o, const uint8_t *data, int bytes) {
    const int sample_bytes = o->pcm8 ? 1 : 2, frame_bytes = sample_bytes * o->channels;
    const int frames = bytes / frame_bytes;
    auto sample = [&](int frame, int ch) -> int16_t {
        const uint8_t *p = data + frame * frame_bytes + std::min(ch, o->channels - 1) * sample_bytes;
        if (o->pcm8) return int16_t((int(*p) - 128) << 8);
        uint16_t v;
        std::memcpy(&v, p, 2);
        return int16_t(LE16(v));  // the game's PCM: little-endian
    };
    std::lock_guard<std::mutex> lock(g_out_lock);
    if (o->rate == kOutRate) {  // Vox's case: it mixes at getNativeOutputSampleRate (48 kHz)
        for (int f = 0; f < frames; f++) o->push(sample(f, 0), sample(f, 1));
        return;
    }
    // linear interpolation between the previous input frame and the next
    for (; o->pos < frames; o->pos += o->step) {
        const int i = int(o->pos);
        const double t = o->pos - i;
        int16_t v[2];
        for (int ch = 0; ch < 2; ch++) {
            const int16_t a = i == 0 ? o->last[ch] : sample(i - 1, ch), b = sample(i, ch);
            v[ch] = int16_t(a + (b - a) * t);
        }
        o->push(v[0], v[1]);
    }
    o->pos -= frames;
    if (frames > 0) o->last[0] = sample(frames - 1, 0), o->last[1] = sample(frames - 1, 1);
}
void out_pause(Output *o) {
    std::lock_guard<std::mutex> lock(g_out_lock);
    o->paused = true;
}
void out_resume(Output *o) {
    std::lock_guard<std::mutex> lock(g_out_lock);
    o->paused = false;
}
void out_clear(Output *o) {
    std::lock_guard<std::mutex> lock(g_out_lock);
    o->head = o->size = 0;
}
void out_close(Output *o) {
    {
        std::lock_guard<std::mutex> lock(g_out_lock);
        g_outputs.erase(std::remove(g_outputs.begin(), g_outputs.end(), o), g_outputs.end());
    }
    delete o;
}
void out_gain(Output *o, float gain) {
    std::lock_guard<std::mutex> lock(g_out_lock);
    o->gain = gain;
}
int native_rate() { return kOutRate; }
#ifdef __3DS__
// 128 ms plus the DSP's three 512-frame buffers (47 ms) was heard as late sound on a console; Vox's
// mixer runs above the guest threads on its core, so 48-80 ms queued is slack enough
constexpr int kQueueMs = 80;
#else
constexpr int kQueueMs = 128;  // more headroom: guest threads share cores without time slices
#endif
#else
using Output = SDL_AudioStream;
// SDL's device thread asks the stream for data: `additional` is what the queue lacks.
void SDLCALL on_get(void *, SDL_AudioStream *, int additional, int) {
    if (additional > 0 && g_fed.load(std::memory_order_relaxed)) {
        g_underruns.fetch_add(1, std::memory_order_relaxed);
        g_underrun_bytes.fetch_add(additional, std::memory_order_relaxed);
    }
}
Output *out_open(int rate, int channels, bool pcm8) {
    SDL_AudioSpec spec{pcm8 ? SDL_AUDIO_U8 : SDL_AUDIO_S16LE, channels, rate};
    return SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, on_get, nullptr);
}
int out_queued(Output *o) { return SDL_GetAudioStreamQueued(o); }
void out_put(Output *o, const uint8_t *data, int bytes) { SDL_PutAudioStreamData(o, data, bytes); }
void out_pause(Output *o) { SDL_PauseAudioStreamDevice(o); }
void out_resume(Output *o) { SDL_ResumeAudioStreamDevice(o); }
void out_clear(Output *o) { SDL_ClearAudioStream(o); }
void out_close(Output *o) { SDL_DestroyAudioStream(o); }
void out_gain(Output *o, float gain) { SDL_SetAudioStreamGain(o, gain); }
int native_rate() {
    SDL_AudioSpec spec{};
    int frames = 0;
    return SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &frames) && spec.freq > 0 ? spec.freq : 44100;
}
constexpr int kQueueMs = 96;
#endif

// How Vox drives AudioTrack (vox::DriverAndroid, AudioTrack backend):
//  - _InitAT (0x8024e0): rate = getNativeOutputSampleRate; minbuf = getMinBufferSize(rate,
//    stereo, PCM16); chunk = min(minbuf / 4, 32 ms) frames; m_updateTime = chunk duration;
//    m_dataThreshold = -(m_dataThresholdRatio x minbuf duration); then starts its thread.
//  - UpdateThreadedAT (0x804748): new AudioTrack(MUSIC, rate, stereo, PCM16, minbuf,
//    MODE_STREAM), play() before the first write, a byte[minbuf]; then loops on DoCallbackAT
//    (waiting on a condition variable while _SuspendAT's flag is set) until m_running is
//    cleared; then stop(), release().
//  - DoCallbackAT (0x803e08): mixes one chunk under the driver's vox::Mutex (this+4), releases
//    it, then write(array, 0, chunk * 4) (the result is ignored), m_dataDuration +=
//    m_updateTime, and usleep(m_updateTime) when start + m_dataDuration - now >
//    m_dataThreshold, else loops at once. Its clock is gettimeofday.
// With that pacing the mixer sleeps a whole chunk whenever it is even slightly ahead, so what
// it has written swings by a full chunk (32 ms) against the wall clock, and the old
// cushion/restart policy here left only ~20-35 ms queued at the low point; every run-dry then
// paused the device until 64 ms were queued again (a 35-70 ms gap) and raised the latency.
//
// Now the device's clock paces the mixer, like a classic blocking audio loop:
//  - select_backend sets m_dataThresholdRatio to -1e6, so m_dataThreshold is +40000 s and
//    DoCallbackAT never sleeps: the mixer only waits in write();
//  - write() blocks while the queue holds more than `max_bytes` - chunk (kQueueMs: 96 ms on
//    the PC, 128 ms on the Switch), so the queue stays between max - chunk and max: 64-96 ms
//    of slack for a late mixer, and a constant latency. It waits in short sleeps sized to
//    the missing room (no lock held: Vox calls write outside its own locks);
//  - the device runs from play() to pause()/stop(), and simply plays silence when the queue
//    is empty (SDL pads, the Switch feeder mixes nothing): no pause and refill on underrun;
//    the mixer, no longer tied to a clock, refills the queue as fast as it can mix;
//  - write() never returns at once without having either queued the chunk or waited: a device
//    that stopped consuming gets the chunk's excess dropped after waiting a chunk + 100 ms, a
//    paused or stopped track takes up to 2 x max_bytes and otherwise waits a chunk, and with no
//    output device at all write() sleeps the chunk's duration. Vox no longer sleeps by itself,
//    so it must never spin here.
struct Track {
    Output *stream = nullptr;
    int frame_bytes = 4;
    int rate = 44100;
    int max_bytes = 0;  // the queue's limit: write() waits while a chunk does not fit below it
    int state = kStateStopped;
};

std::mutex g_lock;
std::unordered_map<jni::Handle, Track> g_tracks;
float g_gain = 1;

int channels_of(int config) { return config == kChannelOutStereo || config == kChannelConfStereo ? 2 : 1; }
int sample_bytes(int format) { return format == kEncodingPcm8 ? 1 : 2; }

// ~40 ms: Vox mixes chunks of min(this, 32 ms)
int min_buffer_bytes(int rate, int config, int format) {
    return std::max(1024, rate / 25 * channels_of(config) * sample_bytes(format));
}
int ms_bytes(int rate, int frame_bytes, int ms) { return int(int64_t(rate) * ms / 1000) * frame_bytes; }
int64_t bytes_ns(const Track &t, int64_t bytes) { return bytes * 1'000'000'000 / (int64_t(t.frame_bytes) * t.rate); }

int queue_ms() {
    static const int ms = [] {
        const char *env = std::getenv("NOVA3_AUDIO_QUEUE_MS");  // tests: another queue limit
        const int v = env ? std::atoi(env) : 0;
        return v >= 48 && v <= 1000 ? v : kQueueMs;
    }();
    return ms;
}

Track *find(jni::Handle self) {
    auto it = g_tracks.find(self);
    return it == g_tracks.end() ? nullptr : &it->second;
}

jni::Value integer(int v) {
    jni::Value r{};
    r.i = v;
    return r;
}

void log_underruns(const Track &t) {
    static int logged;
    const int n = g_underruns.load(std::memory_order_relaxed);
    if (n == logged || (n > 5 && n / 100 == logged / 100)) return;  // the first five, then every 100th
    logged = n;
    rt::log("audio: underrun %d (%lld ms in all)", n, (long long)(bytes_ns(t, g_underrun_bytes.load()) / 1'000'000));
}

// AudioTrack.write(array, offset, count) for byte[] and short[]: queue the samples, blocking
// while the queue is full (Android's MODE_STREAM blocking write; see Track).
jni::Value write(jni::Call &c) {
    static RT_THREAD_LOCAL(bool, raised_slots);  // the mixer thread: ahead of the game's busy threads
    bool &raised = RT_TLS(raised_slots);
    if (!raised) {
        raised = true;
        rt::os::raise_thread_priority();
    }
    uint32_t buf = 0, len = 0;
    char elem = 0;
    if (!jni::array_data(c.args[0].l, buf, len, elem)) return integer(-3);  // ERROR_BAD_VALUE
    const int esize = elem == 'S' ? 2 : 1;
    uint32_t off = uint32_t(c.args[1].i), count = uint32_t(c.args[2].i);
    if (off > len || count > len - off) return integer(-3);
    const uint8_t *data = reinterpret_cast<const uint8_t *>(rt::ptr(buf + off * esize));
    int bytes = int(count) * esize;
    static bool heard;  // log once that the mixer produces sound (not just silence); PCM 16-bit
    if (!heard)
        for (int i = 0; i + 1 < bytes; i += 2) {
            uint16_t w;
            std::memcpy(&w, data + i, 2);
            const int16_t v = int16_t(LE16(w));
            if (std::abs(v) > 256) {
                heard = true;
                rt::log("audio: first sound (sample %d)", v);
                break;
            }
        }
    static FILE *dump = std::getenv("NOVA3_AUDIO_DUMP") ? std::fopen(std::getenv("NOVA3_AUDIO_DUMP"), "wb") : nullptr;
    if (dump) std::fwrite(data, 1, size_t(bytes), dump);  // debug: raw PCM as written by the game

    std::unique_lock<std::mutex> lock(g_lock);
    Track *t = find(c.self);
    if (!t) return integer(-3);
    const int64_t chunk_ns = bytes_ns(*t, bytes);
    if (!t->stream) {  // no output device: consume in real time, so the mixer does not spin
        lock.unlock();
        rt::os::sleep_ns(chunk_ns);
        return integer(int(count));
    }
    const int64_t give_up = rt::os::monotonic_ns() + chunk_ns + 100'000'000;
    int queued = out_queued(t->stream);
    while (t->state == kStatePlaying && queued + bytes > t->max_bytes) {
        const int64_t now = rt::os::monotonic_ns();
        if (now >= give_up) break;  // the device stopped consuming: drop below
        // about until the device has taken what is missing (it pulls 10-21 ms at a time)
        const int64_t ns = std::clamp<int64_t>(bytes_ns(*t, queued + bytes - t->max_bytes), 1'000'000, 20'000'000);
        lock.unlock();
        rt::os::sleep_ns(std::min(ns, give_up - now));
        lock.lock();
        t = find(c.self);
        if (!t || !t->stream) return integer(-3);
        queued = out_queued(t->stream);
    }
    // a paused or stopped track keeps what is written (one chunk at most: Vox checks its own
    // suspend flag between chunks), up to twice the limit
    const int limit = t->state == kStatePlaying ? t->max_bytes : 2 * t->max_bytes;
    const int room = std::max(0, limit - queued) / t->frame_bytes * t->frame_bytes;
    if (bytes > room) {
        static int drops;  // the oldest part of the chunk is left out
        if (++drops <= 5 || drops % 100 == 0)
            rt::log("audio: %lld ms dropped (%d)", (long long)(bytes_ns(*t, bytes - room) / 1'000'000), drops);
        data += bytes - room;
        bytes = room;
    }
    if (bytes > 0) {
        out_put(t->stream, data, bytes);
        g_fed.store(true, std::memory_order_relaxed);
    }
    if (t->state == kStatePlaying) log_underruns(*t);
    else if (bytes == 0) {  // nothing fit and nothing consumes: never return at once
        lock.unlock();
        rt::os::sleep_ns(chunk_ns);
    }
    return integer(int(count));
}

}  // namespace

void register_java() {
    using jni::Call;
    using jni::Value;
    const std::string cls = "android/media/AudioTrack.";
    jni::register_method(cls + "getNativeOutputSampleRate(I)I", [](Call &) {
        return integer(native_rate());
    });
    jni::register_method(cls + "getMinBufferSize(III)I", [](Call &c) {
        return integer(min_buffer_bytes(c.args[0].i, c.args[1].i, c.args[2].i));
    });
    // AudioTrack(streamType, sampleRate, channelConfig, audioFormat, bufferSizeInBytes, mode)
    jni::register_method(cls + "<init>(IIIIII)V", [](Call &c) {
        const int rate = c.args[1].i, config = c.args[2].i, format = c.args[3].i, bytes = c.args[4].i;
        Track t;
        t.stream = out_open(rate, channels_of(config), format == kEncodingPcm8);
        t.frame_bytes = channels_of(config) * sample_bytes(format);
        t.rate = rate;
        t.max_bytes = std::max(2 * bytes, ms_bytes(rate, t.frame_bytes, queue_ms()));
        if (t.stream) out_gain(t.stream, g_gain);
#if defined(__3DS__) || defined(__GAMECUBE__)
        if (!t.stream) rt::log("audio: no output device");
#else
        if (!t.stream) rt::log("audio: cannot open the output device: %s", SDL_GetError());
#endif
        else rt::log("audio: %d Hz, %d channel(s), %d-bit, queue at most %lld ms", rate, channels_of(config),
                     8 * sample_bytes(format), (long long)(bytes_ns(t, t.max_bytes) / 1'000'000));
        std::lock_guard<std::mutex> lock(g_lock);
        g_tracks[c.self] = t;
        return Value{};
    });
    jni::register_method(cls + "play()V", [](Call &c) {
        std::lock_guard<std::mutex> lock(g_lock);
        if (Track *t = find(c.self); t && t->stream) {
            t->state = kStatePlaying;  // the device plays silence until write() queues sound
            out_resume(t->stream);
        }
        return Value{};
    });
    jni::register_method(cls + "pause()V", [](Call &c) {
        std::lock_guard<std::mutex> lock(g_lock);
        if (Track *t = find(c.self); t && t->stream) {
            out_pause(t->stream);
            t->state = kStatePaused;
        }
        return Value{};
    });
    jni::register_method(cls + "stop()V", [](Call &c) {
        std::lock_guard<std::mutex> lock(g_lock);
        if (Track *t = find(c.self); t && t->stream) {
            out_pause(t->stream);
            out_clear(t->stream);
            g_fed.store(false, std::memory_order_relaxed);
            t->state = kStateStopped;
        }
        return Value{};
    });
    jni::register_method(cls + "release()V", [](Call &c) {
        std::lock_guard<std::mutex> lock(g_lock);
        if (Track *t = find(c.self)) {
            if (t->stream) out_close(t->stream);
            g_tracks.erase(c.self);
        }
        return Value{};
    });
    jni::register_method(cls + "getPlayState()I", [](Call &c) {
        std::lock_guard<std::mutex> lock(g_lock);
        Track *t = find(c.self);
        return integer(t ? t->state : kStateStopped);
    });
    jni::register_method(cls + "write([BII)I", write);
    jni::register_method(cls + "write([SII)I", write);
}

void set_volume(float gain) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_gain = std::clamp(gain, 0.0f, 1.0f);
    for (auto &[h, t] : g_tracks)
        if (t.stream) out_gain(t.stream, g_gain);
}

void select_backend() {
    if (uint32_t fn = rt::export_address("VoxSetAndroidAPILevel")) rt::call(rt::current_cpu(), fn, {8});
    // m_dataThreshold = -ratio x 40 ms = +40000 s: DoCallbackAT never sleeps, write() paces the
    // mixer by the device's clock (see Track). _InitAT reads the ratio when Vox starts.
    if (uint32_t ratio = rt::export_address("_ZN3vox13DriverAndroid20m_dataThresholdRatioE")) {
        const double never = -1e6;
        uint64_t bits;
        std::memcpy(&bits, &never, sizeof bits);
        ST64(ratio, bits);
    } else {
        rt::log("audio: m_dataThresholdRatio not found");
    }
}

}  // namespace audio
