/*
 * audio_output_sdl.c -- SDL3 output for replaced DirectSound buffers, the
 * host side of audio_output.h on every platform but Windows.
 *
 * The same contract and the same switches as audio_output_xaudio2.cpp, which
 * is the reference; where it says something below, this does the same:
 *
 *   - RECOMP_AUDIO_GAIN (0..1, absent = 1): the host master gain. 0 opens no
 *     device at all, so nothing reports a play position and the streams fall
 *     back to their own clocks. An invalid value also opens none.
 *   - RECOMP_MUTE=1: the device opens and plays at gain 0, so the play
 *     position is the device's own, exactly as an audible run's would be.
 *   - Each slot (0..255 buffers, 256..271 streams, 272 a movie) is its own
 *     voice, here an SDL_AudioStream bound to one device; SDL mixes them.
 *   - At most kQueueSize submissions in flight per voice. A full queue
 *     refuses (returns 0) and the stream offers the chunk again, never steps
 *     over it (CLAUDE.md, "Audio de-sync was dropped sound").
 *   - recomp_audio_output_position is the device's clock: bytes the device
 *     has pulled from the voice since it was created, and the submissions
 *     not yet wholly pulled.
 *
 * SDL's stream does its own conversion and resampling to the device format,
 * so a voice is just a source format, a gain, and a FIFO of the submission
 * sizes (the stream holds the bytes).
 */
#include "audio_output.h"

#include <SDL3/SDL.h>

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VOICE_COUNT     (256u + 16u + 1u)
#define QUEUE_SIZE      12u          /* audio_output_xaudio2.cpp kQueueSize */
#define MAX_BUFFER      160000u
#define MIN_RATE        1000u
#define MAX_RATE        200000u
#define RESAMPLER_SLACK_FRAMES 8u

typedef struct {
    SDL_AudioStream *stream;
    uint32_t sample_rate, channels, bits_per_sample;
    uint64_t submitted;              /* bytes put into the stream, in the caller's format */
    uint64_t chunk_end[QUEUE_SIZE];  /* FIFO: cumulative end of each submission */
    uint32_t front, queued;
    /* The stream takes 5.1 float frames made from the caller's PCM with the
     * slot's mix applied (recomp_audio_output_set_mix), so the bytes SDL
     * holds are MIX_FRAME_BYTES per caller frame. */
    int      mixed;
    uint32_t mix_applied;
} voice;

/* recomp_audio_output_set_mix, per slot (see the XAudio2 file). */
typedef struct {
    int      set;
    float    levels[2][RECOMP_SPEAKERS];
    float    ratio;
    uint32_t generation;
} slot_mix;

#define MIX_FRAME_BYTES (RECOMP_SPEAKERS * 4u)
#define MAX_RATIO       8.0f

static pthread_mutex_t   g_lock = PTHREAD_MUTEX_INITIALIZER;
static SDL_AudioDeviceID g_device;
static voice             g_voices[VOICE_COUNT];
static slot_mix          g_mixes[VOICE_COUNT];
static int               g_reported_nonzero[VOICE_COUNT];
static int               g_attempted, g_summary_printed;
static unsigned long long g_submitted_buffers, g_submitted_bytes, g_nonzero_buffers;
static unsigned long long g_dropped_buffers;

static void drop_buffer(uint32_t slot, const char *reason)
{
    ++g_dropped_buffers;
    if (g_dropped_buffers <= 4 || (g_dropped_buffers & (g_dropped_buffers - 1)) == 0)
        fprintf(stderr, "[audio-output] dropped=%llu slot=%u reason=%s\n",
                g_dropped_buffers, slot, reason);
}

static void destroy_voice(voice *v)
{
    if (v->stream)
        SDL_DestroyAudioStream(v->stream);     /* unbinds it from the device */
    memset(v, 0, sizeof *v);
}

/* Bytes the device has taken from the voice. The stream reports what is
 * still waiting in the source format, so the rest has been pulled. */
static uint64_t voice_played(voice *v)
{
    int queued_bytes = v->stream ? SDL_GetAudioStreamQueued(v->stream) : 0;
    uint32_t frame = v->channels * (v->bits_per_sample / 8);
    uint64_t waiting;

    if (queued_bytes < 0)
        queued_bytes = 0;
    /* In the caller's bytes: a mixed stream holds 5.1 float frames. */
    waiting = v->mixed ? (uint64_t)queued_bytes / MIX_FRAME_BYTES * frame
                       : (uint64_t)queued_bytes;
    /* SDL's resampler keeps its last few input frames (at most 7,
     * RESAMPLER_MAX_PADDING_FRAMES) until more arrive, so a voice that has
     * run dry never reaches zero and its last submission would never
     * complete. XAudio2 plays those out; count them as played. */
    if (waiting <= RESAMPLER_SLACK_FRAMES * frame)
        waiting = 0;
    return waiting >= v->submitted ? 0 : v->submitted - waiting;
}

/* The caller's PCM as 5.1 float frames with the slot's mix applied. */
static float *mix_pcm(const slot_mix *m, const uint8_t *pcm, uint32_t bytes,
                      uint32_t channels, uint32_t bits, uint32_t *out_bytes)
{
    static float *buf;
    static uint32_t cap;
    uint32_t frame = channels * (bits / 8), frames = bytes / frame, f, c, s;
    uint32_t need = frames * MIX_FRAME_BYTES;

    if (need > cap) {
        float *grown = (float *)realloc(buf, need);
        if (!grown)
            return NULL;
        buf = grown;
        cap = need;
    }
    for (f = 0; f < frames; f++) {
        float *o = buf + (size_t)f * RECOMP_SPEAKERS;
        for (s = 0; s < RECOMP_SPEAKERS; s++)
            o[s] = 0.0f;
        for (c = 0; c < channels && c < 2u; c++) {
            float x = bits == 8u ? ((float)pcm[f * frame + c] - 128.0f) / 128.0f
                                 : (float)(int16_t)(pcm[f * frame + c * 2u] |
                                                    pcm[f * frame + c * 2u + 1u] << 8) / 32768.0f;
            for (s = 0; s < RECOMP_SPEAKERS; s++)
                o[s] += x * m->levels[c][s];
        }
    }
    *out_bytes = need;
    return buf;
}

static void retire_played(voice *v)
{
    uint64_t played = voice_played(v);

    while (v->queued && v->chunk_end[v->front] <= played) {
        v->front = (v->front + 1) % QUEUE_SIZE;
        v->queued--;
    }
}

/* Opens the device, on a host thread of its own (open_device_thread).
 *
 * Not on the caller's: the first submission comes from a guest thread inside
 * an HLE call, which holds the guest lock, and CoreAudio can take seconds to
 * open a device (15 s, then failure, has been seen) -- every other guest
 * thread and the vblank ISR would wait that long. Until the device is up,
 * submissions are refused (a stream offers them again; audio_output.h) and
 * there is no play position, so the streams run on their own clock, exactly
 * as before the first voice exists. */
static float g_open_gain;

static void *open_device_thread(void *arg)
{
    SDL_AudioDeviceID dev;
    float gain = g_open_gain;
    const char *mute = getenv("RECOMP_MUTE");
    int muted = mute && *mute && strcmp(mute, "0") != 0;

    (void)arg;
    /* SDL's audio brings up its event subsystem, which would otherwise turn
     * SIGINT/SIGTERM into a quit event nobody reads (src/host/host_sdl.c). */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        fprintf(stderr, "[audio-output] disabled operation=SDL_InitSubSystem error=%s\n",
                SDL_GetError());
        return NULL;
    }
    dev = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL);
    if (!dev) {
        fprintf(stderr, "[audio-output] disabled operation=SDL_OpenAudioDevice error=%s\n",
                SDL_GetError());
        return NULL;
    }
    /* RECOMP_MUTE: silent, but everything else as usual -- the device runs
     * and pulls the voices at its own rate, so their play positions are the
     * ones an audible run would see (see the XAudio2 file). The gain is set
     * before the device is published, so nothing is ever bound to it unmuted,
     * and a device that cannot be muted is not used at all. */
    if (muted) {
        gain = 0.0f;
        fprintf(stderr, "[audio-output] RECOMP_MUTE: playing silently\n");
    }
    if (!SDL_SetAudioDeviceGain(dev, gain)) {
        fprintf(stderr, "[audio-output] disabled operation=SetAudioDeviceGain error=%s\n",
                SDL_GetError());
        SDL_CloseAudioDevice(dev);
        return NULL;
    }
    {
        SDL_AudioSpec spec;
        int frames = 0;

        SDL_zero(spec);
        SDL_GetAudioDeviceFormat(dev, &spec, &frames);
        fprintf(stderr, "[audio-output] initialized backend=sdl3_%s master_gain=%.6f "
                "device=%d Hz %d ch, %d-frame buffer\n", SDL_GetCurrentAudioDriver(),
                (double)gain, spec.freq, spec.channels, frames);
    }
    {
        /* RECOMP_AUDIO_OPEN_DELAY_MS: publish the device that much later, so
         * a test can watch streams cross from their own clock to the
         * device's, as they do when a slow CoreAudio opens mid-run. */
        const char *delay = getenv("RECOMP_AUDIO_OPEN_DELAY_MS");
        if (delay && *delay && atoi(delay) > 0)
            SDL_Delay((Uint32)atoi(delay));
    }
    pthread_mutex_lock(&g_lock);
    if (g_summary_printed) {                 /* shut down while this was opening */
        pthread_mutex_unlock(&g_lock);
        SDL_CloseAudioDevice(dev);
        return NULL;
    }
    __atomic_store_n(&g_device, dev, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

void recomp_audio_output_initialize(void)
{
    double gain = 1.0;
    const char *setting;
    pthread_t thread;
    pthread_attr_t attr;

    pthread_mutex_lock(&g_lock);
    if (g_attempted) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    g_attempted = 1;
    setting = getenv("RECOMP_AUDIO_GAIN");
    if (setting) {
        char *end;

        gain = strtod(setting, &end);
        if (end == setting || *end != '\0' || !isfinite(gain) || gain < 0.0 || gain > 1.0) {
            fprintf(stderr, "[audio-output] muted invalid RECOMP_AUDIO_GAIN (expected 0..1)\n");
            pthread_mutex_unlock(&g_lock);
            return;
        }
    }
    if (gain == 0.0) {
        fprintf(stderr, "[audio-output] muted RECOMP_AUDIO_GAIN=0\n");
        pthread_mutex_unlock(&g_lock);
        return;
    }
    g_open_gain = (float)gain;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attr, open_device_thread, NULL) != 0)
        fprintf(stderr, "[audio-output] disabled operation=pthread_create\n");
    pthread_attr_destroy(&attr);
    pthread_mutex_unlock(&g_lock);
}

void recomp_audio_output_shutdown(void)
{
    uint32_t i;

    pthread_mutex_lock(&g_lock);
    if (g_summary_printed) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    for (i = 0; i < VOICE_COUNT; i++)
        destroy_voice(&g_voices[i]);
    if (g_device)
        SDL_CloseAudioDevice(g_device);
    g_device = 0;
    g_attempted = 1;
    g_summary_printed = 1;
    fprintf(stderr, "[audio-output] summary submitted_buffers=%llu submitted_bytes=%llu "
            "nonzero_buffers=%llu dropped_buffers=%llu\n", g_submitted_buffers,
            g_submitted_bytes, g_nonzero_buffers, g_dropped_buffers);
    pthread_mutex_unlock(&g_lock);
}

void recomp_audio_output_reset_voice(uint32_t slot)
{
    if (slot >= VOICE_COUNT)
        return;
    pthread_mutex_lock(&g_lock);
    destroy_voice(&g_voices[slot]);
    pthread_mutex_unlock(&g_lock);
}

void recomp_audio_output_set_mix(uint32_t slot, const float levels[2][RECOMP_SPEAKERS],
                                 float ratio)
{
    slot_mix *m;

    if (slot >= VOICE_COUNT || !levels)
        return;
    pthread_mutex_lock(&g_lock);
    m = &g_mixes[slot];
    if (!m->set || m->ratio != ratio || memcmp(m->levels, levels, sizeof m->levels) != 0) {
        memcpy(m->levels, levels, sizeof m->levels);
        m->ratio = ratio;
        m->set = 1;
        if (++m->generation == 0)
            m->generation = 1;
    }
    pthread_mutex_unlock(&g_lock);
}

void recomp_audio_output_clear_mix(uint32_t slot)
{
    if (slot >= VOICE_COUNT)
        return;
    pthread_mutex_lock(&g_lock);
    g_mixes[slot].set = 0;
    pthread_mutex_unlock(&g_lock);
}

int recomp_audio_output_position(uint32_t slot, uint64_t *played_bytes,
                                 uint32_t *queued_buffers)
{
    voice *v;

    if (slot >= VOICE_COUNT)
        return 0;
    pthread_mutex_lock(&g_lock);
    v = &g_voices[slot];
    if (!g_device || !v->stream) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    retire_played(v);
    if (played_bytes)
        *played_bytes = voice_played(v);
    if (queued_buffers)
        *queued_buffers = v->queued;
    pthread_mutex_unlock(&g_lock);
    return 1;
}

int recomp_audio_output_submit(uint32_t slot, const uint8_t *pcm, uint32_t bytes,
                               uint32_t sample_rate, uint32_t channels,
                               uint32_t bits_per_sample, int32_t volume_hundredth_db)
{
    uint32_t block_align, i;
    voice *v;
    float gain;
    int nonzero = 0;
    const uint8_t silence = bits_per_sample == 8 ? 0x80 : 0;

    recomp_audio_output_initialize();
    /* Read outside the lock: g_device is published once, by the opening
     * thread, with release order; 0 until then (refused, offered again). */
    if (!__atomic_load_n(&g_device, __ATOMIC_ACQUIRE) || bytes == 0)
        return 0;
    pthread_mutex_lock(&g_lock);
    if (slot >= VOICE_COUNT || !pcm || bytes > MAX_BUFFER ||
        sample_rate < MIN_RATE || sample_rate > MAX_RATE ||
        (channels != 1 && channels != 2) ||
        (bits_per_sample != 8 && bits_per_sample != 16)) {
        drop_buffer(slot, "invalid-pcm");
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    block_align = channels * (bits_per_sample / 8);
    if (bytes % block_align != 0) {
        drop_buffer(slot, "partial-frame");
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    v = &g_voices[slot];
    if (v->stream && (v->sample_rate != sample_rate || v->channels != channels ||
                      v->bits_per_sample != bits_per_sample ||
                      v->mixed != g_mixes[slot].set))
        destroy_voice(v);
    if (!v->stream) {
        SDL_AudioSpec src;

        src.format = bits_per_sample == 8 ? SDL_AUDIO_U8 : SDL_AUDIO_S16LE;
        src.channels = (int)channels;
        src.freq = (int)sample_rate;
        if (g_mixes[slot].set) {
            src.format = SDL_AUDIO_F32;
            src.channels = RECOMP_SPEAKERS;   /* SDL's 5.1 order is the Xbox's */
        }
        v->stream = SDL_CreateAudioStream(&src, NULL);
        if (!v->stream || !SDL_BindAudioStream(g_device, v->stream)) {
            fprintf(stderr, "[audio-output] voice %u: %s\n", slot, SDL_GetError());
            destroy_voice(v);
            drop_buffer(slot, "create-voice");
            pthread_mutex_unlock(&g_lock);
            return 0;
        }
        v->sample_rate = sample_rate;
        v->channels = channels;
        v->bits_per_sample = bits_per_sample;
        v->mixed = g_mixes[slot].set;
    }
    if (v->mixed && v->mix_applied != g_mixes[slot].generation) {
        float ratio = g_mixes[slot].ratio;
        if (!(ratio >= 1.0f / MAX_RATIO)) ratio = 1.0f / MAX_RATIO;
        if (ratio > MAX_RATIO) ratio = MAX_RATIO;
        SDL_SetAudioStreamFrequencyRatio(v->stream, ratio);
        v->mix_applied = g_mixes[slot].generation;
    }

    if (volume_hundredth_db < -10000)
        volume_hundredth_db = -10000;
    if (volume_hundredth_db > 0)
        volume_hundredth_db = 0;
    gain = volume_hundredth_db == -10000 ? 0.0f : powf(10.0f, (float)volume_hundredth_db / 2000.0f);
    SDL_SetAudioStreamGain(v->stream, gain);

    retire_played(v);
    if (v->queued == QUEUE_SIZE) {
        drop_buffer(slot, "queue-full");
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    if (v->mixed) {
        uint32_t out_bytes = 0;
        const float *frames = mix_pcm(&g_mixes[slot], pcm, bytes, channels,
                                      bits_per_sample, &out_bytes);
        if (!frames || !SDL_PutAudioStreamData(v->stream, frames, (int)out_bytes)) {
            drop_buffer(slot, "submit");
            pthread_mutex_unlock(&g_lock);
            return 0;
        }
    } else if (!SDL_PutAudioStreamData(v->stream, pcm, (int)bytes)) {
        drop_buffer(slot, "submit");
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    v->submitted += bytes;
    v->chunk_end[(v->front + v->queued) % QUEUE_SIZE] = v->submitted;
    v->queued++;
    ++g_submitted_buffers;
    g_submitted_bytes += bytes;
    for (i = 0; i < bytes && !nonzero; ++i)
        nonzero = pcm[i] != silence;
    if (nonzero)
        ++g_nonzero_buffers;
    if (g_submitted_buffers == 1 || (nonzero && !g_reported_nonzero[slot]))
        fprintf(stderr, "[audio-output] submitted slot=%u bytes=%u rate=%u channels=%u "
                "bits=%u nonzero=%u gain=%.6f\n", slot, bytes, sample_rate, channels,
                bits_per_sample, nonzero ? 1u : 0u, (double)gain);
    if (nonzero)
        g_reported_nonzero[slot] = 1;
    pthread_mutex_unlock(&g_lock);
    return 1;
}
