#ifndef WAYBRIDGE_UDP_FEC_H
#define WAYBRIDGE_UDP_FEC_H

#include <stddef.h>
#include <stdint.h>

#define WB_RS_CHUNK 1024
#define WB_RS_GROUP 16
#define WB_RS_PARITY 2

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _reed_solomon WbReedSolomon;
void wb_rs_initialize(void);
WbReedSolomon *wb_rs_create(void);
void wb_rs_destroy(WbReedSolomon *codec);
int wb_rs_encode(WbReedSolomon *codec, uint8_t *data, uint8_t *parity);
size_t wb_rs_recover(WbReedSolomon *codec, uint8_t *body, size_t total, uint8_t *seen,
                     size_t start, const uint8_t *parity0, const uint8_t *parity1);

#ifdef __cplusplus
}
#endif
#endif
