/*
 * audio_output_xaudio2.cpp -- XAudio2 output for replaced DirectSound buffers.
 *
 * From doaxbv-re (https://github.com/NoRain211/doaxbv-re,
 * recomp-runtime/audio_output_xaudio2.cpp), GPL-3.0. Changed: an unset
 * RECOMP_AUDIO_GAIN means full gain here rather than muted.
 */
#include "audio_output.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <xaudio2.h>
#include <xaudio2fx.h>
#include <xapofx.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

/* 0..255 are DirectSound buffers (hle_dsound.c), 256..271 streams
 * (hle_dsound_stream.c), 272 a movie's sound (hle_xmv.c). */
constexpr uint32_t kVoiceCount = 256 + 16 + 1;
/* Submissions in flight per voice. A stream keeps LEAD_MS of sound ahead of
   its clock in CHUNK_MS pieces (hle_dsound_stream.c), so four was one short
   of what the music path asks for and every frame past the first dropped a
   whole chunk: the sound fell behind the picture by 100 ms at a time, and a
   two-minute run lost 42 seconds of music. */
constexpr uint32_t kQueueSize = 12;
constexpr uint32_t kMaxBufferBytes = 160000;

struct PcmBuffer {
    uint8_t *data;
    uint32_t capacity;
};

struct OutputVoice {
    IXAudio2SourceVoice *source;
    PcmBuffer buffers[kQueueSize];
    uint32_t sample_rate, channels, bits_per_sample;
    uint32_t front, queued;
    /* Times the voice had played everything it was given before the next
       submission arrived: each one is a gap in the sound. */
    uint32_t starved;
    bool fed;
    uint32_t mix_applied;           /* SlotMix::generation last applied */
    uint32_t route_applied;         /* the submix key its sends point at, 0 = speakers */
};

/* recomp_audio_output_set_mix / _route: kept per slot, not per voice, so a
   voice the slot gets later (a format change, a reset) starts with them. */
struct SlotMix {
    bool set;
    RecompAudioMix mix;
    uint32_t generation;            /* bumped on every change; 0 = never set */
    uint32_t route;                 /* submix key, 0 = speakers */
};

/* A mix-in buffer's submix (recomp_audio_output_set_submix). */
struct Submix {
    uint32_t key;
    IXAudio2SubmixVoice *voice;
    uint32_t channels;
};
constexpr uint32_t kSubmixCount = 32;

/* Voices may play up to this much faster than they are submitted. */
constexpr float kMaxFrequencyRatio = 8.0f;

std::atomic<HRESULT> critical_error{S_OK};

struct EngineCallback : IXAudio2EngineCallback {
    void STDMETHODCALLTYPE OnProcessingPassStart() override {}
    void STDMETHODCALLTYPE OnProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnCriticalError(HRESULT error) override
    {
        critical_error.store(error, std::memory_order_relaxed);
    }
};

EngineCallback callback;
IXAudio2 *engine;
IXAudio2MasteringVoice *master;
OutputVoice voices[kVoiceCount];
SlotMix mixes[kVoiceCount];
bool reported_nonzero[kVoiceCount];
/* The mastering voice's speakers: how many, and which (SPEAKER_* mask). */
uint32_t master_channels;
DWORD master_mask;
/* RECOMP_AUDIO_METER=1: XAudio2's volume meter on the mastering voice. The
   mix is float until it reaches the device, where anything past full scale is
   clipped, so this is the one place distortion from a too-loud mix shows. */
bool meter_on;
/* With the meter on, every voice goes through this submix, which carries the
   meter, on its way to the mastering voice -- so the meter reads the mix
   before the master's volume, and a muted run (RECOMP_MUTE) can still be
   measured. */
IXAudio2SubmixVoice *meter_mix;
/* The I3DL2 room (recomp_audio_output_set_reverb): one mono-in submix with
   XAudio2's reverb, which takes I3DL2 parameters as the console's does.
   Silent (volume 0) until a title describes a room. */
IXAudio2SubmixVoice *reverb_voice;
uint32_t reverb_inputs;             /* 1, or 2 where the reverb would not take mono */
Submix submixes[kSubmixCount];
unsigned long long meter_last_poll, meter_last_report;
float meter_peak_max;
unsigned long meter_readings, meter_over;
bool attempted, com_initialized, callback_registered, summary_printed;
unsigned long long submitted_buffers, submitted_bytes, nonzero_buffers;
unsigned long long dropped_buffers;
/* Gaps since the last report, by slot, and in all. */
unsigned long starved_total;
unsigned long long last_starve_report;

/* RECOMP_AUDIO_CAPTURE=<dir>: everything a slot is given, as raw PCM in
   <dir>/slot<N>_<rate>_<channels>ch_<bits>.raw, so what reached the host
   can be listened to or compared with what the title wrote. */
const char *capture_dir()
{
    static int read;
    static const char *dir;
    if (!read) {
        read = 1;
        dir = std::getenv("RECOMP_AUDIO_CAPTURE");
        if (dir && !*dir) dir = nullptr;
    }
    return dir;
}

void capture(uint32_t slot, const uint8_t *pcm, uint32_t bytes,
             uint32_t rate, uint32_t channels, uint32_t bits)
{
    static FILE *files[kVoiceCount];
    static uint32_t formats[kVoiceCount];
    const char *dir = capture_dir();
    const uint32_t format = rate << 8 | channels << 6 | bits;
    if (!dir) return;
    if (files[slot] && formats[slot] != format) {
        std::fclose(files[slot]);
        files[slot] = nullptr;
    }
    if (!files[slot]) {
        char path[1024];
        std::snprintf(path, sizeof path, "%s/slot%u_%u_%uch_%u.raw", dir, slot,
                      rate, channels, bits);
        files[slot] = std::fopen(path, "ab");
        formats[slot] = format;
        if (!files[slot]) return;
    }
    std::fwrite(pcm, 1, bytes, files[slot]);
    std::fflush(files[slot]);
}

void destroyVoice(OutputVoice &voice)
{
    /* DestroyVoice waits until XAudio2 no longer reads these buffers. */
    if (voice.source) voice.source->DestroyVoice();
    for (auto &buffer : voice.buffers) std::free(buffer.data);
    voice = {};
}

void releaseOutput()
{
    for (auto &voice : voices) destroyVoice(voice);
    for (auto &sub : submixes) {
        if (sub.voice) sub.voice->DestroyVoice();
        sub = {};
    }
    if (reverb_voice) reverb_voice->DestroyVoice();
    reverb_voice = nullptr;
    if (meter_mix) meter_mix->DestroyVoice();
    meter_mix = nullptr;
    meter_on = false;
    if (master) master->DestroyVoice();
    master = nullptr;
    if (engine) {
        if (callback_registered) engine->UnregisterForCallbacks(&callback);
        engine->Release();
    }
    engine = nullptr;
    callback_registered = false;
    if (com_initialized) CoUninitialize();
    com_initialized = false;
}

void disableOutput(const char *operation, HRESULT error)
{
    std::fprintf(stderr,
        "[audio-output] disabled operation=%s error=0x%08lx\n",
        operation, static_cast<unsigned long>(error));
    releaseOutput();
}

void checkCriticalError()
{
    const HRESULT error = critical_error.load(std::memory_order_relaxed);
    if (engine && FAILED(error)) disableOutput("device", error);
}

/* The mastering voice's channel for a SPEAKER_* bit, or -1. */
int masterChannel(DWORD speaker)
{
    if (!(master_mask & speaker)) return -1;
    DWORD below = master_mask & (speaker - 1);
    int index = 0;
    while (below) { index += below & 1u; below >>= 1; }
    return index < static_cast<int>(master_channels) ? index : -1;
}

/* Add `level` of a source channel to an Xbox speaker, on whatever the device
   has: the speaker itself, else its usual fold-down (centre to both fronts
   and the backs to the sides or the fronts at -3 dB, LFE dropped, a mono
   device taking everything at -3 dB). */
void foldSpeaker(float *matrix, uint32_t src_channels, uint32_t c, int speaker, float level)
{
    constexpr float kMinus3dB = 0.70710678f;
    auto add = [&](int channel, float gain) {
        if (channel >= 0)
            matrix[static_cast<uint32_t>(channel) * src_channels + c] += level * gain;
    };
    const int fl = masterChannel(SPEAKER_FRONT_LEFT), fr = masterChannel(SPEAKER_FRONT_RIGHT);
    if (fl < 0 && fr < 0) {                              /* not a speaker layout we know */
        if (speaker != RECOMP_SPEAKER_LFE) add(0, kMinus3dB);
        return;
    }
    switch (speaker) {
    case RECOMP_SPEAKER_FRONT_LEFT:  add(fl, 1.0f); break;
    case RECOMP_SPEAKER_FRONT_RIGHT: add(fr, 1.0f); break;
    case RECOMP_SPEAKER_CENTER: {
        const int ch = masterChannel(SPEAKER_FRONT_CENTER);
        if (ch >= 0) add(ch, 1.0f);
        else { add(fl, kMinus3dB); add(fr, kMinus3dB); }
        break;
    }
    case RECOMP_SPEAKER_LFE:
        add(masterChannel(SPEAKER_LOW_FREQUENCY), 1.0f);
        break;
    case RECOMP_SPEAKER_BACK_LEFT: {
        int ch = masterChannel(SPEAKER_BACK_LEFT);
        if (ch < 0) ch = masterChannel(SPEAKER_SIDE_LEFT);
        if (ch >= 0) add(ch, 1.0f); else add(fl, kMinus3dB);
        break;
    }
    case RECOMP_SPEAKER_BACK_RIGHT: {
        int ch = masterChannel(SPEAKER_BACK_RIGHT);
        if (ch < 0) ch = masterChannel(SPEAKER_SIDE_RIGHT);
        if (ch >= 0) add(ch, 1.0f); else add(fr, kMinus3dB);
        break;
    }
    default: break;
    }
}

/* Where the dry mix goes: the meter's submix when there is one. */
IXAudio2Voice *dryTarget()
{
    return meter_mix ? static_cast<IXAudio2Voice *>(meter_mix)
                     : static_cast<IXAudio2Voice *>(master);
}

Submix *findSubmix(uint32_t key)
{
    for (auto &sub : submixes)
        if (sub.key == key && sub.voice) return &sub;
    return nullptr;
}

/* A voice's sends: the submix it is routed to, or the speakers and the
   reverb. A new send gets XAudio2's default levels, so the reverb's is set to
   nothing straight away; the mix, if any, sets it properly. */
void applySends(IXAudio2Voice *v, uint32_t channels, uint32_t route)
{
    XAUDIO2_SEND_DESCRIPTOR d[2] = {};
    uint32_t n = 0;
    Submix *sub = route ? findSubmix(route) : nullptr;
    if (sub) {
        d[n++] = {0, sub->voice};
    } else {
        d[n++] = {0, dryTarget()};
        if (reverb_voice) d[n++] = {0, reverb_voice};
    }
    XAUDIO2_VOICE_SENDS sends{n, d};
    v->SetOutputVoices(&sends);
    if (!sub && reverb_voice) {
        float zero[2 * 2] = {};
        v->SetOutputMatrix(reverb_voice, channels, reverb_inputs, zero);
    }
}

/* The speaker and reverb levels of `mix` on `v`, which has `channels`. */
void applyLevels(IXAudio2Voice *v, uint32_t channels, const RecompAudioMix &mix)
{
    float matrix[2 * XAUDIO2_MAX_AUDIO_CHANNELS] = {};
    for (uint32_t c = 0; c < channels && c < 2; ++c)
        for (int s = 0; s < RECOMP_SPEAKERS; ++s)
            if (mix.levels[c][s] != 0.0f)
                foldSpeaker(matrix, channels, c, s, mix.levels[c][s]);
    v->SetOutputMatrix(dryTarget(), channels, master_channels, matrix);
    if (reverb_voice) {
        /* Channel to input; a mono voice (the console's I3DL2 bin is mono)
           into both at -3 dB, which measured within 1.2 dB of a mono-input
           reverb's level. */
        float send[2 * 2] = {};
        for (uint32_t c = 0; c < channels && c < 2; ++c)
            for (uint32_t i = 0; i < reverb_inputs; ++i)
                if (reverb_inputs == 1 || i == c || channels == 1)
                    send[i * channels + c] = mix.reverb[c] *
                        (channels == 1 && reverb_inputs == 2 ? 0.70710678f : 1.0f);
        v->SetOutputMatrix(reverb_voice, channels, reverb_inputs, send);
    }
}

/* Put the slot's mix and route (if it has them) on its voice. */
void applyMix(uint32_t slot, OutputVoice &voice)
{
    SlotMix &sm = mixes[slot];
    if (!voice.source || !master_channels)
        return;
    const uint32_t route = sm.route && findSubmix(sm.route) ? sm.route : 0;
    if (voice.route_applied != route) {
        applySends(voice.source, voice.channels, route);
        voice.route_applied = route;
        voice.mix_applied = 0;          /* the matrices go with the sends */
    }
    if (!sm.set || voice.mix_applied == sm.generation)
        return;
    const RecompAudioMix &mix = sm.mix;
    if (route) {
        /* Into the mix-in buffer as it is: channel for channel, a mono voice
         * into every channel of a stereo submix, stereo into mono at half. */
        Submix *sub = findSubmix(route);
        float matrix[2 * 2] = {};
        for (uint32_t c = 0; c < voice.channels && c < 2; ++c)
            for (uint32_t o = 0; o < sub->channels && o < 2; ++o)
                if (voice.channels == 1 || sub->channels == 1 || o == c)
                    matrix[o * voice.channels + c] =
                        (sub->channels == 1 && voice.channels == 2) ? 0.5f : 1.0f;
        voice.source->SetOutputMatrix(sub->voice, voice.channels, sub->channels, matrix);
    } else {
        applyLevels(voice.source, voice.channels, mix);
    }
    XAUDIO2_FILTER_PARAMETERS filter{LowPassFilter, XAUDIO2_MAX_FILTER_FREQUENCY, 1.0f};
    if (mix.filter.low_pass) {
        filter.Frequency = mix.filter.frequency;
        filter.OneOverQ = mix.filter.one_over_q;
        if (!(filter.Frequency > 0.0f)) filter.Frequency = 0.0f;
        if (filter.Frequency > XAUDIO2_MAX_FILTER_FREQUENCY) filter.Frequency = XAUDIO2_MAX_FILTER_FREQUENCY;
        if (!(filter.OneOverQ > 0.0f)) filter.OneOverQ = 0.01f;
        if (filter.OneOverQ > XAUDIO2_MAX_FILTER_ONEOVERQ) filter.OneOverQ = XAUDIO2_MAX_FILTER_ONEOVERQ;
    }
    voice.source->SetFilterParameters(&filter);
    float ratio = mix.ratio;
    if (!(ratio >= 1.0f / kMaxFrequencyRatio)) ratio = 1.0f / kMaxFrequencyRatio;
    if (ratio > kMaxFrequencyRatio) ratio = kMaxFrequencyRatio;
    voice.source->SetFrequencyRatio(ratio);
    voice.mix_applied = sm.generation;
}

float gainOf(int32_t hundredth_db)
{
    if (hundredth_db <= -10000) return 0.0f;
    if (hundredth_db > 0) hundredth_db = 0;
    return std::pow(10.0f, hundredth_db / 2000.0f);
}

/* RECOMP_AUDIO_METER: read the mastering voice's peak, at most every 2 ms,
   and say every 5 s how loud the mix got and how often it was at or past
   full scale -- clipped on its way to the device. The meter reports the last
   processing pass only, so this samples the passes rather than seeing all. */
void pollMeter()
{
    if (!meter_on || !meter_mix) return;
    const unsigned long long now = GetTickCount64();
    if (now - meter_last_poll < 2) return;
    meter_last_poll = now;
    float peaks[XAUDIO2_MAX_AUDIO_CHANNELS] = {}, rms[XAUDIO2_MAX_AUDIO_CHANNELS] = {};
    XAUDIO2FX_VOLUMEMETER_LEVELS levels{};
    levels.pPeakLevels = peaks;
    levels.pRMSLevels = rms;
    levels.ChannelCount = master_channels;
    if (FAILED(meter_mix->GetEffectParameters(0, &levels, sizeof levels))) return;
    float peak = 0.0f;
    for (uint32_t i = 0; i < master_channels; ++i)
        if (peaks[i] > peak) peak = peaks[i];
    ++meter_readings;
    if (peak >= 1.0f) ++meter_over;
    if (peak > meter_peak_max) meter_peak_max = peak;
    if (!meter_last_report) meter_last_report = now;
    if (now - meter_last_report >= 5000) {
        std::fprintf(stderr,
            "[audio-output] master peak %.1f dBFS; %lu of %lu readings at or past full scale\n",
            meter_peak_max > 0.0f ? 20.0 * std::log10(meter_peak_max) : -120.0,
            meter_over, meter_readings);
        std::fflush(stderr);
        meter_peak_max = 0.0f;
        meter_over = meter_readings = 0;
        meter_last_report = now;
    }
}

void dropBuffer(uint32_t slot, const char *reason)
{
    ++dropped_buffers;
    if (dropped_buffers <= 4 ||
        (dropped_buffers & (dropped_buffers - 1)) == 0) {
        std::fprintf(stderr,
            "[audio-output] dropped=%llu slot=%u reason=%s\n",
            dropped_buffers, slot, reason);
    }
}

} // namespace

extern "C" void recomp_audio_output_initialize(void)
{
    checkCriticalError();
    if (attempted) return;
    attempted = true;
    double gain = 1.0;
    const char *setting = std::getenv("RECOMP_AUDIO_GAIN");
    if (setting) {
        char *end;
        gain = std::strtod(setting, &end);
        if (end == setting || *end != '\0' || !std::isfinite(gain) ||
            gain < 0.0 || gain > 1.0) {
            std::fprintf(stderr,
                "[audio-output] muted invalid RECOMP_AUDIO_GAIN (expected 0..1)\n");
            return;
        }
    }
    if (gain == 0.0) {
        std::fprintf(stderr, "[audio-output] muted RECOMP_AUDIO_GAIN=0\n");
        return;
    }
    HRESULT error = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(error)) {
        disableOutput("CoInitializeEx", error);
        return;
    }
    com_initialized = true;
    error = XAudio2Create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(error)) {
        disableOutput("XAudio2Create", error);
        return;
    }
    error = engine->RegisterForCallbacks(&callback);
    if (FAILED(error)) {
        disableOutput("RegisterForCallbacks", error);
        return;
    }
    callback_registered = true;
    error = engine->CreateMasteringVoice(&master);
    if (FAILED(error)) {
        disableOutput("CreateMasteringVoice", error);
        return;
    }
    {
        XAUDIO2_VOICE_DETAILS details{};
        master->GetVoiceDetails(&details);
        master_channels = details.InputChannels;
        if (master_channels > XAUDIO2_MAX_AUDIO_CHANNELS) master_channels = 0;
        if (FAILED(master->GetChannelMask(&master_mask))) master_mask = 0;
        std::fprintf(stderr, "[audio-output] device: %u channel(s), speaker mask 0x%lx\n",
                     master_channels, static_cast<unsigned long>(master_mask));
    }
    /* A limiter on the mix, on by default (RECOMP_AUDIO_LIMITER=0 for none).
     * Voices are summed in float and anything past full scale used to be
     * clipped where the mix met the device. Measured with RECOMP_AUDIO_METER
     * (Oct 2026): MK Shaolin Monks' fights peaked at +3.8 dBFS with every
     * voice at one gain and +8.3 dBFS once voices reached the speakers their
     * mix bins name, past full scale in up to 43 readings of 320 -- the
     * distortion players heard, in Max Payne too. Below full scale it does
     * nothing (unity loudness); above it it limits instead of clipping. */
    {
        const char *limit = std::getenv("RECOMP_AUDIO_LIMITER");
        if (!(limit && std::strcmp(limit, "0") == 0) && master_channels) {
            IUnknown *fx = nullptr;
            FXMASTERINGLIMITER_PARAMETERS params{FXMASTERINGLIMITER_DEFAULT_RELEASE,
                                                 FXMASTERINGLIMITER_DEFAULT_LOUDNESS};
            if (SUCCEEDED(CreateFX(__uuidof(FXMasteringLimiter), &fx, &params, sizeof params))) {
                XAUDIO2_EFFECT_DESCRIPTOR effect{fx, TRUE, master_channels};
                XAUDIO2_EFFECT_CHAIN chain{1, &effect};
                const bool on = SUCCEEDED(master->SetEffectChain(&chain));
                fx->Release();
                std::fprintf(stderr, "[audio-output] limiter %s\n",
                             on ? "on the mix (RECOMP_AUDIO_LIMITER=0 for none)"
                                : "could not be attached; the mix can clip");
            } else {
                std::fprintf(stderr, "[audio-output] no limiter (CreateFX failed); the mix can clip\n");
            }
        }
    }
    {
        const char *meter = std::getenv("RECOMP_AUDIO_METER");
        IUnknown *apo = nullptr;
        if (meter && *meter && std::strcmp(meter, "0") != 0 && master_channels &&
            SUCCEEDED(XAudio2CreateVolumeMeter(&apo, 0))) {
            XAUDIO2_VOICE_DETAILS details{};
            master->GetVoiceDetails(&details);
            XAUDIO2_EFFECT_DESCRIPTOR effect{apo, TRUE, master_channels};
            XAUDIO2_EFFECT_CHAIN chain{1, &effect};
            /* Processing stage 2: after the mix-in submixes (0) and the
               reverb (1), which both send to it. */
            meter_on = SUCCEEDED(engine->CreateSubmixVoice(
                &meter_mix, master_channels, details.InputSampleRate, 0, 2, nullptr, &chain));
            if (!meter_on) meter_mix = nullptr;
            apo->Release();
            std::fprintf(stderr, "[audio-output] RECOMP_AUDIO_METER: %s\n",
                         meter_on ? "metering the mix" : "could not attach the meter");
        }
    }
    /* The I3DL2 room. XAudio2's reverb takes I3DL2 parameters through
       ReverbConvertI3DL2ToNative, as the console's DSP reverb takes them from
       IDirectSound_SetI3DL2Listener. RECOMP_AUDIO_REVERB=0 for none.

       Two inputs and two outputs, and nothing else. The first version took
       one input to two (or six) outputs, the console's shape -- a mono I3DL2
       bin, a multichannel room -- and XAudio2 accepts that voice but does not
       run it reliably: measured in isolation (Oct 2026), its output was
       garbage in one run of three, up to 294 dBFS, with nothing sent to it,
       and 12 dB too loud in the others; at volume 0 it still was. In MK Shaolin
       Monks the mix read a constant 128-182 dBFS from boot in some runs, with
       the new mixing switched off, since the reverb is made whatever the title
       does. Two-in, two-out ran clean every time, silent with silence in and
       steady with noise. A mono send reaches both inputs at -3 dB
       (applyLevels). On a 5.1 device its stereo output takes the default
       upmix. It starts silent -- volume 0 and a room of -10000 -- until a
       title describes a room. */
    {
        const char *rev = std::getenv("RECOMP_AUDIO_REVERB");
        if (!(rev && std::strcmp(rev, "0") == 0) && master_channels) {
            XAUDIO2_VOICE_DETAILS details{};
            master->GetVoiceDetails(&details);
            IUnknown *apo = nullptr;
            if (SUCCEEDED(XAudio2CreateReverb(&apo, 0))) {
                XAUDIO2_EFFECT_DESCRIPTOR effect{apo, TRUE, 2};
                XAUDIO2_EFFECT_CHAIN chain{1, &effect};
                XAUDIO2_SEND_DESCRIPTOR d{0, dryTarget()};
                XAUDIO2_VOICE_SENDS sends{1, &d};
                if (SUCCEEDED(engine->CreateSubmixVoice(&reverb_voice, 2,
                        details.InputSampleRate, 0, 1, &sends, &chain)))
                    reverb_inputs = 2;
                else
                    reverb_voice = nullptr;
                apo->Release();
            }
            if (reverb_voice) {
                XAUDIO2FX_REVERB_I3DL2_PARAMETERS silent = XAUDIO2FX_I3DL2_PRESET_DEFAULT;
                silent.WetDryMix = 100.0f;
                silent.Room = -10000;
                XAUDIO2FX_REVERB_PARAMETERS native{};
                ReverbConvertI3DL2ToNative(&silent, &native);
                reverb_voice->SetEffectParameters(0, &native, sizeof native);
                reverb_voice->SetVolume(0.0f);
            }
            std::fprintf(stderr, "[audio-output] reverb: %s\n",
                         reverb_voice ? "ready (stereo in and out)"
                                      : "could not be made; voices play dry");
        }
    }
    /* RECOMP_MUTE: silent, but everything else as usual. RECOMP_AUDIO_GAIN=0
     * opens no device at all, and then nothing reports a play position: the
     * DirectSound streams and the movie's sound fall back to other clocks, so
     * a muted run would not be the run being tested. */
    {
        const char *mute = std::getenv("RECOMP_MUTE");
        if (mute && *mute && std::strcmp(mute, "0") != 0) {
            gain = 0.0;
            std::fprintf(stderr, "[audio-output] RECOMP_MUTE: playing silently\n");
        }
    }
    error = master->SetVolume(static_cast<float>(gain));
    if (FAILED(error)) {
        disableOutput("MasterSetVolume", error);
        return;
    }
    std::fprintf(stderr,
        "[audio-output] initialized backend=xaudio2_9 master_gain=%.6f\n", gain);
}

extern "C" void recomp_audio_output_shutdown(void)
{
    if (summary_printed) return;
    checkCriticalError();
    releaseOutput();
    attempted = true;
    summary_printed = true;
    std::fprintf(stderr,
        "[audio-output] summary submitted_buffers=%llu submitted_bytes=%llu "
        "nonzero_buffers=%llu dropped_buffers=%llu\n",
        submitted_buffers, submitted_bytes, nonzero_buffers, dropped_buffers);
}

extern "C" void recomp_audio_output_reset_voice(uint32_t slot)
{
    checkCriticalError();
    if (slot < kVoiceCount) destroyVoice(voices[slot]);
}

extern "C" void recomp_audio_output_set_mix(uint32_t slot, const RecompAudioMix *mix)
{
    if (slot >= kVoiceCount || !mix) return;
    SlotMix &sm = mixes[slot];
    if (sm.set && std::memcmp(&sm.mix, mix, sizeof sm.mix) == 0)
        return;
    sm.mix = *mix;
    sm.set = true;
    if (++sm.generation == 0) sm.generation = 1;
    if (engine) applyMix(slot, voices[slot]);
}

extern "C" void recomp_audio_output_route(uint32_t slot, uint32_t key)
{
    if (slot >= kVoiceCount || mixes[slot].route == key) return;
    mixes[slot].route = key;
    if (++mixes[slot].generation == 0) mixes[slot].generation = 1;
    if (engine) applyMix(slot, voices[slot]);
}

extern "C" void recomp_audio_output_set_submix(uint32_t key, uint32_t channels,
                                               const RecompAudioMix *mix,
                                               int32_t volume_hundredth_db)
{
    recomp_audio_output_initialize();
    if (!engine || !key || !mix || !master_channels) return;
    if (channels < 1 || channels > 2) channels = 2;
    Submix *sub = findSubmix(key);
    if (sub && sub->channels != channels) {
        /* A different shape: make it again, and point what was routed to it
           there anew at the next submission. */
        sub->voice->DestroyVoice();
        *sub = {};
        sub = nullptr;
        for (auto &v : voices) if (v.route_applied == key) v.route_applied = ~0u;
    }
    if (!sub) {
        for (auto &s : submixes) if (!s.voice) { sub = &s; break; }
        if (!sub) {
            static bool said;
            if (!said) {
                said = true;
                std::fprintf(stderr, "[audio-output] more than %u mix-in buffers; "
                             "the rest play to the speakers\n", kSubmixCount);
            }
            return;
        }
        XAUDIO2_VOICE_DETAILS details{};
        master->GetVoiceDetails(&details);
        XAUDIO2_SEND_DESCRIPTOR d[2] = {{0, dryTarget()}, {0, reverb_voice}};
        XAUDIO2_VOICE_SENDS sends{reverb_voice ? 2u : 1u, d};
        if (FAILED(engine->CreateSubmixVoice(&sub->voice, channels, details.InputSampleRate,
                                             0, 0, &sends, nullptr))) {
            sub->voice = nullptr;
            return;
        }
        sub->key = key;
        sub->channels = channels;
        std::fprintf(stderr, "[audio-output] mix-in buffer %08X: %u-channel submix\n",
                     key, channels);
    }
    applyLevels(sub->voice, channels, *mix);
    sub->voice->SetVolume(gainOf(volume_hundredth_db));
}

extern "C" void recomp_audio_output_set_reverb(const RecompI3dl2 *room)
{
    recomp_audio_output_initialize();
    if (!engine || !reverb_voice) return;
    if (!room || room->room <= -10000) {
        reverb_voice->SetVolume(0.0f);
        return;
    }
    XAUDIO2FX_REVERB_I3DL2_PARAMETERS i3{};
    i3.WetDryMix = 100.0f;                 /* a send: all of what reaches it is the room */
    i3.Room = room->room;
    i3.RoomHF = room->room_hf;
    i3.RoomRolloffFactor = room->room_rolloff_factor;
    i3.DecayTime = room->decay_time;
    i3.DecayHFRatio = room->decay_hf_ratio;
    i3.Reflections = room->reflections;
    i3.ReflectionsDelay = room->reflections_delay;
    i3.Reverb = room->reverb;
    i3.ReverbDelay = room->reverb_delay;
    i3.Diffusion = room->diffusion;
    i3.Density = room->density;
    i3.HFReference = room->hf_reference;
    XAUDIO2FX_REVERB_PARAMETERS native{};
    ReverbConvertI3DL2ToNative(&i3, &native);
    reverb_voice->SetEffectParameters(0, &native, sizeof native);
    reverb_voice->SetVolume(1.0f);
}

extern "C" void recomp_audio_output_clear_mix(uint32_t slot)
{
    /* The slot's next voice gets the default mapping and the submitted rate.
       The current one keeps what it has rather than being restarted, which
       would drop what it has queued. */
    if (slot >= kVoiceCount) return;
    mixes[slot].set = false;
}

extern "C" int recomp_audio_output_position(uint32_t slot, uint64_t *played_bytes,
                                            uint32_t *queued_buffers)
{
    if (!engine || slot >= kVoiceCount) return 0;
    OutputVoice &voice = voices[slot];
    if (!voice.source) return 0;

    XAUDIO2_VOICE_STATE state{};
    voice.source->GetState(&state, 0);     /* SamplesPlayed wanted here */
    const uint32_t block_align = voice.channels * (voice.bits_per_sample / 8);
    if (played_bytes)
        *played_bytes = static_cast<uint64_t>(state.SamplesPlayed) * block_align;
    if (queued_buffers) *queued_buffers = state.BuffersQueued;
    return 1;
}

extern "C" int recomp_audio_output_submit(
    uint32_t slot, const uint8_t *pcm, uint32_t bytes,
    uint32_t sample_rate, uint32_t channels, uint32_t bits_per_sample,
    int32_t volume_hundredth_db)
{
    recomp_audio_output_initialize();
    if (!engine || bytes == 0) return 0;
    if (slot >= kVoiceCount || !pcm || bytes > kMaxBufferBytes ||
        sample_rate < XAUDIO2_MIN_SAMPLE_RATE ||
        sample_rate > XAUDIO2_MAX_SAMPLE_RATE ||
        (channels != 1 && channels != 2) ||
        (bits_per_sample != 8 && bits_per_sample != 16)) {
        dropBuffer(slot, "invalid-pcm");
        return 0;
    }
    const uint32_t block_align = channels * (bits_per_sample / 8);
    if (bytes % block_align != 0) {
        dropBuffer(slot, "partial-frame");
        return 0;
    }

    OutputVoice &voice = voices[slot];
    if (voice.source && (voice.sample_rate != sample_rate ||
        voice.channels != channels || voice.bits_per_sample != bits_per_sample)) {
        destroyVoice(voice);
    }
    if (!voice.source) {
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = static_cast<WORD>(channels);
        format.nSamplesPerSec = sample_rate;
        format.nAvgBytesPerSec = sample_rate * block_align;
        format.nBlockAlign = static_cast<WORD>(block_align);
        format.wBitsPerSample = static_cast<WORD>(bits_per_sample);
        /* Not XAUDIO2_VOICE_NOPITCH: a slot's mix may set a frequency ratio
           (pitch, Doppler), and changing the rate must not mean a new voice. */
        /* USEFILTER: a slot's mix may carry the voice's DLS2 filter. The sends
           are set straight after (applySends), before anything plays. */
        HRESULT error = engine->CreateSourceVoice(
            &voice.source, &format, XAUDIO2_VOICE_USEFILTER, kMaxFrequencyRatio);
        if (FAILED(error)) {
            dropBuffer(slot, "create-voice");
            disableOutput("CreateSourceVoice", error);
            return 0;
        }
        voice.sample_rate = sample_rate;
        voice.channels = channels;
        voice.bits_per_sample = bits_per_sample;
        voice.route_applied = 0;
        applySends(voice.source, channels, 0);
        error = voice.source->Start();
        if (FAILED(error)) {
            dropBuffer(slot, "start-voice");
            disableOutput("Start", error);
            return 0;
        }
    }

    if (volume_hundredth_db < -10000) volume_hundredth_db = -10000;
    if (volume_hundredth_db > 0) volume_hundredth_db = 0;
    const float gain = volume_hundredth_db == -10000 ? 0.0f :
        std::pow(10.0f, volume_hundredth_db / 2000.0f);
    HRESULT error = voice.source->SetVolume(gain);
    if (FAILED(error)) {
        dropBuffer(slot, "volume");
        disableOutput("SetVolume", error);
        return 0;
    }
    applyMix(slot, voice);
    pollMeter();
    XAUDIO2_VOICE_STATE state{};
    voice.source->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    /* BuffersQueued includes the active buffer; only retire the FIFO prefix. */
    while (voice.queued > state.BuffersQueued) {
        voice.front = (voice.front + 1) % kQueueSize;
        --voice.queued;
    }
    if (voice.fed && state.BuffersQueued == 0) {
        ++voice.starved;
        ++starved_total;
    }
    {
        const unsigned long long now = GetTickCount64();
        if (starved_total && now - last_starve_report >= 5000) {
            last_starve_report = now;
            std::fprintf(stderr, "[audio-output] ran dry:");
            for (uint32_t i = 0; i < kVoiceCount; ++i)
                if (voices[i].starved) {
                    std::fprintf(stderr, " slot %u x%u", i, voices[i].starved);
                    voices[i].starved = 0;
                }
            std::fprintf(stderr, " (in the last 5 s)\n");
            std::fflush(stderr);
            starved_total = 0;
        }
    }
    if (voice.queued == kQueueSize) {
        dropBuffer(slot, "queue-full");
        return 0;
    }
    PcmBuffer &owned = voice.buffers[(voice.front + voice.queued) % kQueueSize];
    if (owned.capacity < bytes) {
        auto *data = static_cast<uint8_t *>(std::realloc(owned.data, bytes));
        if (!data) {
            dropBuffer(slot, "allocation");
            return 0;
        }
        owned.data = data;
        owned.capacity = bytes;
    }
    std::memcpy(owned.data, pcm, bytes);
    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = bytes;
    buffer.pAudioData = owned.data;
    error = voice.source->SubmitSourceBuffer(&buffer);
    if (FAILED(error)) {
        dropBuffer(slot, "submit");
        disableOutput("SubmitSourceBuffer", error);
        return 0;
    }
    ++voice.queued;
    voice.fed = true;
    capture(slot, pcm, bytes, sample_rate, channels, bits_per_sample);
    ++submitted_buffers;
    submitted_bytes += bytes;
    bool nonzero = false;
    const uint8_t silence = bits_per_sample == 8 ? 0x80 : 0;
    for (uint32_t i = 0; i < bytes && !nonzero; ++i)
        nonzero = owned.data[i] != silence;
    if (nonzero) ++nonzero_buffers;
    if (submitted_buffers == 1 || (nonzero && !reported_nonzero[slot])) {
        std::fprintf(stderr,
            "[audio-output] submitted slot=%u bytes=%u rate=%u channels=%u "
            "bits=%u nonzero=%u gain=%.6f\n",
            slot, bytes, sample_rate, channels, bits_per_sample,
            nonzero ? 1u : 0u, static_cast<double>(gain));
    }
    if (nonzero) reported_nonzero[slot] = true;
    return 1;
}
