// Build with native/nvenc.c and pkg-config libavcodec/libavutil on Linux.
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

struct wb_encoder;
struct wb_encoder *wb_encoder_new(int, int, int, int, char *, size_t);
int wb_encoder_encode(struct wb_encoder *, const uint8_t *, int64_t, int,
                      const uint8_t **, int64_t *, int *);
void wb_encoder_free(struct wb_encoder *);

static double now_ms(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return time.tv_sec * 1000.0 + time.tv_nsec / 1000000.0;
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 5) {
        fprintf(stderr, "Usage: generate-resize-stream OUTPUT.h264 OUTPUT.json [FRAMES_PER_SIZE] [CACHE_SIZES:0|1]\n");
        return 2;
    }
    int per_size = argc >= 4 ? atoi(argv[3]) : 1;
    int reuse = argc == 5 ? atoi(argv[4]) : 0;
    if (per_size < 1 || per_size > 120) return 2;
    FILE *stream = fopen(argv[1], "wb"), *metadata = fopen(argv[2], "w");
    if (!stream || !metadata) return 2;
    const int segments = 11;
    struct wb_encoder *cached[2] = {NULL, NULL};
    double create_ms[11], destroy_ms[11];
    fprintf(metadata, "{\"fps\":60,\"switches\":10,\"cache_sizes\":%d,\"frames_per_size\":%d,\"frames\":[\n", reuse, per_size);
    long offset = 0;
    int64_t sequence = 0;
    for (int segment = 0; segment < segments; ++segment) {
        int width = segment % 2 ? 2240 : 960, height = segment % 2 ? 1400 : 640;
        char error[256] = {0};
        double started = now_ms();
        struct wb_encoder *encoder = reuse ? cached[segment % 2] : NULL;
        if (!encoder) encoder = wb_encoder_new(width, height, 60, 30000, error, sizeof(error));
        create_ms[segment] = now_ms() - started;
        if (!encoder) { fprintf(stderr, "%s\n", error); return 1; }
        if (reuse) cached[segment % 2] = encoder;
        uint8_t *rgba = malloc((size_t)width * height * 4);
        if (!rgba) return 2;
        for (int frame = 0; frame < per_size; ++frame) {
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                uint8_t *p = rgba + ((size_t)y * width + x) * 4;
                p[0] = (uint8_t)(x / 8 + segment * 13 + frame * 3);
                p[1] = (uint8_t)(y / 6 + segment * 19 + frame * 5);
                p[2] = (uint8_t)((x + y) / 16 + segment * 7);
                p[3] = 255;
            }
            const uint8_t *output = NULL;
            int64_t pts = 0;
            int key = 0;
            ++sequence;
            started = now_ms();
            int length = wb_encoder_encode(encoder, rgba, sequence, frame == 0, &output, &pts, &key);
            double encode_ms = now_ms() - started;
            if (length <= 0 || pts != sequence || (frame == 0 && !key)) {
                fprintf(stderr, "encode failed at %lld: length=%d key=%d pts=%lld\n",
                        (long long)sequence, length, key, (long long)pts);
                return 1;
            }
            if (fwrite(output, 1, length, stream) != (size_t)length) return 2;
            fprintf(metadata, "%s{\"offset\":%ld,\"length\":%d,\"sequence\":%lld,\"epoch\":%d,\"width\":%d,\"height\":%d,\"coded_width\":%d,\"coded_height\":%d,\"key\":%d,\"encode_ms\":%.6f}",
                    sequence > 1 ? ",\n" : "", offset, length, (long long)sequence,
                    segment + 1, width, height, width, height, key, encode_ms);
            offset += length;
        }
        free(rgba);
        started = now_ms();
        if (!reuse) wb_encoder_free(encoder);
        destroy_ms[segment] = now_ms() - started;
    }
    double cleanup_started = now_ms();
    for (int i = 0; i < 2; ++i) wb_encoder_free(cached[i]);
    double cleanup_ms = now_ms() - cleanup_started;
    fprintf(metadata, "\n],\"encoder_timings\":[\n");
    for (int segment = 0; segment < segments; ++segment)
        fprintf(metadata, "%s{\"epoch\":%d,\"create_ms\":%.6f,\"destroy_ms\":%.6f}",
                segment ? ",\n" : "", segment + 1, create_ms[segment], destroy_ms[segment]);
    fprintf(metadata, "\n],\"cache_cleanup_ms\":%.6f}\n", cleanup_ms);
    fclose(metadata); fclose(stream);
    printf("Generated %lld access units, 10 switches: 960x640 <-> 2240x1400 (%ld bytes)\n",
           (long long)sequence, offset);
    return 0;
}
