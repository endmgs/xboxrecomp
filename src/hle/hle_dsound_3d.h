/*
 * hle_dsound_3d.h -- where a DirectSound buffer or stream is heard: its mix
 * bins, its 3D position, and its pitch, as the title set them through the
 * public DirectSound API. See hle_dsound_3d.c.
 */
#ifndef XBOXRECOMP_HLE_DSOUND_3D_H
#define XBOXRECOMP_HLE_DSOUND_3D_H

#include <stdint.h>
#include "audio_output.h"

/* For the buffer or stream the title holds as `voice` (the interface
 * pointer): the gain from each source channel to each speaker and into the
 * reverb, relative to the voice's own volume (`voice_volume`, hundredths of
 * a dB, which decides where the hardware's attenuation runs out), the
 * Doppler shift as mix->ratio, and the voice's filter. *route is the mix-in
 * buffer it plays into (SetOutputBuffer), 0 for the speakers.
 *
 * Returns 0 when the title has said nothing about this voice -- no 3D, no
 * mix bins, no filter, no I3DL2 source, no output buffer -- so the caller
 * leaves the slot alone and the sound plays exactly as it did before this
 * file existed. `force` computes the default mix instead (a mix-in buffer's
 * own submix needs one whatever was said). */
int hle_ds3d_mix(uint32_t voice, uint32_t channels, int32_t voice_volume, int force,
                 RecompAudioMix *mix, uint32_t *route);

/* The mix-in buffers (DSBCAPS_MIXIN) that exist: up to `max` interface
 * pointers into `keys`; returns how many. */
uint32_t hle_ds3d_mixins(uint32_t *keys, uint32_t max);

/* Called by hle_dsound.c's service thread, outside any lock of its own:
 * hands a changed I3DL2 room (SetI3DL2Listener) to the host's reverb. */
void hle_ds3d_service(void);

/* SetEG's DSENVELOPEDESC (which: 0 amplitude, 1 multi -- this file's
 * numbering, not DSENVELOPEDESC.dwEG's, where amplitude is 1), as the title
 * last gave it for this voice; returns 0 if it never did. `out` is the ten
 * DWORDs of the structure. */
int hle_ds3d_envelope(uint32_t voice, uint32_t which, uint32_t out[10]);

/* The frequency a stream was asked to play at (SetFrequency, or SetPitch as
 * a frequency), in Hz; 0 for its own rate. Buffers keep theirs in
 * hle_dsound.c's model. */
uint32_t hle_ds3d_stream_frequency(uint32_t stream);

/* A new buffer or stream at this interface pointer: whatever was known about
 * an earlier one there is gone. `flags` is the DSBUFFERDESC/DSSTREAMDESC
 * dwFlags; `mixbins` a guest LPCDSMIXBINS or 0. */
void hle_ds3d_created(uint32_t voice, uint32_t flags, uint32_t mixbins);

/* Xbox pitch (1/4096 octave relative to 48 kHz) as a frequency in Hz. */
uint32_t hle_ds3d_pitch_to_frequency(int32_t pitch);

#endif
