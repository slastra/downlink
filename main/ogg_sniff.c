#include "ogg_sniff.h"

#include <string.h>
#include <strings.h>

enum { ST_SYNC, ST_HDR, ST_SEGS, ST_BODY };

void ogg_sniff_reset(ogg_sniff_t *s)
{
    s->state = ST_SYNC;
    s->hlen = 0;
    s->body_left = 0;
    s->cap_len = 0;
    s->is_tags = false;
}

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* "KEY=value" with a case-insensitive KEY. */
static bool tag_value(const uint8_t *c, uint32_t len, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    if (len <= kl || c[kl] != '=' || strncasecmp((const char *)c, key, kl) != 0) return false;
    size_t vl = len - kl - 1;
    if (vl >= cap) vl = cap - 1;
    memcpy(out, c + kl + 1, vl);
    out[vl] = 0;
    return true;
}

static void parse_tags(ogg_sniff_t *s)
{
    const uint8_t *p = s->cap + 8, *end = s->cap + s->cap_len;
    char vendor[64] = "", title[128] = "", artist[128] = "";
    if (end - p < 4) return;
    uint32_t vl = le32(p); p += 4;
    if ((uint32_t)(end - p) < vl) return;
    size_t n = vl < sizeof vendor - 1 ? vl : sizeof vendor - 1;
    memcpy(vendor, p, n);
    vendor[n] = 0;
    p += vl;
    if (end - p < 4) goto done;
    uint32_t count = le32(p); p += 4;
    for (uint32_t i = 0; i < count && end - p >= 4; i++) {
        uint32_t cl = le32(p); p += 4;
        if ((uint32_t)(end - p) < cl) break;   /* truncated by the capture cap */
        if (!tag_value(p, cl, "TITLE", title, sizeof title))
            tag_value(p, cl, "ARTIST", artist, sizeof artist);
        p += cl;
    }
done:
    if (s->on_tags) s->on_tags(vendor, title, artist, s->ctx);
}

void ogg_sniff_feed(ogg_sniff_t *s, const uint8_t *p, size_t n, uint64_t offset)
{
    static const uint8_t magic[4] = { 'O', 'g', 'g', 'S' };
    for (size_t i = 0; i < n; ) {
        switch (s->state) {
        case ST_SYNC:
            /* Hunt for the capture pattern one byte at a time. */
            if (p[i] == magic[s->hlen]) {
                if (s->hlen == 0) s->page_start = offset + i;
                s->hdr[s->hlen++] = p[i];
                if (s->hlen == 4) s->state = ST_HDR;
            } else {
                s->hlen = 0;
                if (p[i] == 'O') { s->page_start = offset + i; s->hdr[s->hlen++] = p[i]; }
            }
            i++;
            break;

        case ST_HDR:
            s->hdr[s->hlen++] = p[i++];
            if (s->hlen == 27) {
                if (s->hdr[4] != 0) { ogg_sniff_reset(s); break; }   /* version */
                s->need = s->hdr[26];
                s->state = ST_SEGS;
                if (s->need == 0) goto page_header_done;
            }
            break;

        case ST_SEGS:
            s->hdr[s->hlen++] = p[i++];
            if (--s->need == 0) goto page_header_done;
            break;

        case ST_BODY: {
            size_t take = n - i < s->body_left ? n - i : s->body_left;
            /* Capture the body start: 8 bytes to identify it, then the rest
             * only if it is OpusTags (and not a continuation of a packet). */
            size_t k = 0;
            if (s->body_seen < 8) {
                k = 8 - s->body_seen < take ? 8 - s->body_seen : take;
                memcpy(s->cap + s->cap_len, p + i, k);
                s->cap_len += k;
                if (s->cap_len == 8) {
                    s->is_tags = memcmp(s->cap, "OpusTags", 8) == 0 && !(s->hdr[5] & 0x01);
                    if ((s->hdr[5] & 0x02) && s->on_codec) s->on_codec(s->cap, s->ctx);
                }
            }
            if (s->is_tags && k < take) {
                size_t c = OGG_SNIFF_CAP - s->cap_len;
                if (c > take - k) c = take - k;
                memcpy(s->cap + s->cap_len, p + i + k, c);
                s->cap_len += c;
            }
            s->body_seen += take;
            s->body_left -= take;
            i += take;
            if (s->body_left == 0) {
                if (s->is_tags) parse_tags(s);
                ogg_sniff_reset(s);
            }
            break;
        }
        }
        continue;

    page_header_done: {
            size_t body = 0;
            for (int k = 0; k < s->hdr[26]; k++) body += s->hdr[27 + k];
            if ((s->hdr[5] & 0x02) && s->on_bos) s->on_bos(s->page_start, s->ctx);
            s->body_left = body;
            s->body_seen = 0;
            s->cap_len = 0;
            s->is_tags = false;
            s->state = ST_BODY;
            if (body == 0) ogg_sniff_reset(s);
        }
    }
}
