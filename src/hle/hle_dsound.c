/*
 * hle_dsound.c -- DirectSound buffers replaced by name.
 *
 * Adapted from doaxbv-re (https://github.com/NoRain211/doaxbv-re,
 * recomp-runtime/dsound_service_adapter.c), GPL-3.0.
 *
 * The game's own DirectSound drives an emulated MCPX audio chip, and every
 * piece of chip behaviour it relies on has to be found before it works: the
 * codec-ready bit, the DSP doorbells, starting the chip, and then its
 * interrupt, whose handler still declined it. Burnout 2 stops a sound before
 * its attract movie and waits for IDirectSoundBuffer_GetStatus to stop saying
 * PLAYING -- 4.5 million polls, forever.
 *
 * doaxbv-re's answer, reused here: replace the buffer functions and keep each
 * buffer's play state (playing, cursor, loop, frequency) in a model on the
 * host clock (dsound_buffer_model.c), sending the samples to XAudio2
 * (audio_output_xaudio2.cpp). GetStatus then never waits on hardware.
 *
 * Differences from doaxbv-re:
 *  - keyed by XDK function name through tools.xdk_symbols, not by one title's
 *    addresses, so any title on a covered XDK gets it;
 *  - the public IDirectSoundBuffer_* entry points are replaced; creation,
 *    SetVolume and the rest still run the game's own code, which keeps the
 *    settings object this reads current. SetFormat and SetBufferData are
 *    replaced too, but only for a buffer whose settings this file can read
 *    (settings_in_layout); any other buffer gets the game's own code;
 *  - a buffer whose format cannot be played still gets a clock, so status and
 *    position always advance: doaxbv-re fell back to the original function,
 *    which here is the path that hangs;
 *  - a lock, since nothing guarantees one guest thread.
 *
 * Where the fields live (XDK 5344, Burnout 2). The interface pointer the game
 * holds is the buffer object + 0x1C, and the object keeps two settings
 * pointers: +0x1C for the buffer (data, size, play and loop regions -- read
 * by CDirectSoundBufferSettings_SetBufferData, SetLoopRegion and the
 * play-region helper) and +0x10 for the voice (format, rate, alignment,
 * volume -- written by the format packer and CDirectSoundVoice_SetVolume).
 * doaxbv-re reads all of it through the first, which is only right if both
 * point at one object; each field is read here from the one the game's own
 * code writes it to. The offsets inside match doaxbv-re's.
 */
/* The NT type vocabulary: <windows.h> on Windows, the POSIX primitives
 * (critical sections, GetTickCount64) elsewhere. */
#include "platform/xbox_winnt.h"
#include <stdio.h>
#include <string.h>

#include "hle.h"
#include "platform/host_timer.h"
#include "dsound_buffer_model.h"
#include "xbox_adpcm.h"
#include "audio_output.h"
#include "hle_dsound_3d.h"
#include <math.h>

void hle_dsound_stream_tick(uint64_t now);   /* hle_dsound_stream.c */

/* Reads a RECOMP_* switch; see xbox_memory_layout.h. Declared here rather
 * than including the kernel header, which this file has no other need of. */
int xbox_EnvSwitch(const char *name, int default_on);

enum {
    SET_FORMAT     = 0x0Cu,   /* tag | channels << 16 | bits << 24 */
    SET_RATE       = 0x10u,
    SET_ALIGN      = 0x14u,
    SET_VOLUME     = 0x1Cu,   /* hundredths of a dB, headroom applied */
    /* The buffer settings as DSOUND 5344 and earlier lay them out; later
     * builds put them 4 bytes on (buf_shift below). */
    BUF_DATA       = 0xB8u,
    BUF_SIZE       = 0xBCu,
    BUF_PLAY_START = 0xC0u,
    BUF_PLAY_LEN   = 0xC4u,
    BUF_LOOP_START = 0xC8u,
    BUF_LOOP_LEN   = 0xCCu,

    TAG_PCM   = 0x0001u,
    TAG_ADPCM = 0x0069u,
    SLOTS     = 256u,

    PRE_ROLL_MS       = 50u,     /* silence ahead of a buffer's sound; see prime() */
    SERVICE_PERIOD_US = 5000,    /* the service thread's cadence; see service_thread() */
};

typedef struct Buffer {
    uint32_t iface;              /* the IDirectSoundBuffer the game holds */
    uint32_t data, size, format;
    int      output;             /* samples we can send to the host */
    /* IDirectSoundBuffer_Pause: the model is stopped with its cursor kept,
     * and paused_flags holds the play flags to resume with. */
    int      paused;
    uint32_t paused_flags;
    RecompDsoundBufferModel model;        /* what the game is told */
    RecompDsoundBufferModel output_model; /* what has been sent */
    int      primed;             /* the host voice has had its pre-roll */
    /* Since the last five-second report: the longest the output went
     * without a pump, the sound thrown away for it, and how often the host
     * voice ran dry. */
    uint32_t gap_max_ms, discarded_ms, discards, dry;
    uint64_t report_ms;
    uint64_t cursor_trace_ms;    /* RECOMP_DSOUND_CURSOR_TRACE */
    /* Where it is heard (hle_dsound_3d.c): the rate the title asked for
     * (SetFrequency/SetPitch; 0 = its own), and whether the slot has a mix,
     * in which case it is submitted at its own rate and plays at
     * model.sample_rate through the mix's ratio. The model's rate is the
     * asked-for one times the Doppler shift. */
    uint32_t asked_rate;
    int mixed;
} Buffer;

static Buffer g_buffers[SLOTS];
static CRITICAL_SECTION g_lock;
static INIT_ONCE g_lock_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_lock(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_lock);
    return TRUE;
}

static void lock(void)
{
    InitOnceExecuteOnce(&g_lock_once, init_lock, NULL, NULL);
    EnterCriticalSection(&g_lock);
}

static void unlock(void) { LeaveCriticalSection(&g_lock); }

/* Where the buffer settings (data, size, play and loop regions) sit moved by
 * 4 bytes between DSOUND 5344 and 5455: the XDK's own
 * CDirectSoundBufferSettings_SetBufferData keeps the data pointer at +0xB8
 * up to 5344 and at +0xBC from 5455 on. Read from that function in all 25
 * titles in games/ (Oct 2026): 3925 has none, 4134-5344 +0xB8, 5455-5849
 * +0xBC, with no exception. Read with the old offsets, a 5849 buffer's size
 * was a pointer: MK Shaolin Monks' 64 KB movie-sound ring was clocked as
 * 22.7 MB, so its play cursor ran on past the ring and wrapped 118 s later
 * instead of every 0.34 s, and the movie, paced by that cursor, froze where
 * it wrapped. The build comes from the XBE's own library table, which the
 * loader copies to 0x00010000 with the rest of the image header. */
static uint32_t dsound_build(void)
{
    static uint32_t build = 0xFFFFFFFFu;
    uint32_t n, table, i;

    if (build != 0xFFFFFFFFu)
        return build;
    build = 0u;
    if (HLE_MEM32(0x00010000u) != 0x48454258u) {     /* "XBEH" */
        fprintf(stderr, "[DSOUND] no XBE header at 0x00010000 (%08X, %u libraries at %08X)\n",
                HLE_MEM32(0x00010000u), HLE_MEM32(0x00010160u), HLE_MEM32(0x00010164u));
        return build;
    }
    n = HLE_MEM32(0x00010160u);
    table = HLE_MEM32(0x00010164u);
    for (i = 0; i < n && i < 64u; i++) {
        uint32_t e = table + i * 16u;
        if (e < 0x00010000u || e + 16u > 0x00020000u)
            break;
        if (HLE_MEM32(e) == 0x554F5344u && HLE_MEM32(e + 4u) == 0x0000444Eu) {  /* "DSOUND\0\0" */
            build = HLE_MEM32(e + 12u) & 0xFFFFu;
            break;
        }
    }
    fprintf(stderr, "[DSOUND] library build %u: buffer settings at +0x%02X\n",
            build, BUF_DATA + (build >= 5455u ? 4u : 0u));
    return build;
}

static uint32_t buf_shift(void)
{
    static int shift = -1;
    if (shift < 0)
        shift = dsound_build() >= 5455u ? 4 : 0;
    return (uint32_t)shift;
}

#define SET_DATA       (BUF_DATA + buf_shift())
#define SET_SIZE       (BUF_SIZE + buf_shift())
#define SET_PLAY_START (BUF_PLAY_START + buf_shift())
#define SET_PLAY_LEN   (BUF_PLAY_LEN + buf_shift())
#define SET_LOOP_START (BUF_LOOP_START + buf_shift())
#define SET_LOOP_LEN   (BUF_LOOP_LEN + buf_shift())

/* Buffer settings: [iface] = [object + 0x1C]. Voice settings: [object + 0x10],
 * i.e. [iface - 0x0C]. */
static int voice_field(uint32_t offset)
{
    return offset == SET_FORMAT || offset == SET_RATE ||
           offset == SET_ALIGN || offset == SET_VOLUME;
}

/* A guest address worth dereferencing: title RAM, or the contiguous window
 * where MmAllocateContiguousMemory memory lives (xbox_memory_layout.c). */
static int guest_readable(uint32_t va, uint32_t bytes)
{
    if (va >= 0x00010000u && (uint64_t)va + bytes <= g_xbox_total_ram)
        return 1;
    return va >= 0x80000000u && (uint64_t)va + bytes <= 0x80000000ull + 0x04000000ull;
}

/* 0 when either pointer on the way is not guest memory. Buffer Release is not
 * replaced, so a buffer the title freed keeps its slot here until a change is
 * noticed, and its settings pointers then hold whatever reused that memory.
 * Burnout 2 crashed reading one (guest 0xFFFFD9AC) from StopEx while loading a
 * race. A 0 reads as "the buffer changed", and pump() and model_for() retire
 * the slot. */
static uint32_t setting(uint32_t iface, uint32_t offset)
{
    uint32_t holder = voice_field(offset) ? iface - 0x0Cu : iface;
    uint32_t object;

    if (!guest_readable(holder, 4u))
        return 0u;
    object = HLE_MEM32(holder);
    if (!guest_readable(object + offset, 4u))
        return 0u;
    return HLE_MEM32(object + offset);
}

static uint32_t slot_of(const Buffer *b) { return (uint32_t)(b - g_buffers); }

/* Drop what the host has queued for this buffer; the next submission starts
 * a new voice, behind a fresh pre-roll. */
static void reset_output(Buffer *b)
{
    recomp_audio_output_reset_voice(slot_of(b));
    b->primed = 0;
}

static void retire(Buffer *b)
{
    recomp_audio_output_reset_voice(slot_of(b));
    recomp_audio_output_clear_mix(slot_of(b));
    memset(b, 0, sizeof *b);
}

/* The pre-roll. pump() sends each slice of the buffer as the clock passes
 * it -- not before, because a title streaming into a ring rewrites what is
 * ahead of the cursor right up to the moment it is played -- so without a
 * cushion the host voice holds at most one slice, and any pump that comes
 * late leaves it dry: a gap in the sound. Bink's movie ring in Max Payne ran
 * dry several times every five seconds. This much silence first, each time
 * the voice starts or has run dry, plays everything that much later and
 * absorbs a late pump up to the same length. */
static void prime(Buffer *b, uint32_t rate, uint32_t channels, uint32_t bits,
                  int32_t volume)
{
    static uint8_t silence[PRE_ROLL_MS * 200 * 2 * 2];   /* 200 kHz, stereo, 16 bit */
    uint32_t frame = channels * bits / 8u;
    uint32_t bytes = rate / 1000u * PRE_ROLL_MS * frame;
    uint32_t queued = 0u;

    if (b->primed) {
        uint64_t played;
        if (!recomp_audio_output_position(slot_of(b), &played, &queued) || queued != 0u)
            return;
        b->dry++;
    }
    if (bytes == 0u || bytes > sizeof silence)
        return;
    memset(silence, bits == 8u ? 0x80 : 0x00, bytes);
    /* The buffer's own volume: the voice has one gain, set at each submission. */
    if (recomp_audio_output_submit(slot_of(b), silence, bytes, rate, channels, bits, volume))
        b->primed = 1;
}

/* Send the samples the output clock has consumed since the last pump. */
static void pump(Buffer *b, uint64_t now)
{
    RecompDsoundBufferModel *out = &b->output_model;
    uint32_t channels, bits, offset = 0u, bytes;
    int adpcm;
    static uint8_t pcm[80000];

    if (!b->output || !out->playing || now <= out->last_ms ||
        now - out->last_ms < 10u)
        return;
    if (b->data != setting(b->iface, SET_DATA) ||
        b->size != setting(b->iface, SET_SIZE) ||
        b->format != setting(b->iface, SET_FORMAT)) {
        retire(b);
        return;
    }
    channels = (b->format >> 16) & 0xFFu;
    bits = b->format >> 24;
    adpcm = (b->format & 0xFFFFu) == TAG_ADPCM;
    /* What was lost, reported when anything was: a pump more than 100 ms
     * after the last one is thrown away (recomp_dsound_buffer_consume), and
     * a voice that ran dry was re-primed. */
    {
        uint32_t gap = (uint32_t)(now - out->last_ms);
        if (gap > b->gap_max_ms) b->gap_max_ms = gap;
        if (gap > 100u) { b->discarded_ms += gap; b->discards++; }
        if (!b->report_ms) b->report_ms = now;
        if (now - b->report_ms >= 5000u) {
            if (b->discards || b->dry) {
                fprintf(stderr, "[DSOUND] buffer %08X (slot %u, %u bytes), last %u ms: "
                        "longest gap between pumps %u ms, %u ms discarded in %u gaps, "
                        "ran dry %u times\n", b->iface, slot_of(b), out->size_bytes,
                        (uint32_t)(now - b->report_ms), b->gap_max_ms, b->discarded_ms,
                        b->discards, b->dry);
                fflush(stderr);
            }
            b->gap_max_ms = b->discarded_ms = b->discards = b->dry = 0u;
            b->report_ms = now;
        }
    }
    bytes = recomp_dsound_buffer_consume(out, now, &offset);
    if (bytes == 0u || b->data == 0u || bytes > sizeof pcm)
        return;

    if (adpcm) {
        uint32_t block_bytes = XBOX_ADPCM_BLOCK_BYTES * channels;
        uint32_t decoded_bytes = XBOX_ADPCM_BLOCK_SAMPLES * channels * 2u;
        uint32_t written;
        for (written = 0u; written < bytes;) {
            int16_t decoded[XBOX_ADPCM_BLOCK_SAMPLES * 2];
            uint32_t within = offset % decoded_bytes;
            uint32_t count = decoded_bytes - within;
            if (count > bytes - written) count = bytes - written;
            if (!xbox_adpcm_decode_block(
                    (const uint8_t *)HLE_PTR(b->data + offset / decoded_bytes * block_bytes),
                    block_bytes, channels, decoded, XBOX_ADPCM_BLOCK_SAMPLES * 2u)) {
                b->output = 0;          /* bad data: keep the clock, stop the sound */
                reset_output(b);
                return;
            }
            memcpy(pcm + written, (uint8_t *)decoded + within, count);
            written += count;
            offset += count;
            if (offset == out->size_bytes) offset = out->loop_start_bytes;
        }
        bits = 16u;
    } else {
        uint32_t written;
        for (written = 0u; written < bytes;) {
            uint32_t count = out->size_bytes - offset;
            if (count > bytes - written) count = bytes - written;
            memcpy(pcm + written, HLE_PTR(b->data + offset), count);
            written += count;
            offset += count;
            if (offset == out->size_bytes) offset = out->loop_start_bytes;
        }
    }
    {
        int32_t volume = (int32_t)setting(b->iface, SET_VOLUME);
        float levels[2][RECOMP_SPEAKERS], doppler = 1.0f;
        uint32_t rate = out->sample_rate;

        if (hle_ds3d_mix(b->iface, channels, volume, levels, &doppler)) {
            /* Submitted at its own rate; the mix's ratio plays it at the
             * model's, so a rate change never restarts the host voice. */
            rate = out->original_sample_rate;
            recomp_audio_output_set_mix(slot_of(b), (const float (*)[RECOMP_SPEAKERS])levels,
                                        (float)out->sample_rate / (float)rate);
            b->mixed = 1;
        } else if (b->mixed) {
            recomp_audio_output_clear_mix(slot_of(b));
            b->mixed = 0;
        }
        prime(b, rate, channels, bits, volume);
        recomp_audio_output_submit(slot_of(b), pcm, bytes, rate, channels, bits, volume);
        /* The Doppler shift changes how fast the buffer is consumed, as it
         * does on the console -- from now: after this slice was taken, or
         * the output clock would step over it. */
        if (b->mixed) {
            uint32_t asked = b->asked_rate ? b->asked_rate : b->model.original_sample_rate;
            uint32_t want = (uint32_t)lrintf((float)asked * doppler);
            if (want < 1000u) want = 1000u;
            if (want > 200000u) want = 200000u;
            if (want != b->model.sample_rate) {
                (void)recomp_dsound_buffer_set_frequency(&b->model, want, now);
                (void)recomp_dsound_buffer_set_frequency(&b->output_model, want, now);
            }
        }
    }
}

static Buffer *find(uint32_t iface)
{
    uint32_t i;
    if (iface == 0u) return NULL;
    for (i = 0; i < SLOTS; i++)
        if (g_buffers[i].iface == iface)
            return &g_buffers[i];
    return NULL;
}

/* The buffer's model, created or refreshed from the settings object. NULL when
 * nothing about it can be clocked (no rate or alignment worth trusting).
 *
 * `known` is the buffer if the caller already has it, else NULL to look it up.
 * DirectSoundDoWork walks all 256 slots and used to pass only the iface, so
 * every active buffer cost another full scan to find the slot the loop was
 * already standing on -- 256 slots squared, every call, on a function the
 * title calls far more often than once a frame. It was 22.7% of Dino Crisis
 * 3's on-CPU time as a leaf, which is the shape of a scan rather than of the
 * work it was meant to be doing. */
static Buffer *model_for_known(Buffer *known, uint32_t iface, uint64_t now)
{
    Buffer *b = known ? known : find(iface);
    uint32_t size, data, format, tag, channels, bits, align, rate, loop_start;
    uint32_t decoded_size, decoded_loop, block_align;
    int output;
    uint32_t i;

    if (iface == 0u || !guest_readable(iface - 0x0Cu, 0x10u) ||
        HLE_MEM32(iface) == 0u || HLE_MEM32(iface - 0x0Cu) == 0u)
        return NULL;
    size       = setting(iface, SET_SIZE);
    data       = setting(iface, SET_DATA);
    format     = setting(iface, SET_FORMAT);
    align      = setting(iface, SET_ALIGN);
    rate       = setting(iface, SET_RATE);
    loop_start = setting(iface, SET_LOOP_START);
    tag        = format & 0xFFFFu;
    channels   = (format >> 16) & 0xFFu;
    bits       = format >> 24;

    if (b && (b->data != data || b->size != size || b->format != format)) {
        retire(b);
        b = NULL;
    }
    if (align == 0u || rate < 1000u || rate > 200000u)
        return NULL;
    /* Clock whole blocks only. Burnout 2's attract-movie stream is a stereo
     * ADPCM ring of 7,876,608 bytes -- not a multiple of its 72-byte block --
     * and requiring an exact multiple left the one buffer the game waits on
     * with no model and no sound. The tail is less than one block. b->size
     * below keeps the raw size, so a real change is still noticed. */
    {
        uint32_t raw_size = size;
        size -= size % align;
        if (size == 0u)
            return NULL;
        if (b) {
            b->size = raw_size;
        }
        if (loop_start >= size)
            loop_start = 0u;
        (void)raw_size;
    }
    if (loop_start >= size || loop_start % align != 0u)
        loop_start = 0u;

    output = channels >= 1u && channels <= 2u &&
        ((tag == TAG_PCM && (bits == 8u || bits == 16u) &&
          align == channels * bits / 8u) ||
         (tag == TAG_ADPCM && bits == 4u &&
          align == XBOX_ADPCM_BLOCK_BYTES * channels));

    /* ADPCM is clocked in decoded PCM bytes, as the output consumes it. */
    if (tag == TAG_ADPCM && output) {
        uint64_t d = (uint64_t)(size / align) * XBOX_ADPCM_BLOCK_SAMPLES * channels * 2u;
        if (d > UINT32_MAX) return NULL;
        decoded_size = (uint32_t)d;
        decoded_loop = loop_start / align * XBOX_ADPCM_BLOCK_SAMPLES * channels * 2u;
        block_align = channels * 2u;
    } else {
        decoded_size = size;
        decoded_loop = loop_start;
        block_align = align;
    }

    if (b) {
        if (b->model.loop_start_bytes != decoded_loop) {
            pump(b, now);
            recomp_dsound_buffer_cursor(&b->model, now);
            reset_output(b);
            b->model.loop_start_bytes = decoded_loop;
            b->output_model = b->model;
        }
        return b;
    }
    for (i = 0; i < SLOTS; i++) {
        if (g_buffers[i].iface == 0u) {
            b = &g_buffers[i];
            if (recomp_dsound_buffer_configure(&b->model, decoded_size, rate,
                                               block_align, now) != 0u)
                return NULL;
            b->iface = iface;
            b->data = data;
            b->size = size;
            b->format = format;
            b->output = output;
            b->model.loop_start_bytes = decoded_loop;
            b->output_model = b->model;
            return b;
        }
    }
    {
        static int said;
        if (!said++) {
            fprintf(stderr, "[DSOUND] more than %u buffers in play; "
                            "the rest are not modelled\n", SLOTS);
            fflush(stderr);
        }
    }
    return NULL;
}

static Buffer *model_for(uint32_t iface, uint64_t now)
{
    return model_for_known(NULL, iface, now);
}

static uint64_t now_ms(void) { return GetTickCount64(); }

/* After a control change, restart the output from the model unless this is a
 * Play that simply continues a sound already playing. */
static void resync_output(Buffer *b, int continuing)
{
    if (!continuing)
        reset_output(b);
    if (continuing)
        b->output_model.play_flags = b->model.play_flags;
    else
        b->output_model = b->model;
}

/* ── The service thread ──────────────────────────────────────────
 *
 * On the console the audio hardware plays a buffer and consumes a stream's
 * packets by itself; the title only refills them. Here nothing reached the
 * host unless the title happened to call in: a buffer was pumped from inside
 * GetCurrentPosition, GetStatus, Play and DirectSoundDoWork, a stream ticked
 * from DoWork, Process and GetStatus. A title whose audio code pauses -- for a
 * disc read, a page of a graphic novel loading -- left the host voice dry for
 * the length of the pause, and a buffer pump more than 100 ms late threw that
 * stretch of sound away. Max Payne's Bink sound thread polls its 0.6 s movie
 * ring every 20 ms or so but stops for 140-300 ms at a time: up to 640 ms of
 * every five seconds was dropped, the rest crackled.
 *
 * This thread is the hardware's half: every SERVICE_PERIOD_US it pumps each
 * playing buffer and services the streams, whatever the title is doing. The
 * title-driven pumps stay, and they matter: the one inside GetCurrentPosition
 * reads the ring up to the cursor before the title is told the cursor and
 * starts rewriting behind it. It runs no guest code -- it has no guest stack
 * -- so a stream packet whose completion is a guest callback is still
 * completed on a guest thread (hle_dsound_stream.c).
 *
 * RECOMP_DSOUND_SERVICE=0 switches it off, for comparing with the old
 * behaviour. */
void hle_dsound_stream_service(uint64_t now);   /* hle_dsound_stream.c */

static DWORD WINAPI service_thread(LPVOID param)
{
    host_timer *timer = host_timer_create(HOST_TIMER_ANY);
    int64_t next = host_time_ns();

    (void)param;
    for (;;) {
        uint64_t now = now_ms();
        uint32_t i;

        lock();
        for (i = 0; i < SLOTS; i++) {
            Buffer *b = &g_buffers[i];
            if (b->iface && b->output && b->output_model.playing)
                pump(b, now);
        }
        unlock();
        hle_dsound_stream_service(now);

        next += (int64_t)SERVICE_PERIOD_US * 1000;
        if (host_time_ns() - next > 100000000)   /* 100 ms behind: do not catch up */
            next = host_time_ns();
        if (!timer || host_timer_wait_until(timer, next, 50u) == HOST_WAIT_NOT_ARMED)
            Sleep(SERVICE_PERIOD_US / 1000);
    }
    return 0;
}

static BOOL CALLBACK start_service(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    HANDLE thread;
    (void)once; (void)param; (void)ctx;

    if (!xbox_EnvSwitch("RECOMP_DSOUND_SERVICE", 1)) {
        fprintf(stderr, "[DSOUND] RECOMP_DSOUND_SERVICE=0: sound is fed only when "
                        "the title calls DirectSound\n");
        return TRUE;
    }
    thread = CreateThread(NULL, 0, service_thread, NULL, 0, NULL);
    if (!thread) {
        fprintf(stderr, "[DSOUND] could not start the service thread; sound is fed "
                        "only when the title calls DirectSound\n");
        return TRUE;
    }
    CloseHandle(thread);
    fprintf(stderr, "[DSOUND] service thread feeds buffers and streams every %d ms\n",
            SERVICE_PERIOD_US / 1000);
    fflush(stderr);
    return TRUE;
}

/* Started by the first buffer played or stream created. */
void hle_dsound_service_start(void)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, start_service, NULL, NULL);
}

/* HRESULT IDirectSoundBuffer_Play(this, reserved1, reserved2, flags) */
/* How often the title calls each of these.
 *
 * DirectSoundDoWork alone turned out to be 68.8 million calls in 85 seconds
 * of Dino Crisis 3 -- some 62,000 per presented frame against a console
 * budget of one -- and a count on its own does not say what the title is
 * waiting for. The ratio does: a poll loop shows up as one other entry point
 * keeping pace with DoWork, and everything else near zero.
 *
 * Reported on powers of ten of the DoWork count, so this costs a handful of
 * lines across a whole run. */
extern uint32_t g_xbox_code_lo;
extern uint32_t g_xbox_code_hi;

enum {
    DS_CALL_BUF_PLAY,
    DS_CALL_BUF_STOP,
    DS_CALL_BUF_STOPEX,
    DS_CALL_BUF_PAUSE,
    DS_CALL_BUF_GETSTATUS,
    DS_CALL_BUF_GETCURRENTPOSITION,
    DS_CALL_BUF_SETCURRENTPOSITION,
    DS_CALL_BUF_SETFREQUENCY,
    DS_CALL_BUF_SETFORMAT,
    DS_CALL_BUF_SETBUFFERDATA,
    DS_CALL_DIRECTSOUNDDOWORK,
    DS_CALL_COUNT
};
static uint64_t g_ds_calls[DS_CALL_COUNT];
static const char *const g_ds_call_names[DS_CALL_COUNT] = {
    "IDirectSoundBuffer_Play",
    "IDirectSoundBuffer_Stop",
    "IDirectSoundBuffer_StopEx",
    "IDirectSoundBuffer_Pause/PauseEx",
    "IDirectSoundBuffer_GetStatus",
    "IDirectSoundBuffer_GetCurrentPosition",
    "IDirectSoundBuffer_SetCurrentPosition",
    "IDirectSoundBuffer_SetFrequency",
    "IDirectSoundBuffer_SetFormat",
    "IDirectSoundBuffer_SetBufferData",
    "DirectSoundDoWork",
};

/* `caller` is the guest return address, which is the thing that actually
 * identifies the loop. DirectSoundDoWork at 810,000 calls a second is not
 * paired with any other entry point here -- GetCurrentPosition, the next
 * busiest, runs at an eighth of it -- so the counts alone say only that
 * something outside this file is calling it, and not what. */
static void ds_count_report(uint32_t caller)
{
    int i;
    fprintf(stderr, "[DSOUND] calls so far (DoWork from 0x%08X):", caller);
    for (i = 0; i < DS_CALL_COUNT; i++)
        if (g_ds_calls[i])
            fprintf(stderr, " %s=%llu", g_ds_call_names[i],
                    (unsigned long long)g_ds_calls[i]);
    fprintf(stderr, "\n");
    /* And the chain above it. The immediate caller is a game tick that
     * happens to include this call; what matters is the loop spinning on
     * that tick 58,000 times per presented frame. Same heuristic the ICALL
     * logger uses: code-range values above esp, first one most reliable. */
    {
        const uint8_t *stk = (const uint8_t *)g_xbox_mem_offset + g_esp;
        int k, shown = 0;
        fprintf(stderr, "[DSOUND]   guest stack:");
        for (k = 0; g_esp && k < 200 && shown < 8; k++) {
            uint32_t v;
            memcpy(&v, stk + (size_t)k * 4, sizeof v);
            if (v >= g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "%s 0x%08X", shown ? " <-" : "", v);
                shown++;
            }
        }
        if (!shown)
            fprintf(stderr, " (none)");
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

HLE_EXPORT(IDirectSoundBuffer_Play)
{
    g_ds_calls[DS_CALL_BUF_PLAY]++;
    uint32_t iface = HLE_ARG(0), flags = HLE_ARG(3), result;
    uint64_t now = now_ms();
    static unsigned said;
    Buffer *b;
    int continuing;

    hle_dsound_service_start();
    lock();
    b = model_for(iface, now);
    if (said < 16) {
        said++;
        fprintf(stderr, "[DSOUND] Play buffer=%08X format=%08X bytes=%u rate=%u "
                        "flags=%u %s\n", iface,
                iface && HLE_MEM32(iface) ? setting(iface, SET_FORMAT) : 0u,
                iface && HLE_MEM32(iface) ? setting(iface, SET_SIZE) : 0u,
                iface && HLE_MEM32(iface) ? setting(iface, SET_RATE) : 0u, flags,
                !b ? "not modelled" : b->output ? "output" : "clock only");
        /* RECOMP_DSOUND_DUMP=1 -- the settings object, as words.
         *
         * The offsets above are one XDK's layout. When a title reports a rate
         * of 29433088 and a format of 0x01C10D60 -- both of which are guest
         * pointers, not a rate and a packed format -- the table is being read
         * against a different layout, and the only way to say which is to look
         * at what is actually there. Tony Hawk's Pro Skater 2X (XDK 3947) is
         * the first title here to disagree with it. */
        if (!b && xbox_EnvSwitch("RECOMP_DSOUND_DUMP", 0)) {
            uint32_t object = (iface && guest_readable(iface, 4u))
                            ? HLE_MEM32(iface) : 0u;
            uint32_t voice  = (iface >= 0x0Cu && guest_readable(iface - 0x0Cu, 4u))
                            ? HLE_MEM32(iface - 0x0Cu) : 0u;
            unsigned w;
            fprintf(stderr, "[DSOUND]   object=0x%08X voice=0x%08X\n",
                    object, voice);
            for (w = 0; w < 0x34u; w += 4u) {
                if (object && guest_readable(object + w, 4u))
                    fprintf(stderr, "[DSOUND]     object+%02X = %08X\n",
                            w, HLE_MEM32(object + w));
            }
            for (w = 0; w < 0x20u; w += 4u) {
                if (voice && guest_readable(voice + w, 4u))
                    fprintf(stderr, "[DSOUND]     voice +%02X = %08X\n",
                            w, HLE_MEM32(voice + w));
            }
            /* Follow whatever sits where the format and rate are expected.
             * If this build keeps a WAVEFORMATEX pointer there, its first
             * words read as tag | channels << 16, then samples per second. */
            for (w = 0x0Cu; w <= 0x10u; w += 4u) {
                uint32_t p = (voice && guest_readable(voice + w, 4u))
                           ? HLE_MEM32(voice + w) : 0u;
                unsigned k;
                if (!p || !guest_readable(p, 0x18u))
                    continue;
                fprintf(stderr, "[DSOUND]     *(voice+%02X)=0x%08X:", w, p);
                for (k = 0; k < 0x18u; k += 4u)
                    fprintf(stderr, " %08X", HLE_MEM32(p + k));
                fprintf(stderr, "\n");
            }
        }
        fflush(stderr);
    }
    if (!b) {
        unlock();
        HLE_RETURN(iface ? RECOMP_DSOUND_OK : RECOMP_DSOUND_POINTER_ERROR);
    }
    pump(b, now);
    recomp_dsound_buffer_cursor(&b->model, now);
    continuing = b->model.playing && b->output_model.playing &&
                 !(flags & RECOMP_DSOUND_PLAY_FROMSTART);
    result = recomp_dsound_buffer_play(&b->model, flags, now);
    if (result == RECOMP_DSOUND_OK) {
        b->paused = 0;
        resync_output(b, continuing);
    }
    unlock();
    HLE_RETURN(result);
}

static void stop(uint32_t iface)
{
    uint64_t now = now_ms();
    Buffer *b;

    lock();
    b = find(iface);
    if (b) {
        pump(b, now);
        recomp_dsound_buffer_stop(&b->model, now);
        b->paused = 0;
        resync_output(b, 0);
    }
    unlock();
}

/* HRESULT IDirectSoundBuffer_Stop(this) */
HLE_EXPORT(IDirectSoundBuffer_Stop)
{
    g_ds_calls[DS_CALL_BUF_STOP]++;
    stop(HLE_ARG(0));
    HLE_RETURN(HLE_ARG(0) ? RECOMP_DSOUND_OK : RECOMP_DSOUND_POINTER_ERROR);
}

/* HRESULT IDirectSoundBuffer_StopEx(this, REFERENCE_TIME rtTimeStamp, flags).
 * The time stamp and envelope flags ask for a scheduled or released stop; an
 * immediate one is what the game can observe, and all it waits for. */
HLE_EXPORT(IDirectSoundBuffer_StopEx)
{
    g_ds_calls[DS_CALL_BUF_STOPEX]++;
    stop(HLE_ARG(0));
    HLE_RETURN(HLE_ARG(0) ? RECOMP_DSOUND_OK : RECOMP_DSOUND_POINTER_ERROR);
}

/* The C++ methods under the interface, which XACT calls directly.
 *
 * IDirectSoundBuffer_PlayEx is `CDirectSoundBuffer_PlayEx(pThis ? pThis - 0x1C
 * : 0, ...)`, and the rest of the interface wraps its method the same way. A
 * title that plays through XACT never goes through the interface: Forza's
 * engine and tyre sounds start with CDirectSoundBuffer_PlayEx, whose lifted
 * body drives CMcpxBuffer_Play against voice hardware nothing emulates and
 * polls the clock until it gives up -- once per voice, so the race ran at a
 * frame a second. These map `this` back to the interface the models are keyed
 * by and do what the interface replacements do. PlayEx's time stamp is two
 * words, so its flags sit where Play's do. */
#define DS_BUFFER_FROM_OBJECT(this_) ((this_) ? (this_) + 0x1Cu : 0u)

HLE_EXPORT(IDirectSoundBuffer_PlayEx)
{
    hle_IDirectSoundBuffer_Play();
}

HLE_EXPORT(CDirectSoundBuffer_Play)
{
    HLE_MEM32(g_esp + 4u) = DS_BUFFER_FROM_OBJECT(HLE_ARG(0));
    hle_IDirectSoundBuffer_Play();
}

HLE_EXPORT(CDirectSoundBuffer_PlayEx)
{
    HLE_MEM32(g_esp + 4u) = DS_BUFFER_FROM_OBJECT(HLE_ARG(0));
    hle_IDirectSoundBuffer_Play();
}

HLE_EXPORT(CDirectSoundBuffer_Stop)
{
    uint32_t iface = DS_BUFFER_FROM_OBJECT(HLE_ARG(0));
    g_ds_calls[DS_CALL_BUF_STOP]++;
    stop(iface);
    HLE_RETURN(iface ? RECOMP_DSOUND_OK : RECOMP_DSOUND_POINTER_ERROR);
}

HLE_EXPORT(CDirectSoundBuffer_StopEx)
{
    uint32_t iface = DS_BUFFER_FROM_OBJECT(HLE_ARG(0));
    g_ds_calls[DS_CALL_BUF_STOPEX]++;
    stop(iface);
    HLE_RETURN(iface ? RECOMP_DSOUND_OK : RECOMP_DSOUND_POINTER_ERROR);
}

/* DSBPAUSE_RESUME, DSBPAUSE_PAUSE, DSBPAUSE_SYNCHPLAYBACK. */
enum { DSBPAUSE_RESUME = 0u, DSBPAUSE_PAUSE = 1u, DSBPAUSE_SYNCHPLAYBACK = 2u };

/* Pause keeps the cursor and stops the clock; resume continues from it.
 *
 * Dino Crisis 3 (XDK 5558) fades its music out by setting the volume to
 * -64 dB, calling Pause(DSBPAUSE_PAUSE), then spinning on DirectSoundDoWork
 * until GetStatus stops saying PLAYING, and only then calling Stop. Pause was
 * not replaced, so the model never heard of it, GetStatus said PLAYING for
 * ever, and the title hung for good straight after its difficulty select.
 *
 * SYNCHPLAYBACK pauses too: it holds the buffer so that several can be
 * resumed together, which from the title's side looks the same. */
static uint32_t pause_buffer(uint32_t iface, uint32_t how)
{
    uint64_t now = now_ms();
    Buffer *b;

    if (iface == 0u)
        return RECOMP_DSOUND_POINTER_ERROR;
    if (how > DSBPAUSE_SYNCHPLAYBACK)
        return RECOMP_DSOUND_INVALID_PARAM;
    lock();
    b = find(iface);
    if (b) {
        pump(b, now);
        recomp_dsound_buffer_cursor(&b->model, now);
        if (how == DSBPAUSE_RESUME) {
            if (b->paused) {
                b->paused = 0;
                if (recomp_dsound_buffer_play(&b->model, b->paused_flags, now)
                        == RECOMP_DSOUND_OK)
                    resync_output(b, 0);
            }
        } else if (b->model.playing) {
            b->paused = 1;
            b->paused_flags = b->model.play_flags & RECOMP_DSOUND_PLAY_LOOPING;
            recomp_dsound_buffer_stop(&b->model, now);
            resync_output(b, 0);
        }
    }
    unlock();
    return RECOMP_DSOUND_OK;
}

/* HRESULT IDirectSoundBuffer_Pause(this, DWORD dwPause) */
HLE_EXPORT(IDirectSoundBuffer_Pause)
{
    g_ds_calls[DS_CALL_BUF_PAUSE]++;
    HLE_RETURN(pause_buffer(HLE_ARG(0), HLE_ARG(1)));
}

/* HRESULT IDirectSoundBuffer_PauseEx(this, REFERENCE_TIME rtTimestamp,
 * DWORD dwPause). A scheduled pause is taken now, as StopEx's stop is. */
HLE_EXPORT(IDirectSoundBuffer_PauseEx)
{
    g_ds_calls[DS_CALL_BUF_PAUSE]++;
    HLE_RETURN(pause_buffer(HLE_ARG(0), HLE_ARG(3)));
}

/* HRESULT IDirectSoundBuffer_GetStatus(this, DWORD *status)
 * DSBSTATUS_PLAYING 1, DSBSTATUS_PAUSED 2, DSBSTATUS_LOOPING 4. */
HLE_EXPORT(IDirectSoundBuffer_GetStatus)
{
    g_ds_calls[DS_CALL_BUF_GETSTATUS]++;
    uint32_t iface = HLE_ARG(0), out = HLE_ARG(1), status = 0u;
    uint64_t now = now_ms();
    Buffer *b;

    if (out == 0u)
        HLE_RETURN(RECOMP_DSOUND_POINTER_ERROR);
    lock();
    b = find(iface);
    if (b) {
        pump(b, now);
        recomp_dsound_buffer_cursor(&b->model, now);
        if (b->model.playing)
            status = 1u | ((b->model.play_flags & RECOMP_DSOUND_PLAY_LOOPING) ? 4u : 0u);
        else if (b->paused)
            status = 2u | ((b->paused_flags & RECOMP_DSOUND_PLAY_LOOPING) ? 4u : 0u);
    }
    unlock();
    HLE_MEM32(out) = status;          /* never played here: not playing */
    HLE_RETURN(RECOMP_DSOUND_OK);
}

/* HRESULT IDirectSoundBuffer_GetCurrentPosition(this, DWORD *play, DWORD *write) */
HLE_EXPORT(IDirectSoundBuffer_GetCurrentPosition)
{
    g_ds_calls[DS_CALL_BUF_GETCURRENTPOSITION]++;
    uint32_t iface = HLE_ARG(0), cursor = 0u, i;
    uint64_t now = now_ms();
    Buffer *b;

    lock();
    b = model_for(iface, now);
    if (b) {
        pump(b, now);
        cursor = recomp_dsound_buffer_cursor(&b->model, now);
        /* ADPCM is clocked in decoded bytes; the game asks in source bytes. */
        if ((b->format & 0xFFFFu) == TAG_ADPCM && b->output)
            cursor = cursor / (XBOX_ADPCM_BLOCK_SAMPLES * b->model.block_align) *
                     (XBOX_ADPCM_BLOCK_BYTES * b->model.block_align / 2u);
        /* RECOMP_DSOUND_CURSOR_TRACE=1: what a title polling for its clock
         * sees, once a second per buffer. A movie paced by its sound stops
         * dead when this stops moving. */
        {
            static int trace = -1;
            if (trace < 0)
                trace = xbox_EnvSwitch("RECOMP_DSOUND_CURSOR_TRACE", 0);
            if (trace && now - b->cursor_trace_ms >= 1000u) {
                b->cursor_trace_ms = now;
                fprintf(stderr, "[DSOUND] cursor %08X (slot %d): %u of %u, %s%s, %u Hz;"
                        " play region %u+%u, loop region %u+%u\n",
                        iface, slot_of(b), cursor, b->size,
                        b->model.playing ? "playing" : "stopped",
                        (b->model.play_flags & RECOMP_DSOUND_PLAY_LOOPING) ? " looping" : "",
                        b->model.sample_rate,
                        setting(iface, SET_PLAY_START), setting(iface, SET_PLAY_LEN),
                        setting(iface, SET_LOOP_START), setting(iface, SET_LOOP_LEN));
            }
        }
    }
    unlock();
    for (i = 1u; i <= 2u; i++) {
        uint32_t out = HLE_ARG(i);
        if (out != 0u)
            HLE_MEM32(out) = cursor;
    }
    HLE_RETURN(RECOMP_DSOUND_OK);
}

/* HRESULT IDirectSoundBuffer_SetCurrentPosition(this, DWORD position) */
HLE_EXPORT(IDirectSoundBuffer_SetCurrentPosition)
{
    g_ds_calls[DS_CALL_BUF_SETCURRENTPOSITION]++;
    uint32_t iface = HLE_ARG(0), position = HLE_ARG(1), result = RECOMP_DSOUND_OK;
    uint64_t now = now_ms();
    Buffer *b;

    lock();
    b = model_for(iface, now);
    if (b) {
        pump(b, now);
        if ((b->format & 0xFFFFu) == TAG_ADPCM && b->output) {
            uint32_t block = XBOX_ADPCM_BLOCK_BYTES * b->model.block_align / 2u;
            if (position >= b->size || position % block != 0u)
                result = RECOMP_DSOUND_INVALID_PARAM;
            else
                position = position / block * XBOX_ADPCM_BLOCK_SAMPLES *
                           b->model.block_align;
        }
        if (result == RECOMP_DSOUND_OK)
            result = recomp_dsound_buffer_set_position(&b->model, position, now);
        if (result == RECOMP_DSOUND_OK)
            resync_output(b, 0);
    }
    unlock();
    HLE_RETURN(result);
}

static uint32_t set_rate(uint32_t iface, uint32_t frequency);

/* HRESULT IDirectSoundBuffer_SetFrequency(this, DWORD frequency); 0 = default */
HLE_EXPORT(IDirectSoundBuffer_SetFrequency)
{
    g_ds_calls[DS_CALL_BUF_SETFREQUENCY]++;
    HLE_RETURN(set_rate(HLE_ARG(0), HLE_ARG(1)));
}

/* HRESULT IDirectSoundBuffer_SetPitch(this, LONG pitch): the same, in the
 * Xbox's pitch units (1/4096 octave from 48 kHz). MK Shaolin Monks calls it
 * from 14 places and SetFrequency from 2; until this was replaced, every one
 * of its pitch changes went unheard. The title's own body runs first, so its
 * settings stay as the XDK keeps them (the chip part is CMcpxVoiceClient_SetPitch,
 * replaced below). */
HLE_ORIGINAL(IDirectSoundBuffer_SetPitch);
HLE_EXPORT(IDirectSoundBuffer_SetPitch)
{
    uint32_t iface = HLE_ARG(0);
    int32_t pitch = (int32_t)HLE_ARG(1);

    if (hle_original_IDirectSoundBuffer_SetPitch)
        HLE_CALL_ORIGINAL(IDirectSoundBuffer_SetPitch);
    HLE_RETURN(set_rate(iface, hle_ds3d_pitch_to_frequency(pitch)));
}

static uint32_t set_rate(uint32_t iface, uint32_t frequency)
{
    uint32_t result = RECOMP_DSOUND_OK;
    uint64_t now = now_ms();
    Buffer *b;

    /* The output carries on from where it is at the new rate; it is not
     * restarted. Max Payne calls this several times for each menu sound it
     * plays, mostly with the rate it already has, and restarting the voice
     * each time -- dropping what was queued and starting again behind the
     * pre-roll -- left a 0.4 s cursor sound as a scatter of 2-30 ms pieces:
     * no menu sounds at all. */
    lock();
    b = model_for(iface, now);
    if (b) {
        uint32_t before = b->model.sample_rate;
        pump(b, now);
        b->asked_rate = frequency;
        result = recomp_dsound_buffer_set_frequency(&b->model, frequency, now);
        if (result == RECOMP_DSOUND_OK && b->model.sample_rate != before)
            (void)recomp_dsound_buffer_set_frequency(&b->output_model, frequency, now);
    }
    unlock();
    return result;
}

/* Whether the buffer's settings are in the layout this file reads: a rate
 * and an alignment worth trusting, the test model_for applies before it
 * models a buffer. Not every XDK keeps them there. Halo's DSOUND (XDK 3936)
 * has a guest pointer where the rate should be (its Play log line says
 * "rate=18895392 ... not modelled"), so none of its buffers is modelled --
 * and the two replacements below must not write this layout into its
 * objects either. They hand such a buffer to the game's own code, which is
 * what ran before they existed and what Halo needs: with the replacement,
 * its SetBufferData wrote 0xB8..0xCC of the wrong object and its Bink intro
 * movie waited on its audio for ever, 22 s in. */
static int settings_in_layout(uint32_t iface)
{
    uint32_t rate = setting(iface, SET_RATE), align = setting(iface, SET_ALIGN);

    return align != 0u && rate >= 1000u && rate <= 200000u;
}

/* The game's own body, for a buffer settings_in_layout() rejects. */
static void original_body(void (*const body)(void), const char *name)
{
    static unsigned said;

    if (!body) {
        fprintf(stderr, "[DSOUND] %s: the buffer is not in the layout this file "
                        "reads and the original body was not kept -- lift again\n", name);
        fflush(stderr);
        g_eax = 0x80004005u;            /* E_FAIL */
        return;
    }
    if (said < 4) {
        said++;
        fprintf(stderr, "[DSOUND] %s: buffer settings not in the layout this file "
                        "reads; the game's own code runs\n", name);
        fflush(stderr);
    }
    {
        uint32_t esp = g_esp;

        body();
        g_esp = esp;
    }
}

/* HRESULT IDirectSoundBuffer_SetFormat(this, LPCWAVEFORMATEX format)
 *
 * The game's own SetFormat ends in CMcpxBuffer_SetBufferData, which first
 * waits for the buffer's hardware voice to be released by the chip -- a voice
 * the replaced Play never put on the chip, so the release never comes.
 * Outrun 2 calls it at the start of every race and spun there for ever, its
 * 0.5 s retry re-arming the same command each time.
 *
 * So the format goes where the game's packer would put it, the voice
 * settings this file reads (tag | channels << 16 | bits << 24, rate, block
 * alignment), and the model notices the change and rebuilds -- for a buffer
 * in that layout (settings_in_layout). */
HLE_ORIGINAL(IDirectSoundBuffer_SetFormat);
HLE_EXPORT(IDirectSoundBuffer_SetFormat)
{
    g_ds_calls[DS_CALL_BUF_SETFORMAT]++;
    uint32_t iface = HLE_ARG(0), wfx = HLE_ARG(1), result = RECOMP_DSOUND_OK;
    uint64_t now = now_ms();
    uint32_t holder = iface - 0x0Cu, object;
    static unsigned said;

    if (!settings_in_layout(iface)) {
        original_body(hle_original_IDirectSoundBuffer_SetFormat,
                      "IDirectSoundBuffer_SetFormat");
        return;
    }
    if (!wfx || !guest_readable(wfx, 16u) || !guest_readable(holder, 4u)
            || !guest_readable(HLE_MEM32(holder) + SET_ALIGN, 4u))
        HLE_RETURN(RECOMP_DSOUND_INVALID_PARAM);
    object = HLE_MEM32(holder);
    {
        uint32_t tag      = HLE_MEM32(wfx) & 0xFFFFu;
        uint32_t channels = HLE_MEM32(wfx) >> 16;
        uint32_t rate     = HLE_MEM32(wfx + 4u);
        uint32_t align    = HLE_MEM32(wfx + 12u) & 0xFFFFu;
        uint32_t bits     = HLE_MEM32(wfx + 12u) >> 16;

        if (said < 8) {
            said++;
            fprintf(stderr, "[DSOUND] SetFormat buffer=%08X tag %04X, %u ch, %u Hz, "
                            "%u bit, align %u\n", iface, tag, channels, rate,
                    bits, align);
        }
        lock();
        HLE_MEM32(object + SET_FORMAT) = tag | (channels & 0xFFu) << 16 | (bits & 0xFFu) << 24;
        HLE_MEM32(object + SET_RATE) = rate;
        HLE_MEM32(object + SET_ALIGN) = align;
        if (model_for(iface, now))
            ;   /* rebuilt from the new settings */
        unlock();
    }
    HLE_RETURN(result);
}

/* HRESULT IDirectSoundBuffer_SetBufferData(this, LPVOID data, DWORD bytes)
 *
 * The game's own version stops the buffer through CMcpxBuffer_Stop_Ex, which
 * waits for the chip to release a hardware voice the replaced Play never
 * used -- the same wait as SetFormat above. Outrun 2 swaps a buffer's data
 * mid-race and lost about 40 seconds to that wait each time.
 *
 * So: stop the modelled buffer, as the replaced Stop does, and write what the
 * game's settings code would -- the data and size, and play and loop regions
 * back to the whole buffer (zero). The model rebuilds from them. Only for a
 * buffer in that layout (settings_in_layout). */
HLE_ORIGINAL(IDirectSoundBuffer_SetBufferData);
HLE_EXPORT(IDirectSoundBuffer_SetBufferData)
{
    g_ds_calls[DS_CALL_BUF_SETBUFFERDATA]++;
    uint32_t iface = HLE_ARG(0), data = HLE_ARG(1), bytes = HLE_ARG(2);
    uint64_t now = now_ms();
    uint32_t object;
    static unsigned said;

    if (!settings_in_layout(iface)) {
        original_body(hle_original_IDirectSoundBuffer_SetBufferData,
                      "IDirectSoundBuffer_SetBufferData");
        return;
    }
    if (!guest_readable(iface, 4u) || !guest_readable(HLE_MEM32(iface) + SET_LOOP_LEN, 4u))
        HLE_RETURN(RECOMP_DSOUND_INVALID_PARAM);
    if (said < 8) {
        said++;
        fprintf(stderr, "[DSOUND] SetBufferData buffer=%08X data 0x%08X, %u bytes\n",
                iface, data, bytes);
    }
    stop(iface);
    lock();
    object = HLE_MEM32(iface);
    HLE_MEM32(object + SET_DATA) = data;
    HLE_MEM32(object + SET_SIZE) = bytes;
    HLE_MEM32(object + SET_PLAY_START) = 0u;
    HLE_MEM32(object + SET_PLAY_LEN) = 0u;
    HLE_MEM32(object + SET_LOOP_START) = 0u;
    HLE_MEM32(object + SET_LOOP_LEN) = 0u;
    (void)model_for(iface, now);
    unlock();
    HLE_RETURN(RECOMP_DSOUND_OK);
}

/* void DirectSoundDoWork(void) -- the title's regular audio tick. The game's
 * own version services the emulated chip's deferred commands, which the
 * replaced buffers no longer need; here it feeds the host output. */
HLE_EXPORT(DirectSoundDoWork)
{
    g_ds_calls[DS_CALL_DIRECTSOUNDDOWORK]++;
    uint64_t now = now_ms();
    uint32_t i;
    static int said;

    if (!said) {
        said = 1;
        fprintf(stderr, "[DSOUND] DirectSoundDoWork replaced by name: buffers "
                        "run on the host clock and play through XAudio2\n");
        fflush(stderr);
    }
    /* How often the title asks. On the console this is a once-a-frame call;
     * anything wildly above the frame rate means the work below is being paid
     * for at a rate nobody chose, and that is worth seeing in the log rather
     * than inferring from a profile. */
    {
        /* Powers of ten, like the ICALL loggers, because a fixed interval
         * is a bug here: at 100,000 this printed 688 lines in 85 seconds
         * and NtWriteFile became 100% of the sampling profile -- the
         * counter measuring the problem became the problem. The
         * progression is the useful part anyway. The title calls this
         * 68.8 million times in 85 seconds, about 62,000 per presented
         * frame; on the console it is a once-a-frame call. */
        static uint64_t calls, next = 1;
        if (++calls >= next) {
            next *= 10;
            ds_count_report(HLE_MEM32(g_esp));
        }
    }
    /* At most one full pass per millisecond. A title that polls this in a
     * loop (Dino Crisis 3, above) paid for the lock, the walk of every slot
     * and a host play-position query per stream on each of its calls, and a
     * second pass inside the same millisecond finds nothing new: the models
     * run on now_ms() and pumps are already one per 10 ms. What a skipped
     * call can delay is a stream packet the host finished playing within
     * that millisecond, by under a millisecond. Keyed on the performance
     * counter, not now_ms(): GetTickCount64 moves in 15.6 ms steps on
     * Windows, which would skip whole frames' worth of calls. */
    {
        static LARGE_INTEGER freq;
        static uint64_t last_pass_ms = UINT64_MAX;
        LARGE_INTEGER c;
        uint64_t pass_ms;

        if (!freq.QuadPart)
            QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&c);
        pass_ms = freq.QuadPart >= 1000
                      ? (uint64_t)c.QuadPart / (uint64_t)(freq.QuadPart / 1000)
                      : now;
        if (pass_ms == last_pass_ms)
            HLE_RETURN(0);
        last_pass_ms = pass_ms;
    }
    lock();
    for (i = 0; i < SLOTS; i++) {
        if (g_buffers[i].iface != 0u) {
            Buffer *b = model_for_known(&g_buffers[i],
                                        g_buffers[i].iface, now);
            if (b)
                pump(b, now);
        }
    }
    unlock();
    hle_dsound_stream_tick(now);      /* streams: feed the host, complete packets */
    HLE_RETURN(0);
}

/* â”€â”€ The chip side of a voice's settings â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 *
 * CDirectSoundVoice_SetVolume, SetHeadroom, SetPitch, SetMixBins and
 * SetMixBinVolumes each write the voice settings object (the volume this
 * file reads at every submission, see SET_VOLUME) and then call one of
 * these three to program the MCPX voice: descriptor writes and MMIO register
 * writes, each register write an exception trap here, each trap waking the
 * APU thread. The chip voice is never started -- IDirectSoundBuffer_Play is
 * replaced above and keeps the sound in the host model -- so the programming
 * was pure cost. Outrun 2 sets volume, headroom, pitch and mix bins on every
 * voice every frame: CMcpxVoiceClient_SetVolume and SetMixBins were 15% of
 * the guest thread's time in a race, the exception dispatcher under them 6%
 * and the APU wake-ups 2% (1 Oct 2026, caller-attributed sampling).
 *
 * Replaced here, below the setters, so every setter still keeps its own
 * settings exactly as the XDK does and only the chip part is skipped. All
 * three are thiscall with no stack arguments and return S_OK.
 *
 * Pitch is therefore recorded by the XDK but not yet heard: nothing in this
 * file applies it, and the XAudio2 backend recreates a source voice when its
 * rate changes, which a per-frame engine pitch would do every frame. Hearing
 * it needs a frequency-ratio path in the backend first. It was not heard
 * before this change either. */
HLE_EXPORT(CMcpxVoiceClient_SetVolume)  { HLE_RETURN(0); }
HLE_EXPORT(CMcpxVoiceClient_SetMixBins) { HLE_RETURN(0); }
HLE_EXPORT(CMcpxVoiceClient_SetPitch)   { HLE_RETURN(0); }
