#ifndef DECODE_H
#define DECODE_H
#include <stdint.h>
#include <stddef.h>

/* Dekodiert einen Frame (XOR-Delta + Zero-Run-RLE) auf den vorherigen Frame.
 * frame: Bitmap (w*h/8 Bytes), wird in-place aktualisiert.
 * Rueckgabe: 0 = ok, -1 = defekte Daten. */
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
            pos += run;            /* Nullen: XOR veraendert nichts */
        }
    }
    return pos == frame_bytes ? 0 : -1;
}
#endif
