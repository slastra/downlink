/*
 * ogg_sniff against hand-built Ogg pages, fed in every chunking from one
 * byte at a time up to the whole stream: BOS offsets, codec magic, OpusTags,
 * and the page renumbering that lets an Icecast listener join mid-stream.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ogg_sniff.h"

static uint8_t  stream[16384];
static size_t   len;
static int64_t  next_granule;    /* granule page() stamps on the next page */

/* One page holding one packet (< 255 * 255 bytes). CRC left zero: the
 * sniffer never checks it, nor does the decoder as downlink runs it. */
static size_t page(uint8_t flags, uint32_t serial, uint32_t seq, const void *body, size_t n)
{
    size_t start = len;
    uint8_t *p = stream + len;
    memcpy(p, "OggS", 4);
    p[4] = 0;
    p[5] = flags;
    for (int i = 0; i < 8; i++) p[6 + i] = (uint64_t)next_granule >> (8 * i);
    for (int i = 0; i < 4; i++) p[14 + i] = serial >> (8 * i);
    for (int i = 0; i < 4; i++) p[18 + i] = seq >> (8 * i);
    memset(p + 22, 0, 4);                                 /* crc */
    size_t segs = n / 255 + 1;
    p[26] = (uint8_t)segs;
    for (size_t i = 0; i < segs; i++) p[27 + i] = i + 1 < segs ? 255 : n % 255;
    memcpy(p + 27 + segs, body, n);
    len += 27 + segs + n;
    return start;
}

static size_t opus_tags(uint8_t *out, const char **comments, int n)
{
    size_t o = 0;
    memcpy(out, "OpusTags", 8); o += 8;
    const char *vendor = "RUMP";
    uint32_t vl = strlen(vendor);
    memcpy(out + o, &vl, 4); o += 4;
    memcpy(out + o, vendor, vl); o += vl;
    uint32_t cn = n;
    memcpy(out + o, &cn, 4); o += 4;
    for (int i = 0; i < n; i++) {
        uint32_t cl = strlen(comments[i]);
        memcpy(out + o, &cl, 4); o += 4;
        memcpy(out + o, comments[i], cl); o += cl;
    }
    return o;
}

/* ---- what the sniffer reported ---------------------------------- */

static uint64_t bos[8];   static int nbos;
static char     codecs[8][9]; static int ncodec;
static char     titles[8][64]; static int ntitle;

static uint64_t pg_off[32]; static int64_t pg_gran[32]; static uint32_t pg_serial[32]; static int npg;

static void on_bos(uint64_t off, void *ctx)   { (void)ctx; bos[nbos++] = off; }
static void on_page(uint64_t off, uint32_t serial, int64_t g, uint8_t flags, void *ctx)
{
    (void)ctx; (void)flags;
    if (npg == 32) return;
    pg_off[npg] = off; pg_serial[npg] = serial; pg_gran[npg++] = g;
}
static void on_codec(const uint8_t m[8], void *ctx) { (void)ctx; memcpy(codecs[ncodec], m, 8); codecs[ncodec++][8] = 0; }
static void on_tags(const char *v, const char *t, const char *a, void *ctx)
{
    (void)ctx;
    snprintf(titles[ntitle++], 64, "%s|%s|%s", v, a, t);
}

static uint32_t seq_at(const uint8_t *s, size_t page_off)
{
    const uint8_t *p = s + page_off + 18;
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

int main(void)
{
    uint8_t body[512];
    const char *c1[] = { "TITLE=It's One", "ARTIST=Angelo" };
    const char *c2[] = { "title=Two" };                  /* keys are case-insensitive */
    uint8_t head[19] = "OpusHead";
    uint8_t audio[300];
    memset(audio, 0x5a, sizeof audio);

    /* A mid-stream join: headers 0, 1 then live pages 625, 626 -- then a
     * chained stream (new serial, BOS) whose pages restart at 0. */
    size_t p[8]; int np = 0;
    const int64_t gran[] = { 0, 0, 2995200, 3000000, 3004800, 0, 0, -1 };
    const uint32_t ser[] = { 7, 7, 7, 7, 7, 9, 9, 9 };
    next_granule = gran[np]; p[np++] = page(0x02, 7, 0, head, sizeof head);
    next_granule = gran[np]; p[np++] = page(0x00, 7, 1, body, opus_tags(body, c1, 2));
    next_granule = gran[np]; p[np++] = page(0x00, 7, 625, audio, sizeof audio);
    next_granule = gran[np]; p[np++] = page(0x00, 7, 626, audio, 200);
    next_granule = gran[np]; p[np++] = page(0x04, 7, 627, audio, 10);             /* EOS */
    next_granule = gran[np]; p[np++] = page(0x02, 9, 0, head, sizeof head);
    next_granule = gran[np]; p[np++] = page(0x00, 9, 1, body, opus_tags(body, c2, 1));
    next_granule = gran[np]; p[np++] = page(0x00, 9, 2, audio, 255);              /* body exactly 255: two lacing values */
    const uint32_t want_seq[] = { 0, 1, 2, 3, 4, 0, 1, 2 };

    int runs = 0;
    for (size_t chunk = 1; chunk <= len; chunk += chunk < 64 ? 1 : 97) {
        static uint8_t work[sizeof stream];
        memcpy(work, stream, len);
        nbos = ncodec = ntitle = npg = 0;
        ogg_sniff_t s = { .on_bos = on_bos, .on_tags = on_tags, .on_codec = on_codec, .on_page = on_page, .renumber = true };
        ogg_sniff_reset(&s);
        for (size_t i = 0; i < len; i += chunk) {
            size_t k = len - i < chunk ? len - i : chunk;
            ogg_sniff_feed(&s, work + i, k, 1000 + i);
        }
        assert(nbos == 2 && bos[0] == 1000 + p[0] && bos[1] == 1000 + p[5]);
        assert(ncodec == 2 && !strcmp(codecs[0], "OpusHead") && !strcmp(codecs[1], "OpusHead"));
        assert(ntitle == 2);
        assert(!strcmp(titles[0], "RUMP|Angelo|It's One"));
        assert(!strcmp(titles[1], "RUMP||Two"));
        for (int i = 0; i < np; i++) assert(seq_at(work, p[i]) == want_seq[i]);
        assert(npg == np);
        for (int i = 0; i < np; i++)
            assert(pg_off[i] == 1000 + p[i] && pg_gran[i] == gran[i] && pg_serial[i] == ser[i]);
        /* Only the sequence bytes may change. */
        for (size_t i = 0; i < len; i++) {
            int in_seq = 0;
            for (int j = 0; j < np; j++) if (i >= p[j] + 18 && i < p[j] + 22) in_seq = 1;
            assert(in_seq || work[i] == stream[i]);
        }
        runs++;
    }

    /* renumber off: pages reported, bytes untouched. */
    {
        static uint8_t work[sizeof stream];
        memcpy(work, stream, len);
        npg = 0;
        ogg_sniff_t s = { .on_page = on_page };
        ogg_sniff_reset(&s);
        ogg_sniff_feed(&s, work, len, 0);
        assert(npg == np && !memcmp(work, stream, len));
        runs++;
    }

    /* Junk before the first page (joining a connection mid-page). */
    {
        static uint8_t work[sizeof stream + 64];
        memcpy(work, "OgOgg\x01xxOggX", 12);
        memcpy(work + 12, stream, len);
        nbos = ncodec = ntitle = npg = 0;
        ogg_sniff_t s = { .on_bos = on_bos, .on_tags = on_tags, .on_codec = on_codec, .on_page = on_page, .renumber = true };
        ogg_sniff_reset(&s);
        ogg_sniff_feed(&s, work, len + 12, 0);
        assert(nbos == 2 && bos[0] == 12 && ntitle == 2);
        runs++;
    }

    /* A Vorbis BOS reports its magic so the stream task can refuse it. */
    {
        len = 0;
        uint8_t vh[30] = "\x01vorbis";
        next_granule = 0;
        page(0x02, 3, 0, vh, sizeof vh);
        nbos = ncodec = ntitle = npg = 0;
        ogg_sniff_t s = { .on_bos = on_bos, .on_tags = on_tags, .on_codec = on_codec, .on_page = on_page, .renumber = true };
        ogg_sniff_reset(&s);
        ogg_sniff_feed(&s, stream, len, 0);
        assert(ncodec == 1 && !memcmp(codecs[0], "\x01vorbis", 7));
        runs++;
    }

    printf("ogg_sniff: %d runs passed\n", runs);
    return 0;
}
