// Native adapter fixture generator. Existing wb_encoder_new ABI remains separately covered.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
struct wb_encoder;
struct wb_encoder *wb_encoder_new_codec(int, int, int, int, int, char *, size_t);
int wb_encoder_encode(struct wb_encoder *, const uint8_t *, int64_t, int, const uint8_t **,
                      int64_t *, int *);
void wb_encoder_free(struct wb_encoder *);
int main(int argc, char **argv) {
  if (argc != 5)
    return 2;
  int kind = atoi(argv[1]), width = atoi(argv[2]), height = atoi(argv[3]);
  char error[256] = {0};
  struct wb_encoder *encoder =
      wb_encoder_new_codec(kind, width, height, 60, 30000, error, sizeof(error));
  if (!encoder) {
    fprintf(stderr, "%s\n", error);
    return 1;
  }
  uint8_t *rgba = malloc((size_t)width * height * 4);
  if (!rgba)
    return 2;
  for (int i = 0; i < width * height; ++i) {
    rgba[i * 4] = 0;
    rgba[i * 4 + 1] = 255;
    rgba[i * 4 + 2] = 0;
    rgba[i * 4 + 3] = 255;
  }
  FILE *file = fopen(argv[4], "wb");
  if (!file)
    return 2;
  for (int i = 0; i < 6; ++i) {
    const uint8_t *output = NULL;
    int64_t pts = -1;
    int key = 0;
    // Reusing the very same encoder must yield another independently decodable key frame.
    int length = wb_encoder_encode(encoder, rgba, i + 1, i == 0 || i == 3, &output, &pts, &key);
    if (length <= 0 || pts != i + 1 || ((i == 0 || i == 3) && !key)) {
      fprintf(stderr, "encode kind=%d frame=%d len=%d pts=%lld key=%d\n", kind, i, length,
              (long long)pts, key);
      return 1;
    }
    printf("%d %d %d\n", i + 1, length, key);
    if (fwrite(output, 1, length, file) != (size_t)length)
      return 2;
  }
  fclose(file);
  free(rgba);
  wb_encoder_free(encoder);
  return 0;
}
