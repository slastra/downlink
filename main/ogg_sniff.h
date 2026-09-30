/*
 * Passive Ogg page walker over the raw stream bytes, run by the network
 * task before bytes enter the ring. It never alters the stream. Two jobs:
 *
 *  - report where each BOS page starts, as an absolute stream offset, so
 *    the player resets its decoder exactly there (a reconnect, or a chained
 *    stream when the source restarts or retags);
 *  - pull TITLE / ARTIST out of OpusTags, since Icecast does not interleave
 *    ICY metadata into Ogg streams -- the tags are the metadata.
 *
 * OpusTags is parsed from the first page of the packet only (capped at
 * OGG_SNIFF_CAP bytes); a tag packet large enough to span pages (cover art)
 * yields whatever fits.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OGG_SNIFF_CAP 2048

typedef struct {
    void (*on_bos)(uint64_t page_offset, void *ctx);
    void (*on_tags)(const char *vendor, const char *title, const char *artist, void *ctx);
    /* First 8 bytes of a BOS page's body: the codec's header magic
     * ("OpusHead", "\x01vorbis", ...). */
    void (*on_codec)(const uint8_t magic[8], void *ctx);
    void *ctx;

    /* private */
    int      state;
    uint8_t  hdr[27 + 255];
    size_t   hlen, need;
    uint64_t page_start;
    size_t   body_left, body_seen;
    bool     is_tags;
    uint8_t  cap[OGG_SNIFF_CAP];
    size_t   cap_len;
} ogg_sniff_t;

void ogg_sniff_reset(ogg_sniff_t *s);
/* `offset` is the absolute stream offset of p[0]. */
void ogg_sniff_feed(ogg_sniff_t *s, const uint8_t *p, size_t n, uint64_t offset);
