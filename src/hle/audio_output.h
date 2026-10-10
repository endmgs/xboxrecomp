/*
 * audio_output.h -- host audio output for replaced DirectSound buffers.
 *
 * From doaxbv-re (https://github.com/NoRain211/doaxbv-re,
 * recomp-runtime/audio_output.h), GPL-3.0. One behaviour change: output is on
 * at full gain unless RECOMP_AUDIO_GAIN says otherwise (doaxbv-re defaults to
 * muted while it validates its decoders).
 */
#ifndef XBOXRECOMP_AUDIO_OUTPUT_H
#define XBOXRECOMP_AUDIO_OUTPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialization is attempted once per process; an unavailable device or
   shutdown leaves guest audio timing unchanged. RECOMP_AUDIO_GAIN is the host
   master gain (0..1); absent means 1, zero mutes without opening a device. */
#define RECOMP_AUDIO_SLOT_MOVIE 272u

void recomp_audio_output_initialize(void);
void recomp_audio_output_shutdown(void);
void recomp_audio_output_reset_voice(uint32_t slot);

/* Copies PCM before returning; never retains a guest-memory pointer.
   Slots: 0..255 for buffers, 256..271 for streams, 272
   (RECOMP_AUDIO_SLOT_MOVIE) for a movie's sound. PCM: 1000..200000 Hz,
   mono/stereo, unsigned 8 or signed 16 bit.
   Each submission is frame-aligned and at most 160000 bytes.

   Returns 1 when the sound was taken and 0 when it was not -- a full queue,
   a device that is gone, PCM it cannot play. A caller feeding a continuous
   stream must not treat 0 as delivered: the sound it describes will never be
   heard, so it has to be offered again rather than stepped over, or the
   stream slips behind by exactly what was dropped. */
int recomp_audio_output_submit(
    uint32_t slot, const uint8_t *pcm, uint32_t bytes,
    uint32_t sample_rate, uint32_t channels, uint32_t bits_per_sample,
    int32_t volume_hundredth_db);

/* Where this slot actually is: bytes of PCM the device has played since the
   voice was created (reset_voice destroys it, so the count restarts at zero
   with the next submission), and how many submissions are still queued.
   Returns 0 when there is no voice, when output is off, or when the count
   cannot be read -- the caller must then fall back to its own clock.

   This is the device's clock, not the wall clock, and it is the one the
   listener hears: a stream whose packets complete on the wall clock drifts
   away from what is coming out of the speakers, by the device's rate error
   and by every gap the queue ever ran dry for. */
int recomp_audio_output_position(uint32_t slot, uint64_t *played_bytes,
                                 uint32_t *queued_buffers);

/* The speakers a slot's sound is placed on, in the order of the Xbox's first
   six mix bins (DSMIXBIN_FRONT_LEFT .. DSMIXBIN_BACK_RIGHT) -- which is also
   SDL's and WAVEFORMATEXTENSIBLE's 5.1 order. */
enum {
    RECOMP_SPEAKER_FRONT_LEFT, RECOMP_SPEAKER_FRONT_RIGHT, RECOMP_SPEAKER_CENTER,
    RECOMP_SPEAKER_LFE, RECOMP_SPEAKER_BACK_LEFT, RECOMP_SPEAKER_BACK_RIGHT,
    RECOMP_SPEAKERS
};

/* Where a slot's sound goes and how fast it plays, from the next submission
   on and for every voice the slot gets until it is changed or cleared.

   levels[c][s] is the linear gain from source channel c (0, or 0 and 1 for
   stereo) to speaker s, on top of the submission's own volume. The backend
   folds speakers the device does not have into the ones it does (centre to
   both fronts, backs to sides or fronts, at -3 dB; LFE where there is one).

   ratio plays the slot that much faster or slower than the rate it is
   submitted at (1 = as submitted), without restarting its voice: the
   submitted bytes keep their meaning, and recomp_audio_output_position still
   counts them. Clamped to 1/8..8.

   A slot that never had a mix set plays exactly as before: each channel to
   the device's default mapping, at the submitted rate. Clearing a slot's
   mix gives its next voice that again; the voice it has now is left as it
   is rather than restarted, which would drop what it has queued. */
void recomp_audio_output_set_mix(uint32_t slot,
                                 const float levels[2][RECOMP_SPEAKERS],
                                 float ratio);
void recomp_audio_output_clear_mix(uint32_t slot);

#ifdef __cplusplus
}
#endif

#endif
