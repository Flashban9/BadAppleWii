#ifndef DECODE_H
#define DECODE_H
#include <stdint.h>
#include <stddef.h>

/* Decodes one frame (XOR delta + zero-run RLE) on top of the previous frame.
 * frame: bitmap (w*h/8 bytes), updated in place.
 * Returns: 0 = ok, -1 = corrupt data. */
static inline int decode_frame(const uint8_t *src, size_t len, uint8_t *frame, size_t frame_bytes)
{
    size_t i = 0, pos = 0;
    while (i < len) {
        uint8_t b = src[i++];
        if (b) {
            if (pos >= frame_bytes) return -1;
            frame[pos++] ^= b;
        } else {
            if (i >= len) return -1;
            size_t run = src[i++];
            if (pos + run > frame_bytes) return -1;
            pos += run;            /* zeros: XOR changes nothing */
        }
    }
    return pos == frame_bytes ? 0 : -1;
}
#endif
