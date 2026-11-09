// RGBA8 GL image -> straight RGBA CUDA input and independent lossless alpha.
#include <cuda_runtime.h>
extern "C" __global__ void wb_straight_rgba(cudaTextureObject_t source, unsigned char *output,
    unsigned long long pitch, unsigned char *alpha, unsigned int *opaque,
    int width, int height, int coded_width, int coded_height, int flip) {
    __shared__ unsigned int transparent;
    if (threadIdx.x == 0 && threadIdx.y == 0) transparent = 0;
    __syncthreads();
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x < coded_width && y < coded_height) {
        int sx = min(x, width - 1), sy = min(y, height - 1);
        if (flip) sy = height - 1 - sy;
        uchar4 pixel = tex2D<uchar4>(source, float(sx) + 0.5f, float(sy) + 0.5f);
        if (x < width && y < height) {
            alpha[(size_t)y * width + x] = pixel.w;
            if (pixel.w != 255) atomicExch(&transparent, 1);
        }
        if (pixel.w != 255) {
            unsigned int a = pixel.w;
            pixel.x = a ? min(255u, (pixel.x * 255u + a / 2u) / a) : 0;
            pixel.y = a ? min(255u, (pixel.y * 255u + a / 2u) / a) : 0;
            pixel.z = a ? min(255u, (pixel.z * 255u + a / 2u) / a) : 0;
        }
        *reinterpret_cast<uchar4 *>(output + (size_t)y * pitch + x * 4) = pixel;
    }
    __syncthreads();
    if (threadIdx.x == 0 && threadIdx.y == 0 && transparent) atomicExch(opaque, 0);
}
