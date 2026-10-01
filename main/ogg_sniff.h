/*
 * Ogg page walker over the raw stream bytes, run by the network task before
 * bytes enter the ring. Three jobs:
 *
 *  - report where each BOS page starts, as an absolute stream offset, so
 *    the player resets its decoder exactly there (a reconnect, or a chained
 *    stream when the source restarts or retags);
 *  - pull TITLE / ARTIST out of OpusTags, since Icecast does not interleave
 *    ICY metadata into Ogg streams -- the tags are the metadata;
 *  - report every page's granule position (on_page), which is how downlink
 *    measures its buffer in time rather than bytes;
 *  - renumber page sequence numbers in place (opt-in). A listener joining mid-stream
 *    gets Icecast's stored header pages (seq 0, 1) followed by live pages
 *    (seq 625...), and micro-ogg-demuxer treats that gap as a fatal error
 *    where libogg would note a hole and carry on. The CRC is left stale;
 *    the decoder runs with CRC checking off.
 *
 * OpusTags is parsed from the first page of the packet only (capped at
 * OGG_SNIFF_CAP bytes); a tag packet large enough to span pages (cover art)
 * yields whatever fits.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OGG_SNIFF_CAP 2048

typedef struct {
    void (*on_bos)(uint64_t page_offset, void *ctx);
    void (*on_tags)(const char *vendor, const char *title, const char *artist, void *ctx);
    /* First 8 bytes of a BOS page's body: the codec's header magic
     * ("OpusHead", "\x01vorbis", ...). */
    void (*on_codec)(const uint8_t magic[8], void *ctx);
    /* Every page, as its header completes. `granule` is the page's granule
     * position (samples at 48 kHz for Opus), -1 when no packet ends on it. */
    void (*on_page)(uint64_t page_offset, uint32_t serial, int64_t granule, uint8_t flags, void *ctx);
    void *ctx;
    bool renumber;    /* rewrite page sequence numbers (see above) */

    /* private */
    int      state;
    uint8_t  hdr[27 + 255];
    size_t   hlen, need;
    uint64_t page_start;
    size_t   body_left, body_seen;
    uint32_t seq_next;
    bool     is_tags;
    uint8_t  cap[OGG_SNIFF_CAP];
    size_t   cap_len;
} ogg_sniff_t;

void ogg_sniff_reset(ogg_sniff_t *s);
/* `offset` is the absolute stream offset of p[0]. With `renumber`, rewrites
 * page sequence bytes in p. */
void ogg_sniff_feed(ogg_sniff_t *s, uint8_t *p, size_t n, uint64_t offset);

#ifdef __cplusplus
}
#endif
