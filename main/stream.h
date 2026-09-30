/*
 * Network side: an HTTP client task that pulls the Icecast mount into a
 * byte ring in PSRAM. The player drains the ring on the other core.
 *
 * Stream offsets are absolute byte counts since boot, across reconnects.
 * Every point where the decoder must start over (a new connection, or a BOS
 * page inside one) is queued as a boundary offset; the player resets its
 * decoder when its read position reaches the head of that queue.
 *
 * ICY: the request asks for Icy-MetaData. If the server answers with
 * icy-metaint (MP3/AAC mounts), the metadata blocks are stripped from the
 * audio and StreamTitle is logged. Ogg mounts carry titles in OpusTags
 * instead, which ogg_sniff reports.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

void stream_start(const char *url);
/* netlink up/down. The task only connects while the link is up. */
void stream_set_link(bool up);

StreamBufferHandle_t stream_ring(void);
bool stream_boundary_peek(uint64_t *offset);
void stream_boundary_pop(void);

/* Drop the connection and start over (the player calls this when the
 * decoder cannot resynchronise on its own). */
void stream_reconnect(const char *why);

/* For the status LED. */
bool stream_connected(void);
bool stream_codec_rejected(void);   /* last connection was Ogg but not Opus */

/* One line for the heartbeat. */
void stream_status(char *out, size_t len);

#ifdef __cplusplus
}
#endif
