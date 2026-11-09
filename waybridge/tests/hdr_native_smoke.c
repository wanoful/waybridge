#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include "../native/gpu_interop.h"
struct wb_encoder;
struct wb_encoder *wb_encoder_new_gpu_codec(int, int, int, int, int, const void *, char *, size_t);
int wb_encoder_encode_gpu(struct wb_encoder *, void *, int64_t, int, const uint8_t **, int64_t *,
                          int *);
void wb_encoder_free(struct wb_encoder *);
static void check(int okay, const char *what) {
  if (!okay) {
    fprintf(stderr, "FAIL %s EGL=%x GL=%x\n", what, eglGetError(), glGetError());
    exit(1);
  }
}
static uint16_t half(float f) {
  _Float16 h = (_Float16)f;
  uint16_t bits;
  memcpy(&bits, &h, 2);
  return bits;
}
static float unhalf(uint16_t bits) {
  _Float16 h;
  memcpy(&h, &bits, 2);
  return (float)h;
}
static double pq(double v) {
  double p = pow(fmax(v, 0.) * .008, 2610. / 16384.);
  return pow((3424. / 4096. + 2413. / 128. * p) / (1. + 2392. / 128. * p), 2523. / 32.);
}
static void color(const uint16_t *p, double q[3]) {
  double a = unhalf(p[3]);
  double r = unhalf(p[0]) / a, g = unhalf(p[1]) / a, b = unhalf(p[2]) / a;
  q[0] = pq(.6274039 * r + .3292830 * g + .0433131 * b);
  q[1] = pq(.0690973 * r + .9195404 * g + .0113623 * b);
  q[2] = pq(.0163914 * r + .0880133 * g + .8955953 * b);
}
int main(void) {
  PFNEGLQUERYDEVICESEXTPROC devices_fn =
      (PFNEGLQUERYDEVICESEXTPROC)eglGetProcAddress("eglQueryDevicesEXT");
  PFNEGLGETPLATFORMDISPLAYEXTPROC platform_fn =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
  PFNEGLQUERYDEVICESTRINGEXTPROC string_fn =
      (PFNEGLQUERYDEVICESTRINGEXTPROC)eglGetProcAddress("eglQueryDeviceStringEXT");
  EGLDeviceEXT devices[16];
  EGLint count = 0;
  check(devices_fn && platform_fn && devices_fn(16, devices, &count), "enumerate EGL devices");
  EGLDisplay display = EGL_NO_DISPLAY;
  EGLContext context = EGL_NO_CONTEXT;
  const char *node = getenv("WAYBRIDGE_GPU_RENDER_NODE");
  if (!node)
    node = "/dev/dri/renderD129";
  for (int i = 0; i < count; ++i) {
    const char *path = string_fn(devices[i], EGL_DRM_RENDER_NODE_FILE_EXT);
    if (!path || strcmp(path, node))
      continue;
    display = platform_fn(EGL_PLATFORM_DEVICE_EXT, devices[i], NULL);
    if (!eglInitialize(display, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API))
      continue;
    EGLConfig config;
    EGLint found = 0;
    EGLint attributes[] = {EGL_SURFACE_TYPE,
                           EGL_PBUFFER_BIT,
                           EGL_RENDERABLE_TYPE,
                           EGL_OPENGL_ES3_BIT,
                           EGL_RED_SIZE,
                           8,
                           EGL_GREEN_SIZE,
                           8,
                           EGL_BLUE_SIZE,
                           8,
                           EGL_ALPHA_SIZE,
                           8,
                           EGL_NONE};
    check(eglChooseConfig(display, attributes, &config, 1, &found) && found, "EGL config");
    EGLint ctx[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx);
    if (context != EGL_NO_CONTEXT &&
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context))
      break;
  }
  check(context != EGL_NO_CONTEXT, "private EGL context");
  printf("GL vendor: %s\n", glGetString(GL_VENDOR));
  int width = 641, height = 481, cw = 642, ch = 482;
  GLuint rb, fbo, texture, upload;
  glGenRenderbuffers(1, &rb);
  glBindRenderbuffer(GL_RENDERBUFFER, rb);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA16F, width, height);
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
  check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "FP16 target");
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glGenFramebuffers(1, &upload);
  glBindFramebuffer(GL_FRAMEBUFFER, upload);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
  size_t pixels_count = (size_t)width * height;
  uint16_t *input = malloc(pixels_count * 8);
  uint8_t *rgba = malloc(pixels_count * 4);
  char error[512] = {0};
  struct wb_gpu_device *device = wb_gpu_device_new(error, sizeof(error));
  if (!device) {
    fprintf(stderr, "%s\n", error);
    return 1;
  }
  struct wb_gpu_source *source =
      wb_gpu_source_new_mode(device, rb, width, height, 1, error, sizeof(error));
  if (!source) {
    fprintf(stderr, "%s\n", error);
    return 1;
  }
  for (int flip = 0; flip < 2; ++flip) {
    for (int y = 0; y < height; ++y)
      for (int x = 0; x < width; ++x) {
        uint16_t *p = input + ((size_t)y * width + x) * 4;
        p[3] = 0x3000 + (x % 1024);
        float a = unhalf(p[3]);
        p[0] = half((.5f + 20.f * x / width) * a);
        p[1] = half((1.f + 10.f * y / height) * a);
        p[2] = half((.25f + 5.f * (x + y) / (width + height)) * a);
      }
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_HALF_FLOAT, input);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, upload);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glFinish();
    check(glGetError() == GL_NO_ERROR, "FP16 upload");
    struct wb_gpu_frame *frame = wb_gpu_capture(source, flip, error, sizeof(error));
    if (!frame) {
      fprintf(stderr, "%s\n", error);
      return 1;
    }
    AVFrame *cpu = av_frame_alloc();
    cpu->format = AV_PIX_FMT_P010LE;
    check(av_hwframe_transfer_data(cpu, wb_gpu_frame_avframe(frame), 0) >= 0,
          "read P010 for oracle");
    size_t length = 0;
    const uint16_t *alpha = (const uint16_t *)wb_gpu_frame_alpha(frame, &length);
    check(length == pixels_count * 2, "alpha16 length");
    for (int y = 0; y < ch; ++y)
      for (int x = 0; x < cw; ++x) {
        int sx = x < width ? x : width - 1, sy = y < height ? y : height - 1;
        if (flip)
          sy = height - 1 - sy;
        const uint16_t *p = input + ((size_t)sy * width + sx) * 4;
        double q[3];
        color(p, q);
        double yy = .2627 * q[0] + .6780 * q[1] + .0593 * q[2];
        uint16_t sample = *(uint16_t *)(cpu->data[0] + (size_t)y * cpu->linesize[0] + x * 2);
        check(abs((sample >> 6) - (int)llround(64 + 876 * yy)) <= 1,
              "every P010 luma/flip/padding");
        if (x < width && y < height)
          check(alpha[(size_t)y * width + x] == p[3], "every exact FP16 alpha bit");
        if (!(x & 1) && !(y & 1)) {
          double u = 0, v = 0;
          for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i) {
              int xx = x + i < width ? x + i : width - 1, yyy = y + j < height ? y + j : height - 1;
              if (flip)
                yyy = height - 1 - yyy;
              double c[3];
              color(input + ((size_t)yyy * width + xx) * 4, c);
              double Y = .2627 * c[0] + .678 * c[1] + .0593 * c[2];
              u += (c[2] - Y) / 1.8814;
              v += (c[0] - Y) / 1.4746;
            }
          const uint16_t *uv =
              (const uint16_t *)(cpu->data[1] + (size_t)(y / 2) * cpu->linesize[1] + x * 2);
          check(abs((uv[0] >> 6) - (int)llround(512 + 224 * u)) <= 1 &&
                    abs((uv[1] >> 6) - (int)llround(512 + 224 * v)) <= 1,
                "every chroma box filter");
        }
      }
    check(wb_gpu_frame_download(frame, rgba, pixels_count * 4) >= 0, "GPU SDR fallback download");
    for (int y = 0; y < height; ++y)
      for (int x = 0; x < width; ++x) {
        int sy = flip ? height - 1 - y : y;
        const uint16_t *p = input + ((size_t)sy * width + x) * 4;
        float a = unhalf(p[3]);
        double c[3] = {unhalf(p[0]) / a, unhalf(p[1]) / a, unhalf(p[2]) / a};
        double l = .2126 * c[0] + .7152 * c[1] + .0722 * c[2];
        const uint8_t *pixel = rgba + ((size_t)y * width + x) * 4;
        check(abs(pixel[3] - (int)llround(a * 255)) <= 1, "fallback alpha");
        for (int k = 0; k < 3; ++k) {
          double v = c[k] / (1 + l), s = v <= .0031308 ? 12.92 * v : 1.055 * pow(v, 1 / 2.4) - .055;
          check(abs(pixel[k] - (int)llround(fmin(s, 1) * 255)) <= 1, "fallback tone-map color");
        }
      }
    av_frame_free(&cpu);
    wb_gpu_frame_free(frame);
  }
  // Non-finite and out-of-gamut samples are rejected, not mislabeled HDR.
  for (int invalid = 0; invalid < 2; ++invalid) {
    input[0] = invalid ? 0x7e00 : half(-5.f);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_HALF_FLOAT, input);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, upload);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glFinish();
    struct wb_gpu_frame *bad = wb_gpu_capture(source, 0, error, sizeof(error));
    check(!bad, "NaN/gamut capture rejection");
  }
  wb_gpu_source_free(source);
  wb_gpu_device_free(device);
  glDeleteRenderbuffers(1, &rb);
  glDeleteTextures(1, &texture);
  glDeleteFramebuffers(1, &fbo);
  glDeleteFramebuffers(1, &upload);
  free(input);
  free(rgba);
  eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroyContext(display, context);
  eglTerminate(display);
  puts("PASS: every real FP16/CUDA P010 luma/chroma/alpha sample matches independent "
       "double-precision oracle; flip/odd padding, SDR fallback, NaN/gamut rejection");
  return 0;
}
