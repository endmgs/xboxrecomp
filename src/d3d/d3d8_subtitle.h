/*
 * d3d8_subtitle.h -- subtitles drawn over the finished frame from a table
 * (subtitles.txt beside the settings), started when a DirectSound stream is
 * handed audio whose fingerprint is in subtitle_index.txt. See
 * d3d8_subtitle.c.
 */
#ifndef D3D8_SUBTITLE_H
#define D3D8_SUBTITLE_H

#include <stdint.h>

/* From the DirectSound stream HLE: a packet's data as the title queued it. */
void d3d8_subtitle_audio_packet(const void *data, uint32_t size);

/* Each frame, after the title's own drawing and before present. */
void d3d8_subtitle_draw(void);

#endif
