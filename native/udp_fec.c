#include "udp_fec.h"
#include "rs.h"
#include <string.h>

void wb_rs_initialize(void) { reed_solomon_init(); }
WbReedSolomon *wb_rs_create(void) { return reed_solomon_new(WB_RS_GROUP, WB_RS_PARITY); }
void wb_rs_destroy(WbReedSolomon *codec) { reed_solomon_release(codec); }

int wb_rs_encode(WbReedSolomon *codec, uint8_t *data, uint8_t *parity) {
    uint8_t *pieces[WB_RS_GROUP + WB_RS_PARITY];
    for (size_t i = 0; i < WB_RS_GROUP; ++i)
        pieces[i] = data + i * WB_RS_CHUNK;
    for (size_t i = 0; i < WB_RS_PARITY; ++i)
        pieces[WB_RS_GROUP + i] = parity + i * WB_RS_CHUNK;
    return reed_solomon_encode(codec, pieces, WB_RS_GROUP + WB_RS_PARITY, WB_RS_CHUNK);
}

size_t wb_rs_recover(WbReedSolomon *codec, uint8_t *body, size_t total, uint8_t *seen,
                     size_t start, const uint8_t *parity0, const uint8_t *parity1) {
    if (!total || !codec || start % WB_RS_GROUP)
        return 0;
    size_t chunks = (total - 1) / WB_RS_CHUNK + 1;
    if (start >= chunks)
        return 0;
    size_t count = chunks - start < WB_RS_GROUP ? chunks - start : WB_RS_GROUP;
    unsigned missing = 0;
    uint8_t marks[WB_RS_GROUP + WB_RS_PARITY] = {0};
    for (size_t i = 0; i < count; ++i) {
        marks[i] = !seen[start + i];
        missing += marks[i];
    }
    unsigned available = (parity0 != NULL) + (parity1 != NULL);
    if (!missing || missing > available)
        return 0;

    uint8_t *pieces[WB_RS_GROUP + WB_RS_PARITY];
    uint8_t zeros[WB_RS_CHUNK] = {0}, tail[WB_RS_CHUNK] = {0};
    size_t remainder = total % WB_RS_CHUNK;
    for (size_t i = 0; i < WB_RS_GROUP; ++i) {
        if (i >= count) {
            pieces[i] = zeros;
        } else if (remainder && start + i + 1 == chunks) {
            if (!marks[i]) memcpy(tail, body + (start + i) * WB_RS_CHUNK, remainder);
            pieces[i] = tail;
        } else {
            pieces[i] = body + (start + i) * WB_RS_CHUNK;
        }
    }
    marks[WB_RS_GROUP] = !parity0;
    marks[WB_RS_GROUP + 1] = !parity1;
    // nanors reads parity shards without modifying them.
    pieces[WB_RS_GROUP] = parity0 ? (uint8_t *)parity0 : zeros;
    pieces[WB_RS_GROUP + 1] = parity1 ? (uint8_t *)parity1 : zeros;
    if (reed_solomon_reconstruct(codec, pieces, marks, WB_RS_GROUP + WB_RS_PARITY, WB_RS_CHUNK))
        return 0;

    for (size_t i = 0; i < count; ++i) {
        if (pieces[i] == tail && marks[i])
            memcpy(body + (start + i) * WB_RS_CHUNK, tail, remainder);
        seen[start + i] = 1;
    }
    return missing;
}
