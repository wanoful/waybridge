#pragma once
#include <stddef.h>
#include <stdint.h>
struct wb_gpu_device;
struct wb_gpu_source;
struct wb_gpu_frame;
/* Optional caller-owned lifetime guard. Each native pool retains its own
 * reference and releases it only after its cached buffers are destroyed.
 * Both callbacks must be thread-safe and must not throw or unwind. */
struct wb_gpu_owner {
    void *opaque;
    void (*retain)(void *opaque);
    void (*release)(void *opaque);
};
struct wb_gpu_device *wb_gpu_device_new(char *error, size_t size);
void wb_gpu_device_free(struct wb_gpu_device *device);
struct wb_gpu_source *wb_gpu_source_new(struct wb_gpu_device *device, unsigned int renderbuffer,
    int width, int height, char *error, size_t size);
struct wb_gpu_source *wb_gpu_source_new_mode(struct wb_gpu_device *device, unsigned int renderbuffer,
    int width, int height, int hdr, char *error, size_t size);
struct wb_gpu_source *wb_gpu_source_new_mode_owned(struct wb_gpu_device *device,
    unsigned int renderbuffer, int width, int height, int hdr,
    const struct wb_gpu_owner *owner, char *error, size_t size);
void wb_gpu_source_free(struct wb_gpu_source *source);
struct wb_gpu_frame *wb_gpu_capture(struct wb_gpu_source *source, int flip, char *error, size_t size);
void wb_gpu_frame_free(struct wb_gpu_frame *frame);
void *wb_gpu_frame_avframe(const struct wb_gpu_frame *frame);
const uint8_t *wb_gpu_frame_alpha(const struct wb_gpu_frame *frame, size_t *length);
size_t wb_gpu_frame_bytes(const struct wb_gpu_frame *frame);
uintptr_t wb_gpu_frame_device(const struct wb_gpu_frame *frame);
int wb_gpu_frame_download(const struct wb_gpu_frame *frame, uint8_t *rgba, size_t length);
