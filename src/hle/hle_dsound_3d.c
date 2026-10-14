/*
 * hle_dsound_3d.c -- where a replaced DirectSound voice is heard.
 *
 * hle_dsound.c plays every buffer and stream on the host with one gain, the
 * voice's volume. On the console a voice reaches up to eight mix bins, each
 * at its own level, and a 3D voice's levels and pitch follow its position,
 * cone and velocity relative to the listener. All of that was lost: every
 * sound came from the same place at the same loudness (Max Payne, Oct 2026),
 * and with no distance attenuation a busy scene summed far louder than it
 * ever did on the console.
 *
 * The XDK computes those levels in its own code, but in structures that move
 * between XDK builds (the voice client keeps its settings at +0x78 in 4134
 * and +0x70 in 5849, and the 3D results moved into a block of their own).
 * What does not move is the public API: IDirectSoundBuffer_SetPosition and
 * the rest take the same arguments on every XDK, and the XDK's 3D math runs
 * on the plain DS3DBUFFER and DS3DLISTENER records they fill in. So this file
 * wraps the public setters -- each runs the title's own code first, then the
 * arguments are recorded here -- and recomputes the levels the way the XDK
 * does. The formulas are the XDK's, read from its code in Max Payne (DSOUND
 * 4134) and MK Shaolin Monks (5849); the two agree, and 5849 adds rolloff
 * curves and mute-at-max-distance:
 *
 *   distance  inside min: 0. Beyond: d is clamped to max, then
 *             -2000 log10(1 + rolloff (d / min - 1)) hundredths of a dB, with
 *             rolloff = listener factor x buffer factor; or, with a rolloff
 *             curve of n points, the curve's linear gain at d (points at
 *             min + (i + 1)(max - min) / n, 1.0 at min, linear between).
 *             With DSBCAPS_MUTE3DATMAXDISTANCE, silent at max and past it.
 *   cone      the XDK's own angle estimate between the cone axis and the
 *             listener (it uses 45 t for atan t), then 0 inside the inner
 *             angle, the outside volume past the outer one, linear between.
 *   doppler   v = (vsource - vlistener) . unit(source - listener) x both
 *             Doppler factors x both distance factors; pitch is
 *             4096 log2(1 - v / 342) (+4096 at v <= -342, -32767 at
 *             v >= 342), i.e. a playback-rate ratio of 1 - v / 342.
 *   mix bins  each bin's attenuation is the voice volume plus the bin's own
 *             volume, plus, for a 3D voice: distance + cone on the speaker
 *             and effect-send bins, and on the crosstalk bins the front or
 *             rear level too. Past 0xFFF/64 dB of attenuation the hardware
 *             is silent; a positive level is no gain (the XDK's unsigned
 *             arithmetic turns one into full attenuation).
 *
 * Not reproduced, said here so nobody takes this for the console's output:
 *
 *   - The HRTF. A 3D voice's crosstalk bins (XTLK, 6-9) are filtered per ear
 *     on the console by an HRTF the voice processor runs; here the two ears
 *     are a constant-power pan from where the source is relative to the
 *     listener's right. Left/right placement is right; the console's head
 *     shadowing, interaural delay and front/back coloration are not there.
 *   - The rear crosstalk bins are silent, as the XDK makes them when the
 *     console is not in surround mode.
 *   - Reverb (I3DL2, bin 10) and the effect sends (FXSEND, 11+) feed the DSP's
 *     effects, which are not run. Those bins are left out of the mix; a voice
 *     heard only through them is played on the front speakers instead, so it
 *     is not silent.
 *   - SetFilter and SetEG (the voice processor's filter and envelopes).
 *
 * Deferred settings (DS3D_DEFERRED) wait for IDirectSound_CommitDeferredSettings
 * as on the console. RECOMP_DS3D=0 turns this file off (every voice back to
 * one gain); RECOMP_DS3D_TRACE=1 prints, once a second for each 3D voice,
 * what it computed.
 */
#include "platform/xbox_winnt.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "hle.h"
#include "hle_dsound_3d.h"

int xbox_EnvSwitch(const char *name, int default_on);

enum {
    DS3D_IMMEDIATE = 0u,
    DS3D_DEFERRED = 1u,

    DS3DMODE_NORMAL = 0u,
    DS3DMODE_HEADRELATIVE = 1u,
    DS3DMODE_DISABLE = 2u,

    CAPS_CTRL3D = 0x00000010u,
    CAPS_MIXIN = 0x00002000u,
    CAPS_MUTE3DATMAXDISTANCE = 0x00020000u,

    BIN_XTLK_FRONT_LEFT = 6u, BIN_XTLK_FRONT_RIGHT = 7u,
    BIN_XTLK_BACK_LEFT = 8u, BIN_XTLK_BACK_RIGHT = 9u,
    BIN_I3DL2 = 10u,
    BIN_COUNT = 32u,
    MAX_BINS = 8u,

    VOICES = 640u,             /* 256 buffers and the streams, with room to spare */
    ATTEN_SILENT = 6398,       /* 0xFFF / 64 dB in hundredths: the hardware's floor */
};

typedef struct Vec3 { float x, y, z; } Vec3;

/* DS3DBUFFER, with the Xbox's per-buffer distance, rolloff and Doppler
 * factors (+0x40..+0x48 in the XDK's own record). */
typedef struct Ds3dBuffer {
    Vec3 position, velocity;
    uint32_t inside_angle, outside_angle;
    Vec3 cone_orientation;
    int32_t cone_outside_volume;
    float min_distance, max_distance;
    uint32_t mode;
    float distance_factor, rolloff_factor, doppler_factor;
    uint32_t curve, curve_points;      /* guest FLOAT[], from SetRolloffCurve */
} Ds3dBuffer;

/* DS3DLISTENER. */
typedef struct Ds3dListener {
    Vec3 position, velocity, front, top;
    float distance_factor, rolloff_factor, doppler_factor;
} Ds3dListener;

typedef struct Voice {
    uint32_t key;                   /* the interface pointer the title holds */
    int is3d;                       /* created with DSBCAPS_CTRL3D, or given 3D */
    int mute_at_max;
    Ds3dBuffer now, pending;
    int pending_set;
    /* Mix bins in the order given (a stereo voice's even entries take the
     * left channel, odd the right), and each bin's volume. */
    uint32_t bin_count;
    uint8_t bins[MAX_BINS];
    int32_t bin_volume[BIN_COUNT];
    int bins_set;
    uint32_t frequency;             /* streams: SetFrequency/SetPitch, 0 = own rate */
    uint64_t used_ms;               /* last mixed: the table drops the stalest */
    uint64_t trace_ms;              /* RECOMP_DS3D_TRACE */
    /* DSI3DL2BUFFER (SetI3DL2Source): the direct path's level and the send
     * into the room, hundredths of a dB. */
    int32_t i3dl2_direct, i3dl2_room;
    int i3dl2_set;
    /* SetFilter: DSFILTERDESC's mode and its first two coefficients, which
     * the XDK writes to the voice's FC0 (cutoff) and FC1 (resonance). */
    uint32_t filter_mode;
    int32_t filter_fc;
    uint32_t filter_q;
    uint32_t output;                /* SetOutputBuffer: the mix-in buffer, 0 = speakers */
    int mixin;                      /* created with DSBCAPS_MIXIN */
    /* SetEG: DSENVELOPEDESC as given, for the amplitude envelope [0] and the
     * multi (pitch and filter) envelope [1]. */
    uint32_t eg[2][10];
    int eg_set[2];
} Voice;

static Voice g_voices[VOICES];
static Ds3dListener g_listener, g_listener_pending;
static int g_listener_pending_set;
/* DSI3DL2LISTENER (IDirectSound_SetI3DL2Listener), deferred like the rest. */
static RecompI3dl2 g_room, g_room_pending;
static int g_room_set, g_room_pending_set, g_room_dirty;
static CRITICAL_SECTION g_lock;
static INIT_ONCE g_lock_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_lock(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_lock);
    g_listener.front.z = 1.0f;
    g_listener.top.y = 1.0f;
    g_listener.distance_factor = g_listener.rolloff_factor = g_listener.doppler_factor = 1.0f;
    g_listener_pending = g_listener;
    return TRUE;
}

static void lock(void)
{
    InitOnceExecuteOnce(&g_lock_once, init_lock, NULL, NULL);
    EnterCriticalSection(&g_lock);
}

static void unlock(void) { LeaveCriticalSection(&g_lock); }

static int enabled(void)
{
    static int on = -1;
    if (on < 0)
        on = xbox_EnvSwitch("RECOMP_DS3D", 1);
    return on;
}

static int tracing(void)
{
    static int on = -1;
    if (on < 0)
        on = xbox_EnvSwitch("RECOMP_DS3D_TRACE", 0);
    return on;
}

static int guest_readable(uint32_t va, uint32_t bytes)
{
    if (va >= 0x00010000u && (uint64_t)va + bytes <= g_xbox_total_ram)
        return 1;
    return va >= 0x80000000u && (uint64_t)va + bytes <= 0x80000000ull + 0x04000000ull;
}

static float as_float(uint32_t bits)
{
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

static float arg_float(int i) { return as_float(HLE_ARG(i)); }

static Vec3 guest_vec(uint32_t va)
{
    Vec3 v;
    v.x = as_float(HLE_MEM32(va));
    v.y = as_float(HLE_MEM32(va + 4u));
    v.z = as_float(HLE_MEM32(va + 8u));
    return v;
}

static void default_buffer(Ds3dBuffer *b)
{
    memset(b, 0, sizeof *b);
    b->inside_angle = b->outside_angle = 360u;
    b->cone_orientation.z = 1.0f;
    b->min_distance = 1.0f;
    b->max_distance = 1000000000.0f;
    b->distance_factor = b->rolloff_factor = b->doppler_factor = 1.0f;
}

/* Where each key was last found: the pump looks every voice up several
 * times every 5 ms, and the table is 640 long. A hint, checked before use. */
static uint16_t g_hint[1024];

static Voice *find(uint32_t key)
{
    uint32_t i, h = (key >> 2) & 1023u;
    if (!key) return NULL;
    if (g_hint[h] < VOICES && g_voices[g_hint[h]].key == key)
        return &g_voices[g_hint[h]];
    for (i = 0; i < VOICES; i++)
        if (g_voices[i].key == key) {
            g_hint[h] = (uint16_t)i;
            return &g_voices[i];
        }
    return NULL;
}

/* The record for this voice, made if there is none. A full table drops the
 * one mixed longest ago -- it has gone quiet the longest. */
static Voice *get(uint32_t key)
{
    Voice *v = find(key), *oldest = NULL;
    uint32_t i;

    if (v || !key) return v;
    for (i = 0; i < VOICES; i++) {
        if (!g_voices[i].key) { v = &g_voices[i]; break; }
        if (!oldest || g_voices[i].used_ms < oldest->used_ms) oldest = &g_voices[i];
    }
    if (!v) v = oldest;
    memset(v, 0, sizeof *v);
    v->key = key;
    default_buffer(&v->now);
    v->pending = v->now;
    return v;
}

/* ── The XDK's arithmetic ─────────────────────────────────────── */

static Vec3 sub3(Vec3 a, Vec3 b) { Vec3 r = { a.x - b.x, a.y - b.y, a.z - b.z }; return r; }
static Vec3 add3(Vec3 a, Vec3 b) { Vec3 r = { a.x + b.x, a.y + b.y, a.z + b.z }; return r; }
static float dot3(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static float len3(Vec3 a) { return sqrtf(dot3(a, a)); }
static Vec3 cross3(Vec3 a, Vec3 b)
{
    Vec3 r = { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    return r;
}

/* A linear gain as hundredths of a dB: -10000 at or below 0, 0 at or above 1. */
static int32_t gain_to_atten(float gain)
{
    if (!(gain > 0.0f)) return -10000;
    if (gain >= 1.0f) return 0;
    return (int32_t)lrintf(2000.0f * log10f(gain));
}

/* Distance attenuation, hundredths of a dB (<= 0). */
static int32_t distance_atten(const Voice *v, const Ds3dBuffer *b, float distance)
{
    float rolloff = g_listener.rolloff_factor * b->rolloff_factor;
    float d = distance;

    if (!(b->min_distance < d))
        return 0;
    if (v->mute_at_max && !(d < b->max_distance))
        return -10000;
    if (d > b->max_distance)
        d = b->max_distance;
    if (b->curve && b->curve_points && guest_readable(b->curve, b->curve_points * 4u) &&
        b->max_distance > b->min_distance) {
        float per = (float)b->curve_points / (b->max_distance - b->min_distance);
        float at = (d - b->min_distance) * per;
        uint32_t n = (uint32_t)at, count = b->curve_points;
        float lo, hi;
        if (n >= count) n = count - 1u;
        if (n == 0u) {
            lo = 1.0f;
            hi = as_float(HLE_MEM32(b->curve));
        } else {
            lo = as_float(HLE_MEM32(b->curve + (n - 1u) * 4u));
            hi = as_float(HLE_MEM32(b->curve + n * 4u));
        }
        return gain_to_atten(lo + (at - (float)n) * (hi - lo));
    }
    {
        float value = (d / b->min_distance - 1.0f) * rolloff;
        if (!(value >= 0.0f))
            return 0;
        return (int32_t)lrintf(-2000.0f * log10f(1.0f + value));
    }
}

/* The XDK's estimate of the cone angle, in its own units: it compares
 * |axis - u| with |axis + u| (u the unit vector from listener to source) and
 * takes 45 t degrees for atan t, scaled by 4. Facing the listener is very
 * negative, side-on 180, facing away very large. */
static float cone_angle(Vec3 axis, Vec3 unit)
{
    float a = len3(sub3(axis, unit)), b = len3(add3(axis, unit));
    if (b > a)
        return a > 0.0f ? b / a * 45.0f * 4.0f : 1.0e30f;
    return b > 0.0f ? (90.0f - a / b * 45.0f) * 4.0f : -1.0e30f;
}

static int32_t cone_atten(const Ds3dBuffer *b, float angle)
{
    float inside = (float)b->inside_angle, outside = (float)b->outside_angle;
    uint32_t span;

    if (!(inside < angle))
        return 0;
    if (b->inside_angle < b->outside_angle && !(outside > angle))
        return b->cone_outside_volume;
    span = b->outside_angle - b->inside_angle;
    if (span < 1u) span = 1u;
    return (int32_t)lrintf((angle - inside) * (float)b->cone_outside_volume / (float)span);
}

/* Doppler, as a playback-rate ratio. */
static float doppler_ratio(const Ds3dBuffer *b, Vec3 unit)
{
    float v = dot3(sub3(b->velocity, g_listener.velocity), unit) *
              b->doppler_factor * g_listener.doppler_factor *
              b->distance_factor * g_listener.distance_factor;
    if (v == 0.0f) return 1.0f;
    if (!(v < 342.0f)) return powf(2.0f, -32767.0f / 4096.0f);
    if (!(v > -342.0f)) return 2.0f;
    return 1.0f - v / 342.0f;
}

/* ── The mix ──────────────────────────────────────────────────── */

static int speaker_of(uint32_t bin)
{
    switch (bin) {
    case 0: case 1: case 2: case 3: case 4: case 5: return (int)bin;
    case BIN_XTLK_FRONT_LEFT:  return RECOMP_SPEAKER_FRONT_LEFT;
    case BIN_XTLK_FRONT_RIGHT: return RECOMP_SPEAKER_FRONT_RIGHT;
    case BIN_XTLK_BACK_LEFT:   return RECOMP_SPEAKER_BACK_LEFT;
    case BIN_XTLK_BACK_RIGHT:  return RECOMP_SPEAKER_BACK_RIGHT;
    default: return -1;        /* I3DL2 and the effect sends: not run here */
    }
}

int hle_ds3d_mix(uint32_t key, uint32_t channels, int32_t voice_volume, int force,
                 RecompAudioMix *mix, uint32_t *route)
{
    static Voice none;              /* a voice nothing was said about */
    Voice *v;
    float (*levels)[RECOMP_SPEAKERS] = mix->levels;
    int reverb_heard = 0;
    uint8_t bins[MAX_BINS];
    int32_t vols[MAX_BINS];
    uint32_t count, k;
    int three_d, audible = 0;
    int32_t dist = 0, cone = 0, front = 0, rear = -10000;
    float ear_left = 1.0f, ear_right = 1.0f, ratio = 1.0f, distance = 0.0f;
    uint64_t now = GetTickCount64();

    if (!enabled() || channels < 1u || channels > 2u)
        return 0;
    lock();
    v = find(key);
    if (!v || (!v->is3d && !v->bins_set && !v->i3dl2_set && !v->filter_mode && !v->output &&
               !v->eg_set[1])) {
        if (!force) {
            unlock();
            return 0;
        }
        if (!v) {
            if (!none.now.min_distance) default_buffer(&none.now);
            v = &none;
        }
    }
    memset(mix, 0, sizeof *mix);
    *route = v->output;
    three_d = v->is3d && v->now.mode != DS3DMODE_DISABLE;

    if (three_d) {
        const Ds3dBuffer *b = &v->now;
        Vec3 rel = b->mode == DS3DMODE_HEADRELATIVE ? b->position
                                                    : sub3(b->position, g_listener.position);
        Vec3 unit = rel;
        distance = len3(rel);
        if (distance > 0.0f) {
            unit.x /= distance; unit.y /= distance; unit.z /= distance;
        }
        dist = distance_atten(v, b, distance);
        cone = cone_atten(b, cone_angle(b->cone_orientation, unit));
        ratio = doppler_ratio(b, unit);
        {
            /* Left/right, standing in for the HRTF: the source's component
             * along the listener's right (top x front, as the XDK takes it). */
            float pan = dot3(unit, cross3(g_listener.top, g_listener.front));
            float angle;
            if (pan > 1.0f) pan = 1.0f;
            if (pan < -1.0f) pan = -1.0f;
            angle = (pan + 1.0f) * 0.78539816f;          /* 0..pi/2 */
            ear_left = cosf(angle) * 1.41421356f;
            ear_right = sinf(angle) * 1.41421356f;
            if (ear_left > 1.0f) ear_left = 1.0f;
            if (ear_right > 1.0f) ear_right = 1.0f;
        }
    }

    if (v->bins_set) {
        count = v->bin_count;
        for (k = 0; k < count; k++) {
            bins[k] = v->bins[k];
            vols[k] = v->bin_volume[v->bins[k] % BIN_COUNT];
        }
    } else if (v->is3d) {
        static const uint8_t def3d[] = { BIN_XTLK_FRONT_LEFT, BIN_XTLK_FRONT_RIGHT,
                                         BIN_XTLK_BACK_LEFT, BIN_XTLK_BACK_RIGHT, BIN_I3DL2 };
        count = (uint32_t)sizeof def3d;
        for (k = 0; k < count; k++) { bins[k] = def3d[k]; vols[k] = 0; }
    } else {
        count = 2u;
        bins[0] = 0u; bins[1] = 1u; vols[0] = vols[1] = 0;
    }

    for (k = 0; k < count; k++) {
        uint32_t bin = bins[k], c = channels == 2u ? (k & 1u) : 0u;
        int speaker = speaker_of(bin);
        int32_t atten = vols[k] > 0 ? 0 : vols[k];
        float gain;

        if (bin == BIN_I3DL2) {
            /* The send into the room: the bin's level, the 3D distance and
             * cone (the XDK adds them to this bin as to the speakers), and
             * SetI3DL2Source's room level. */
            if (three_d) atten += dist + cone;
            if (v->i3dl2_set) atten += v->i3dl2_room > 0 ? 0 : v->i3dl2_room;
            if (voice_volume + atten > -ATTEN_SILENT) {
                mix->reverb[c] += powf(10.0f, (float)atten / 2000.0f);
                reverb_heard = 1;
            }
            continue;
        }
        if (speaker < 0)
            continue;
        if (v->i3dl2_set)               /* SetI3DL2Source's direct-path level */
            atten += v->i3dl2_direct > 0 ? 0 : v->i3dl2_direct;
        if (three_d) {
            atten += dist + cone;
            if (bin == BIN_XTLK_FRONT_LEFT || bin == BIN_XTLK_FRONT_RIGHT) atten += front;
            if (bin == BIN_XTLK_BACK_LEFT || bin == BIN_XTLK_BACK_RIGHT) atten += rear;
        }
        if (voice_volume + atten <= -ATTEN_SILENT) {
            audible = audible ? audible : -1;     /* routed, but silent at this level */
            continue;
        }
        gain = powf(10.0f, (float)atten / 2000.0f);
        if (three_d && bin >= BIN_XTLK_FRONT_LEFT && bin <= BIN_XTLK_BACK_RIGHT)
            gain *= (bin == BIN_XTLK_FRONT_LEFT || bin == BIN_XTLK_BACK_LEFT) ? ear_left : ear_right;
        levels[c][speaker] += gain;
        audible = 1;
    }
    if (!audible && !reverb_heard) {
        /* Heard only through effects this runtime does not run: play it dry on
         * the front speakers rather than not at all. */
        static int said;
        float gain = three_d ? powf(10.0f, (float)(dist + cone) / 2000.0f) : 1.0f;
        if (!said++)
            fprintf(stderr, "[DS3D] voice %08X is routed only to effect sends, "
                    "which are not run; playing it on the front speakers\n", key);
        for (k = 0; k < channels; k++) {
            levels[k][RECOMP_SPEAKER_FRONT_LEFT] = channels == 2u ? (k == 0 ? gain : 0.0f) : gain;
            levels[k][RECOMP_SPEAKER_FRONT_RIGHT] = channels == 2u ? (k == 1 ? gain : 0.0f) : gain;
        }
    }
    if (tracing() && now - v->trace_ms >= 1000u) {
        fprintf(stderr, "[DS3D] voice %08X %s mode %u at %.1f m: distance %d, cone %d "
                "(hundredths dB), ears %.2f/%.2f, doppler x%.3f, volume %d, %u bins:",
                key, three_d ? "3D" : "2D", v->now.mode, (double)distance, dist, cone,
                (double)ear_left, (double)ear_right, (double)ratio, voice_volume, count);
        for (k = 0; k < count; k++)
            fprintf(stderr, " %u@%d", bins[k], vols[k]);
        fprintf(stderr, " -> L %.2f R %.2f C %.2f BL %.2f BR %.2f\n",
                (double)(levels[0][0] + (channels == 2u ? levels[1][0] : 0.0f)),
                (double)(levels[0][1] + (channels == 2u ? levels[1][1] : 0.0f)),
                (double)levels[0][2], (double)levels[0][4], (double)levels[0][5]);
        fflush(stderr);
        v->trace_ms = now;
    }
    /* The voice's filter: DLS2 is the voice processor's resonant low-pass,
     * read as xemu reads FC0 and FC1 -- 2^(fc/4096) for the state-variable
     * coefficient, q/0x8000 for its damping -- which is XAudio2's filter's
     * own parameterisation. Which modes run it depends on the voice, as in
     * xemu's voice_process: a 3D voice's modes are 1 DLS2+I3DL2, 2
     * ParaEQ+I3DL2 and 3 I3DL2 alone; a 2D voice's 1 DLS2, 2 ParaEQ, and 3
     * DLS2+ParaEQ when mono, bypass when stereo. MK Shaolin Monks gives its
     * 3D voices mode 3 with FC0 0x8000 -- no low-pass; read as one, the
     * coefficient is the floor and the voice would be muffled to nothing.
     * ParaEQ is not run (xemu does not run it either). */
    if (v->filter_mode == 1u || (v->filter_mode == 3u && !v->is3d && channels == 1u)) {
        float f = powf(2.0f, (float)v->filter_fc / 4096.0f);
        float q = (float)v->filter_q / 32768.0f;
        if (f < 0.003906f) f = 0.003906f;
        if (f > 1.0f) f = 1.0f;
        if (q < 0.079407f) q = 0.079407f;
        if (q > 1.0f) q = 1.0f;
        mix->filter.low_pass = 1;
        mix->filter.frequency = f;
        mix->filter.one_over_q = q;
    }
    if (v != &none)
        v->used_ms = now;
    unlock();
    mix->ratio = three_d ? ratio : 1.0f;
    return 1;
}

uint32_t hle_ds3d_mixins(uint32_t *keys, uint32_t max)
{
    uint32_t i, n = 0u;
    lock();
    for (i = 0; i < VOICES && n < max; i++)
        if (g_voices[i].key && g_voices[i].mixin)
            keys[n++] = g_voices[i].key;
    unlock();
    return n;
}

int hle_ds3d_envelope(uint32_t voice, uint32_t which, uint32_t out[10])
{
    Voice *v;
    int set = 0;
    if (which > 1u) return 0;
    lock();
    v = find(voice);
    if (v && v->eg_set[which]) {
        memcpy(out, v->eg[which], sizeof v->eg[which]);
        set = 1;
    }
    unlock();
    return set;
}

void hle_ds3d_service(void)
{
    RecompI3dl2 room;
    int dirty;
    lock();
    dirty = g_room_dirty;
    room = g_room;
    g_room_dirty = 0;
    unlock();
    if (dirty) {
        if (tracing())
            fprintf(stderr, "[DS3D] I3DL2 room %d (HF %d), decay %.2f s (HF x%.2f), "
                    "reflections %d after %.3f s, reverb %d after %.3f s, diffusion %.0f, "
                    "density %.0f\n", room.room, room.room_hf, (double)room.decay_time,
                    (double)room.decay_hf_ratio, room.reflections,
                    (double)room.reflections_delay, room.reverb, (double)room.reverb_delay,
                    (double)room.diffusion, (double)room.density);
        recomp_audio_output_set_reverb(&room);
    }
}

uint32_t hle_ds3d_stream_frequency(uint32_t stream)
{
    Voice *v;
    uint32_t f = 0u;
    lock();
    v = find(stream);
    if (v) f = v->frequency;
    unlock();
    return f;
}

uint32_t hle_ds3d_pitch_to_frequency(int32_t pitch)
{
    double f = 48000.0 * pow(2.0, (double)pitch / 4096.0);
    if (f < 1.0) f = 1.0;
    if (f > 200000.0) f = 200000.0;
    return (uint32_t)lrint(f);
}

/* An LPCDSMIXBINS: { DWORD dwMixBinCount; DSMIXBINVOLUMEPAIR *pairs } with
 * pairs { DWORD dwMixBin; LONG lVolume }. `route` replaces the bins (and
 * their volumes); otherwise only the volumes of the bins named change. */
static void read_mixbins(Voice *v, uint32_t mixbins, int route)
{
    uint32_t count, pairs, k;

    if (!mixbins || !guest_readable(mixbins, 8u))
        return;
    count = HLE_MEM32(mixbins);
    pairs = HLE_MEM32(mixbins + 4u);
    if (count > MAX_BINS || (count && !guest_readable(pairs, count * 8u)))
        return;
    if (route) {
        v->bin_count = 0u;
        memset(v->bin_volume, 0, sizeof v->bin_volume);
    }
    for (k = 0; k < count; k++) {
        uint32_t bin = HLE_MEM32(pairs + k * 8u);
        int32_t volume = (int32_t)HLE_MEM32(pairs + k * 8u + 4u);
        if (bin >= BIN_COUNT)
            continue;
        if (route)
            v->bins[v->bin_count++] = (uint8_t)bin;
        v->bin_volume[bin] = volume;
    }
    v->bins_set = 1;
}

void hle_ds3d_created(uint32_t key, uint32_t flags, uint32_t mixbins)
{
    Voice *v;
    if (!key) return;
    lock();
    v = find(key);
    if (v) memset(v, 0, sizeof *v);
    v = get(key);
    v->is3d = (flags & CAPS_CTRL3D) != 0u;
    v->mute_at_max = (flags & CAPS_MUTE3DATMAXDISTANCE) != 0u;
    v->mixin = (flags & CAPS_MIXIN) != 0u;
    if (v->mixin) {
        static int said;
        if (said < 4) {
            said++;
            fprintf(stderr, "[DS3D] buffer %08X is a mix-in buffer\n", key);
        }
    }
    read_mixbins(v, mixbins, 1);
    unlock();
}

/* ── The public setters ──────────────────────────────────────── */

/* Run the title's own body (its result is the call's), or succeed. */
#define PASS(name)                                                       \
    do {                                                                 \
        if (hle_original_##name) { HLE_CALL_ORIGINAL(name); }            \
        else g_eax = 0u;                                                 \
    } while (0)

/* One 3D setting for `key`. Immediate settings change what is heard now;
 * deferred ones wait for CommitDeferredSettings, as on the console. The
 * assignment runs on the pending record, and on the live one when immediate. */
static Ds3dBuffer *begin_3d(uint32_t key, uint32_t apply, Ds3dBuffer **live)
{
    Voice *v;
    lock();
    v = get(key);
    *live = NULL;
    if (!v) return NULL;
    if (!v->is3d) {
        static int said;
        if (said < 16) {
            said++;
            fprintf(stderr, "[DS3D] voice %08X is 3D\n", key);
        }
    }
    v->is3d = 1;
    if (apply & DS3D_DEFERRED) v->pending_set = 1;
    else *live = &v->now;
    return &v->pending;
}

static Ds3dListener *begin_listener(uint32_t apply, Ds3dListener **live)
{
    lock();
    *live = NULL;
    if (apply & DS3D_DEFERRED) g_listener_pending_set = 1;
    else *live = &g_listener;
    return &g_listener_pending;
}

static void read_all_parameters(Ds3dBuffer *b, uint32_t p)
{
    uint32_t size = HLE_MEM32(p);
    int extra = size >= 0x4Cu && guest_readable(p, 0x4Cu);
    b->position = guest_vec(p + 4u);
    b->velocity = guest_vec(p + 0x10u);
    b->inside_angle = HLE_MEM32(p + 0x1Cu);
    b->outside_angle = HLE_MEM32(p + 0x20u);
    b->cone_orientation = guest_vec(p + 0x24u);
    b->cone_outside_volume = (int32_t)HLE_MEM32(p + 0x30u);
    b->min_distance = as_float(HLE_MEM32(p + 0x34u));
    b->max_distance = as_float(HLE_MEM32(p + 0x38u));
    b->mode = HLE_MEM32(p + 0x3Cu);
    if (extra) {
        b->distance_factor = as_float(HLE_MEM32(p + 0x40u));
        b->rolloff_factor = as_float(HLE_MEM32(p + 0x44u));
        b->doppler_factor = as_float(HLE_MEM32(p + 0x48u));
    }
}

static void read_listener(Ds3dListener *l, uint32_t p)
{
    l->position = guest_vec(p + 4u);
    l->velocity = guest_vec(p + 0x10u);
    l->front = guest_vec(p + 0x1Cu);
    l->top = guest_vec(p + 0x28u);
    l->distance_factor = as_float(HLE_MEM32(p + 0x34u));
    l->rolloff_factor = as_float(HLE_MEM32(p + 0x38u));
    l->doppler_factor = as_float(HLE_MEM32(p + 0x3Cu));
}

static void mixbins(uint32_t key, uint32_t bins, int route)
{
    Voice *v;
    lock();
    v = get(key);
    if (v) read_mixbins(v, bins, route || !v->bins_set);
    unlock();
}

static void stream_frequency(uint32_t key, uint32_t frequency)
{
    Voice *v;
    lock();
    v = get(key);
    if (v) v->frequency = frequency;
    unlock();
}

/* IDirectSoundBuffer_* (this, ..., dwApply) */

HLE_ORIGINAL(IDirectSoundBuffer_SetPosition);
HLE_EXPORT(IDirectSoundBuffer_SetPosition)
{
    Vec3 p = { arg_float(1), arg_float(2), arg_float(3) };
    Ds3dBuffer *live, *b = begin_3d(HLE_ARG(0), HLE_ARG(4), &live);
    if (b) { b->position = p; }
    if ((b = live) != NULL) { b->position = p; }
    unlock();
    PASS(IDirectSoundBuffer_SetPosition);
}

HLE_ORIGINAL(IDirectSoundBuffer_SetVelocity);
HLE_EXPORT(IDirectSoundBuffer_SetVelocity)
{
    Vec3 p = { arg_float(1), arg_float(2), arg_float(3) };
    Ds3dBuffer *live, *b = begin_3d(HLE_ARG(0), HLE_ARG(4), &live);
    if (b) { b->velocity = p; }
    if ((b = live) != NULL) { b->velocity = p; }
    unlock();
    PASS(IDirectSoundBuffer_SetVelocity);
}

HLE_ORIGINAL(IDirectSoundBuffer_SetConeOrientation);
HLE_EXPORT(IDirectSoundBuffer_SetConeOrientation)
{
    Vec3 p = { arg_float(1), arg_float(2), arg_float(3) };
    Ds3dBuffer *live, *b = begin_3d(HLE_ARG(0), HLE_ARG(4), &live);
    if (b) { b->cone_orientation = p; }
    if ((b = live) != NULL) { b->cone_orientation = p; }
    unlock();
    PASS(IDirectSoundBuffer_SetConeOrientation);
}

HLE_ORIGINAL(IDirectSoundBuffer_SetConeAngles);
HLE_EXPORT(IDirectSoundBuffer_SetConeAngles)
{
    uint32_t in = HLE_ARG(1), out = HLE_ARG(2);
    Ds3dBuffer *live, *b = begin_3d(HLE_ARG(0), HLE_ARG(3), &live);
    if (b) { b->inside_angle = in; b->outside_angle = out; }
    if ((b = live) != NULL) { b->inside_angle = in; b->outside_angle = out; }
    unlock();
    PASS(IDirectSoundBuffer_SetConeAngles);
}

HLE_ORIGINAL(IDirectSoundBuffer_SetConeOutsideVolume);
HLE_EXPORT(IDirectSoundBuffer_SetConeOutsideVolume)
{
    int32_t vol = (int32_t)HLE_ARG(1);
    Ds3dBuffer *live, *b = begin_3d(HLE_ARG(0), HLE_ARG(2), &live);
    if (b) { b->cone_outside_volume = vol; }
    if ((b = live) != NULL) { b->cone_outside_volume = vol; }
    unlock();
    PASS(IDirectSoundBuffer_SetConeOutsideVolume);
}

HLE_ORIGINAL(IDirectSoundBuffer_SetMinDistance);
HLE_EXPORT(IDirectSoundBuffer_SetMinDistance)
{
    float f = arg_float(1);
    Ds3dBuffer *live, *b = begin_3d(HLE_ARG(0), HLE_ARG(2), &live);
    if (b) { b->min_distance = f; }
    if ((b = live) != NULL) { b->min_distance = f; }
    unlock();
    PASS(IDirectSoundBuffer_SetMinDistance);
}

HLE_ORIGINAL(IDirectSoundBuffer_SetMaxDistance);
HLE_EXPORT(IDirectSoundBuffer_SetMaxDistance)
{
    float f = arg_float(1);
    Ds3dBuffer *live, *b = begin_3d(HLE_ARG(0), HLE_ARG(2), &live);
    if (b) { b->max_distance = f; }
    if ((b = live) != NULL) { b->max_distance = f; }
    unlock();
    PASS(IDirectSoundBuffer_SetMaxDistance);
}

HLE_ORIGINAL(IDirectSoundBuffer_SetMode);
HLE_EXPORT(IDirectSoundBuffer_SetMode)
{
    uint32_t m = HLE_ARG(1);
    Ds3dBuffer *live, *b = begin_3d(HLE_ARG(0), HLE_ARG(2), &live);
    if (b) { b->mode = m; }
    if ((b = live) != NULL) { b->mode = m; }
    unlock();
    PASS(IDirectSoundBuffer_SetMode);
}

HLE_ORIGINAL(IDirectSoundBuffer_SetDistanceFactor);
HLE_EXPORT(IDirectSoundBuffer_SetDistanceFactor)
{
    float f = arg_float(1);
    Ds3dBuffer *live, *b = begin_3d(HLE_ARG(0), HLE_ARG(2), &live);
    if (b) { b->distance_factor = f; }
    if ((b = live) != NULL) { b->distance_factor = f; }
    unlock();
    PASS(IDirectSoundBuffer_SetDistanceFactor);
}

HLE_ORIGINAL(IDirectSoundBuffer_SetRolloffFactor);
HLE_EXPORT(IDirectSoundBuffer_SetRolloffFactor)
{
    float f = arg_float(1);
    Ds3dBuffer *live, *b = begin_3d(HLE_ARG(0), HLE_ARG(2), &live);
    if (b) { b->rolloff_factor = f; }
    if ((b = live) != NULL) { b->rolloff_factor = f; }
    unlock();
    PASS(IDirectSoundBuffer_SetRolloffFactor);
}

HLE_ORIGINAL(IDirectSoundBuffer_SetDopplerFactor);
HLE_EXPORT(IDirectSoundBuffer_SetDopplerFactor)
{
    float f = arg_float(1);
    Ds3dBuffer *live, *b = begin_3d(HLE_ARG(0), HLE_ARG(2), &live);
    if (b) { b->doppler_factor = f; }
    if ((b = live) != NULL) { b->doppler_factor = f; }
    unlock();
    PASS(IDirectSoundBuffer_SetDopplerFactor);
}

HLE_ORIGINAL(IDirectSoundBuffer_SetRolloffCurve);
HLE_EXPORT(IDirectSoundBuffer_SetRolloffCurve)
{
    uint32_t pts = HLE_ARG(1), n = HLE_ARG(2);
    Ds3dBuffer *live, *b = begin_3d(HLE_ARG(0), HLE_ARG(3), &live);
    if (b) { b->curve = pts; b->curve_points = n; }
    if ((b = live) != NULL) { b->curve = pts; b->curve_points = n; }
    unlock();
    PASS(IDirectSoundBuffer_SetRolloffCurve);
}

HLE_ORIGINAL(IDirectSoundBuffer_SetAllParameters);
HLE_EXPORT(IDirectSoundBuffer_SetAllParameters)
{
    uint32_t p = HLE_ARG(1);
    if (p && guest_readable(p, 0x40u)) {
        Ds3dBuffer *live, *b = begin_3d(HLE_ARG(0), HLE_ARG(2), &live);
        if (b) read_all_parameters(b, p);
        if (live) read_all_parameters(live, p);
        unlock();
    }
    PASS(IDirectSoundBuffer_SetAllParameters);
}

HLE_ORIGINAL(IDirectSoundBuffer_SetMixBins);
HLE_EXPORT(IDirectSoundBuffer_SetMixBins)
{
    mixbins(HLE_ARG(0), HLE_ARG(1), 1);
    PASS(IDirectSoundBuffer_SetMixBins);
}

HLE_ORIGINAL(IDirectSoundBuffer_SetMixBinVolumes_8);
HLE_EXPORT(IDirectSoundBuffer_SetMixBinVolumes_8)
{
    mixbins(HLE_ARG(0), HLE_ARG(1), 0);
    PASS(IDirectSoundBuffer_SetMixBinVolumes_8);
}

/* IDirectSoundStream_* (this, ..., dwApply) */

HLE_ORIGINAL(IDirectSoundStream_SetPosition);
HLE_EXPORT(IDirectSoundStream_SetPosition)
{
    Vec3 p = { arg_float(1), arg_float(2), arg_float(3) };
    Ds3dBuffer *live, *b = begin_3d(HLE_ARG(0), HLE_ARG(4), &live);
    if (b) { b->position = p; }
    if ((b = live) != NULL) { b->position = p; }
    unlock();
    PASS(IDirectSoundStream_SetPosition);
}

HLE_ORIGINAL(IDirectSoundStream_SetVelocity);
HLE_EXPORT(IDirectSoundStream_SetVelocity)
{
    Vec3 p = { arg_float(1), arg_float(2), arg_float(3) };
    Ds3dBuffer *live, *b = begin_3d(HLE_ARG(0), HLE_ARG(4), &live);
    if (b) { b->velocity = p; }
    if ((b = live) != NULL) { b->velocity = p; }
    unlock();
    PASS(IDirectSoundStream_SetVelocity);
}

HLE_ORIGINAL(IDirectSoundStream_SetConeOrientation);
HLE_EXPORT(IDirectSoundStream_SetConeOrientation)
{
    Vec3 p = { arg_float(1), arg_float(2), arg_float(3) };
    Ds3dBuffer *live, *b = begin_3d(HLE_ARG(0), HLE_ARG(4), &live);
    if (b) { b->cone_orientation = p; }
    if ((b = live) != NULL) { b->cone_orientation = p; }
    unlock();
    PASS(IDirectSoundStream_SetConeOrientation);
}

HLE_ORIGINAL(IDirectSoundStream_SetConeAngles);
HLE_EXPORT(IDirectSoundStream_SetConeAngles)
{
    uint32_t in = HLE_ARG(1), out = HLE_ARG(2);
    Ds3dBuffer *live, *b = begin_3d(HLE_ARG(0), HLE_ARG(3), &live);
    if (b) { b->inside_angle = in; b->outside_angle = out; }
    if ((b = live) != NULL) { b->inside_angle = in; b->outside_angle = out; }
    unlock();
    PASS(IDirectSoundStream_SetConeAngles);
}

HLE_ORIGINAL(IDirectSoundStream_SetConeOutsideVolume);
HLE_EXPORT(IDirectSoundStream_SetConeOutsideVolume)
{
    int32_t vol = (int32_t)HLE_ARG(1);
    Ds3dBuffer *live, *b = begin_3d(HLE_ARG(0), HLE_ARG(2), &live);
    if (b) { b->cone_outside_volume = vol; }
    if ((b = live) != NULL) { b->cone_outside_volume = vol; }
    unlock();
    PASS(IDirectSoundStream_SetConeOutsideVolume);
}

HLE_ORIGINAL(IDirectSoundStream_SetMinDistance);
HLE_EXPORT(IDirectSoundStream_SetMinDistance)
{
    float f = arg_float(1);
    Ds3dBuffer *live, *b = begin_3d(HLE_ARG(0), HLE_ARG(2), &live);
    if (b) { b->min_distance = f; }
    if ((b = live) != NULL) { b->min_distance = f; }
    unlock();
    PASS(IDirectSoundStream_SetMinDistance);
}

HLE_ORIGINAL(IDirectSoundStream_SetMaxDistance);
HLE_EXPORT(IDirectSoundStream_SetMaxDistance)
{
    float f = arg_float(1);
    Ds3dBuffer *live, *b = begin_3d(HLE_ARG(0), HLE_ARG(2), &live);
    if (b) { b->max_distance = f; }
    if ((b = live) != NULL) { b->max_distance = f; }
    unlock();
    PASS(IDirectSoundStream_SetMaxDistance);
}

HLE_ORIGINAL(IDirectSoundStream_SetMode);
HLE_EXPORT(IDirectSoundStream_SetMode)
{
    uint32_t m = HLE_ARG(1);
    Ds3dBuffer *live, *b = begin_3d(HLE_ARG(0), HLE_ARG(2), &live);
    if (b) { b->mode = m; }
    if ((b = live) != NULL) { b->mode = m; }
    unlock();
    PASS(IDirectSoundStream_SetMode);
}

HLE_ORIGINAL(IDirectSoundStream_SetDistanceFactor);
HLE_EXPORT(IDirectSoundStream_SetDistanceFactor)
{
    float f = arg_float(1);
    Ds3dBuffer *live, *b = begin_3d(HLE_ARG(0), HLE_ARG(2), &live);
    if (b) { b->distance_factor = f; }
    if ((b = live) != NULL) { b->distance_factor = f; }
    unlock();
    PASS(IDirectSoundStream_SetDistanceFactor);
}

HLE_ORIGINAL(IDirectSoundStream_SetRolloffFactor);
HLE_EXPORT(IDirectSoundStream_SetRolloffFactor)
{
    float f = arg_float(1);
    Ds3dBuffer *live, *b = begin_3d(HLE_ARG(0), HLE_ARG(2), &live);
    if (b) { b->rolloff_factor = f; }
    if ((b = live) != NULL) { b->rolloff_factor = f; }
    unlock();
    PASS(IDirectSoundStream_SetRolloffFactor);
}

HLE_ORIGINAL(IDirectSoundStream_SetDopplerFactor);
HLE_EXPORT(IDirectSoundStream_SetDopplerFactor)
{
    float f = arg_float(1);
    Ds3dBuffer *live, *b = begin_3d(HLE_ARG(0), HLE_ARG(2), &live);
    if (b) { b->doppler_factor = f; }
    if ((b = live) != NULL) { b->doppler_factor = f; }
    unlock();
    PASS(IDirectSoundStream_SetDopplerFactor);
}

HLE_ORIGINAL(IDirectSoundStream_SetRolloffCurve);
HLE_EXPORT(IDirectSoundStream_SetRolloffCurve)
{
    uint32_t pts = HLE_ARG(1), n = HLE_ARG(2);
    Ds3dBuffer *live, *b = begin_3d(HLE_ARG(0), HLE_ARG(3), &live);
    if (b) { b->curve = pts; b->curve_points = n; }
    if ((b = live) != NULL) { b->curve = pts; b->curve_points = n; }
    unlock();
    PASS(IDirectSoundStream_SetRolloffCurve);
}

HLE_ORIGINAL(IDirectSoundStream_SetAllParameters);
HLE_EXPORT(IDirectSoundStream_SetAllParameters)
{
    uint32_t p = HLE_ARG(1);
    if (p && guest_readable(p, 0x40u)) {
        Ds3dBuffer *live, *b = begin_3d(HLE_ARG(0), HLE_ARG(2), &live);
        if (b) read_all_parameters(b, p);
        if (live) read_all_parameters(live, p);
        unlock();
    }
    PASS(IDirectSoundStream_SetAllParameters);
}

HLE_ORIGINAL(IDirectSoundStream_SetMixBins);
HLE_EXPORT(IDirectSoundStream_SetMixBins)
{
    mixbins(HLE_ARG(0), HLE_ARG(1), 1);
    PASS(IDirectSoundStream_SetMixBins);
}

HLE_ORIGINAL(IDirectSoundStream_SetMixBinVolumes_8);
HLE_EXPORT(IDirectSoundStream_SetMixBinVolumes_8)
{
    mixbins(HLE_ARG(0), HLE_ARG(1), 0);
    PASS(IDirectSoundStream_SetMixBinVolumes_8);
}

/* Streams have no rate model of their own to change (hle_dsound_stream.c
 * plays them at their format's rate), so their frequency is kept here and
 * becomes the slot's playback ratio. Buffers: hle_dsound.c. */

HLE_ORIGINAL(IDirectSoundStream_SetFrequency);
HLE_EXPORT(IDirectSoundStream_SetFrequency)
{
    stream_frequency(HLE_ARG(0), HLE_ARG(1));
    PASS(IDirectSoundStream_SetFrequency);
}

HLE_ORIGINAL(IDirectSoundStream_SetPitch);
HLE_EXPORT(IDirectSoundStream_SetPitch)
{
    stream_frequency(HLE_ARG(0), hle_ds3d_pitch_to_frequency((int32_t)HLE_ARG(1)));
    PASS(IDirectSoundStream_SetPitch);
}

/* The listener: IDirectSound_* (this, ..., dwApply). One listener. */

HLE_ORIGINAL(IDirectSound_SetPosition);
HLE_EXPORT(IDirectSound_SetPosition)
{
    Vec3 p = { arg_float(1), arg_float(2), arg_float(3) };
    Ds3dListener *live, *l = begin_listener(HLE_ARG(4), &live);
    l->position = p;
    if ((l = live) != NULL) { l->position = p; }
    unlock();
    PASS(IDirectSound_SetPosition);
}

HLE_ORIGINAL(IDirectSound_SetVelocity);
HLE_EXPORT(IDirectSound_SetVelocity)
{
    Vec3 p = { arg_float(1), arg_float(2), arg_float(3) };
    Ds3dListener *live, *l = begin_listener(HLE_ARG(4), &live);
    l->velocity = p;
    if ((l = live) != NULL) { l->velocity = p; }
    unlock();
    PASS(IDirectSound_SetVelocity);
}

HLE_ORIGINAL(IDirectSound_SetOrientation);
HLE_EXPORT(IDirectSound_SetOrientation)
{
    Vec3 f = { arg_float(1), arg_float(2), arg_float(3) }, t = { arg_float(4), arg_float(5), arg_float(6) };
    Ds3dListener *live, *l = begin_listener(HLE_ARG(7), &live);
    l->front = f; l->top = t;
    if ((l = live) != NULL) { l->front = f; l->top = t; }
    unlock();
    PASS(IDirectSound_SetOrientation);
}

HLE_ORIGINAL(IDirectSound_SetDistanceFactor);
HLE_EXPORT(IDirectSound_SetDistanceFactor)
{
    float f = arg_float(1);
    Ds3dListener *live, *l = begin_listener(HLE_ARG(2), &live);
    l->distance_factor = f;
    if ((l = live) != NULL) { l->distance_factor = f; }
    unlock();
    PASS(IDirectSound_SetDistanceFactor);
}

HLE_ORIGINAL(IDirectSound_SetRolloffFactor);
HLE_EXPORT(IDirectSound_SetRolloffFactor)
{
    float f = arg_float(1);
    Ds3dListener *live, *l = begin_listener(HLE_ARG(2), &live);
    l->rolloff_factor = f;
    if ((l = live) != NULL) { l->rolloff_factor = f; }
    unlock();
    PASS(IDirectSound_SetRolloffFactor);
}

HLE_ORIGINAL(IDirectSound_SetDopplerFactor);
HLE_EXPORT(IDirectSound_SetDopplerFactor)
{
    float f = arg_float(1);
    Ds3dListener *live, *l = begin_listener(HLE_ARG(2), &live);
    l->doppler_factor = f;
    if ((l = live) != NULL) { l->doppler_factor = f; }
    unlock();
    PASS(IDirectSound_SetDopplerFactor);
}

HLE_ORIGINAL(IDirectSound_SetAllParameters);
HLE_EXPORT(IDirectSound_SetAllParameters)
{
    uint32_t p = HLE_ARG(1);
    if (p && guest_readable(p, 0x40u)) {
        Ds3dListener *live, *l = begin_listener(HLE_ARG(2), &live);
        read_listener(l, p);
        if (live) read_listener(live, p);
        unlock();
    }
    PASS(IDirectSound_SetAllParameters);
}

/* The room (DSI3DL2LISTENER, the same fields in the same order as
 * RecompI3dl2): IDirectSound_SetI3DL2Listener(this, pds3dl, dwApply). The
 * host's reverb takes it from the service thread (hle_ds3d_service). */
static void read_room(RecompI3dl2 *r, uint32_t p)
{
    r->room = (int32_t)HLE_MEM32(p);
    r->room_hf = (int32_t)HLE_MEM32(p + 4u);
    r->room_rolloff_factor = as_float(HLE_MEM32(p + 8u));
    r->decay_time = as_float(HLE_MEM32(p + 0xCu));
    r->decay_hf_ratio = as_float(HLE_MEM32(p + 0x10u));
    r->reflections = (int32_t)HLE_MEM32(p + 0x14u);
    r->reflections_delay = as_float(HLE_MEM32(p + 0x18u));
    r->reverb = (int32_t)HLE_MEM32(p + 0x1Cu);
    r->reverb_delay = as_float(HLE_MEM32(p + 0x20u));
    r->diffusion = as_float(HLE_MEM32(p + 0x24u));
    r->density = as_float(HLE_MEM32(p + 0x28u));
    r->hf_reference = as_float(HLE_MEM32(p + 0x2Cu));
}

HLE_ORIGINAL(IDirectSound_SetI3DL2Listener);
HLE_EXPORT(IDirectSound_SetI3DL2Listener)
{
    uint32_t p = HLE_ARG(1), apply = HLE_ARG(2);
    if (p && guest_readable(p, 0x30u)) {
        lock();
        if (apply & DS3D_DEFERRED) {
            read_room(&g_room_pending, p);
            g_room_pending_set = 1;
        } else {
            read_room(&g_room, p);
            g_room_set = g_room_dirty = 1;
        }
        unlock();
        {
            static int said;
            if (said < 4) {
                said++;
                fprintf(stderr, "[DS3D] I3DL2 listener: room %d, decay %.2f s%s\n",
                        (int32_t)HLE_MEM32(p), (double)as_float(HLE_MEM32(p + 0xCu)),
                        (apply & DS3D_DEFERRED) ? " (deferred)" : "");
            }
        }
    }
    PASS(IDirectSound_SetI3DL2Listener);
}

/* DSI3DL2BUFFER { lDirect, lDirectHF, lRoom, lRoomHF, flRoomRolloffFactor,
 * Obstruction, Occlusion }: the source's direct level and its send into the
 * room, which hle_ds3d_mix adds to the speaker bins and to the I3DL2 bin.
 * Taken at once whatever dwApply says. The high-frequency levels, the
 * obstruction and the occlusion shape the console's I3DL2 send and direct
 * path filters, which this runtime does not run. */
static void i3dl2_source(uint32_t key, uint32_t p)
{
    Voice *v;
    if (!p || !guest_readable(p, 0x10u))
        return;
    lock();
    v = get(key);
    if (v) {
        v->i3dl2_direct = (int32_t)HLE_MEM32(p);
        v->i3dl2_room = (int32_t)HLE_MEM32(p + 8u);
        v->i3dl2_set = 1;
    }
    unlock();
}

HLE_ORIGINAL(IDirectSoundBuffer_SetI3DL2Source);
HLE_EXPORT(IDirectSoundBuffer_SetI3DL2Source)
{
    i3dl2_source(HLE_ARG(0), HLE_ARG(1));
    PASS(IDirectSoundBuffer_SetI3DL2Source);
}

HLE_ORIGINAL(IDirectSoundStream_SetI3DL2Source);
HLE_EXPORT(IDirectSoundStream_SetI3DL2Source)
{
    i3dl2_source(HLE_ARG(0), HLE_ARG(1));
    PASS(IDirectSoundStream_SetI3DL2Source);
}

/* DSFILTERDESC { dwMode, dwQCoefficient, adwCoefficients[4] }: the XDK writes
 * the first two coefficients to the voice's FC0 and FC1. Mode 0 is bypass. */
static void filter(uint32_t key, uint32_t p)
{
    Voice *v;
    if (!p || !guest_readable(p, 0x18u))
        return;
    lock();
    v = get(key);
    if (v) {
        v->filter_mode = HLE_MEM32(p);
        v->filter_fc = (int16_t)(HLE_MEM32(p + 8u) & 0xFFFFu);
        v->filter_q = HLE_MEM32(p + 0xCu) & 0xFFFFu;
        if (tracing())
            fprintf(stderr, "[DS3D] voice %08X filter mode %u, FC0 %d, FC1 %u\n",
                    key, v->filter_mode, v->filter_fc, v->filter_q);
    }
    unlock();
}

HLE_ORIGINAL(IDirectSoundBuffer_SetFilter);
HLE_EXPORT(IDirectSoundBuffer_SetFilter)
{
    filter(HLE_ARG(0), HLE_ARG(1));
    PASS(IDirectSoundBuffer_SetFilter);
}

HLE_ORIGINAL(IDirectSoundStream_SetFilter);
HLE_EXPORT(IDirectSoundStream_SetFilter)
{
    filter(HLE_ARG(0), HLE_ARG(1));
    PASS(IDirectSoundStream_SetFilter);
}

/* SetOutputBuffer(this, pOutputBuffer): play into a mix-in buffer (NULL: back
 * to the mix bins). */
static void output_buffer(uint32_t key, uint32_t out)
{
    Voice *v;
    lock();
    v = get(key);
    if (v) v->output = out;
    unlock();
}

HLE_ORIGINAL(IDirectSoundBuffer_SetOutputBuffer);
HLE_EXPORT(IDirectSoundBuffer_SetOutputBuffer)
{
    output_buffer(HLE_ARG(0), HLE_ARG(1));
    PASS(IDirectSoundBuffer_SetOutputBuffer);
}

HLE_ORIGINAL(IDirectSoundStream_SetOutputBuffer);
HLE_EXPORT(IDirectSoundStream_SetOutputBuffer)
{
    output_buffer(HLE_ARG(0), HLE_ARG(1));
    PASS(IDirectSoundStream_SetOutputBuffer);
}

/* SetEG(this, pEnvelopeDesc): DSENVELOPEDESC's ten DWORDs, the first saying
 * which envelope: DSEG_AMPLITUDE is 1 and DSEG_MULTI is 0. Stored here as
 * [0] amplitude, [1] multi, which is how hle_dsound.c runs them.
 *
 * The XDK's own CMcpxVoiceClient_SetEG says so: its dwEG == 0 branch is the
 * only one that reads lPitchScale and lFilterCutOff (+0x20, +0x24), which
 * only the multi envelope has, and any other value stores nothing. This was
 * read the other way round, and a title that sets both (TimeSplitters 2, for
 * every sound effect: amplitude sustain 255, multi all 0) had its multi
 * envelope applied as the volume -- every effect sample times 0. */
static void envelope(uint32_t key, uint32_t p)
{
    Voice *v;
    uint32_t k, which, eg;
    if (!p || !guest_readable(p, 40u))
        return;
    eg = HLE_MEM32(p);
    if (eg > 1u)
        return;
    which = eg == 1u ? 0u : 1u;
    lock();
    v = get(key);
    if (v) {
        for (k = 0; k < 10u; k++)
            v->eg[which][k] = HLE_MEM32(p + k * 4u);
        v->eg_set[which] = 1;
        if (tracing())
            fprintf(stderr, "[DS3D] voice %08X %s envelope: mode %u, delay %u attack %u "
                    "hold %u decay %u release %u sustain %u, pitch %d filter %d\n", key,
                    which ? "multi" : "amplitude", v->eg[which][1], v->eg[which][2],
                    v->eg[which][3], v->eg[which][4], v->eg[which][5], v->eg[which][6],
                    v->eg[which][7], (int8_t)v->eg[which][8], (int8_t)v->eg[which][9]);
    }
    unlock();
}

HLE_ORIGINAL(IDirectSoundBuffer_SetEG);
HLE_EXPORT(IDirectSoundBuffer_SetEG)
{
    envelope(HLE_ARG(0), HLE_ARG(1));
    PASS(IDirectSoundBuffer_SetEG);
}

HLE_ORIGINAL(IDirectSoundStream_SetEG);
HLE_EXPORT(IDirectSoundStream_SetEG)
{
    envelope(HLE_ARG(0), HLE_ARG(1));
    PASS(IDirectSoundStream_SetEG);
}

/* Everything deferred becomes what is heard. */
HLE_ORIGINAL(IDirectSound_CommitDeferredSettings);
HLE_EXPORT(IDirectSound_CommitDeferredSettings)
{
    uint32_t i;
    lock();
    if (g_listener_pending_set) {
        g_listener = g_listener_pending;
        g_listener_pending_set = 0;
    }
    if (g_room_pending_set) {
        g_room = g_room_pending;
        g_room_set = g_room_dirty = 1;
        g_room_pending_set = 0;
    }
    for (i = 0; i < VOICES; i++)
        if (g_voices[i].key && g_voices[i].pending_set) {
            g_voices[i].now = g_voices[i].pending;
            g_voices[i].pending_set = 0;
        }
    unlock();
    PASS(IDirectSound_CommitDeferredSettings);
}

/* Creation: DSBUFFERDESC { dwSize, dwFlags, dwBufferBytes, lpwfxFormat,
 * lpMixBins, dwInputMixBin }. Whether the buffer is 3D, mutes at its maximum
 * distance, and the mix bins it starts with. */
static void buffer_created(uint32_t desc, uint32_t out)
{
    if ((int32_t)g_eax < 0 || !out || !guest_readable(out, 4u) ||
        !desc || !guest_readable(desc, 0x14u))
        return;
    hle_ds3d_created(HLE_MEM32(out), HLE_MEM32(desc + 4u), HLE_MEM32(desc + 0x10u));
}

HLE_ORIGINAL(DirectSoundCreateBuffer);
HLE_EXPORT(DirectSoundCreateBuffer)
{
    uint32_t desc = HLE_ARG(0), out = HLE_ARG(1);
    PASS(DirectSoundCreateBuffer);
    buffer_created(desc, out);
}

HLE_ORIGINAL(IDirectSound_CreateSoundBuffer);
HLE_EXPORT(IDirectSound_CreateSoundBuffer)
{
    uint32_t desc = HLE_ARG(1), out = HLE_ARG(2);
    PASS(IDirectSound_CreateSoundBuffer);
    buffer_created(desc, out);
}
