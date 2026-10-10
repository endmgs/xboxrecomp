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
 * pointer): the gain from each source channel to each speaker, relative to
 * the voice's own volume (`voice_volume`, hundredths of a dB, which decides
 * where the hardware's attenuation runs out), and the Doppler shift as a
 * playback-rate ratio.
 *
 * Returns 0 when the title has said nothing about where this voice goes --
 * no 3D, no mix bins -- so the caller leaves the slot's mapping alone and
 * the sound plays exactly as it did before this file existed. */
int hle_ds3d_mix(uint32_t voice, uint32_t channels, int32_t voice_volume,
                 float levels[2][RECOMP_SPEAKERS], float *doppler_ratio);

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
