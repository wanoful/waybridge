// RGBA8 GL image -> straight RGBA CUDA input and independent lossless alpha.
#include <cuda_runtime.h>
#include <cuda_fp16.h>
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

// scRGB is linear Rec.709 at 80 nits per unit. P010 is limited-range,
// non-constant-luminance BT.2020/PQ, with 2x2 box-filtered chroma.
__device__ float pq(float v) {
    float p=powf(fmaxf(v,0.f)*.008f,2610.f/16384.f);
    return powf((3424.f/4096.f+2413.f/128.f*p)/(1.f+2392.f/128.f*p),2523.f/32.f);
}
__device__ float gamma_sdr(float v) {
    v=fmaxf(v,0.f); return v<=.0031308f?12.92f*v:1.055f*powf(v,1.f/2.4f)-.055f;
}
__device__ float3 hdr_pixel(cudaTextureObject_t source,int x,int y,int width,int height,int flip,
    unsigned int *valid,float4 *original) {
    x=min(x,width-1);y=min(y,height-1);if(flip)y=height-1-y;
    float4 p=tex2D<float4>(source,float(x)+.5f,float(y)+.5f);*original=p;
    if(!isfinite(p.x)||!isfinite(p.y)||!isfinite(p.z)||!isfinite(p.w)||p.w<0.f||p.w>1.f)atomicExch(valid,0);
    float a=p.w;float3 c=make_float3(a>0?p.x/a:0,a>0?p.y/a:0,a>0?p.z/a:0);
    float3 b=make_float3(.6274039f*c.x+.3292830f*c.y+.0433131f*c.z,
        .0690973f*c.x+.9195404f*c.y+.0113623f*c.z,
        .0163914f*c.x+.0880133f*c.y+.8955953f*c.z);
    // Extended scRGB outside the supported BT.2020/0..10000 nit gamut takes
    // the SDR fallback. Do not silently clamp and label it faithful HDR.
    if(fminf(b.x,fminf(b.y,b.z)) < -.00001f || fmaxf(b.x,fmaxf(b.y,b.z))>125.f)atomicExch(valid,0);
    return make_float3(pq(b.x),pq(b.y),pq(b.z));
}
extern "C" __global__ void wb_scrgb_p010(cudaTextureObject_t source,unsigned short *luma,
    unsigned short *chroma,unsigned long long pitch,unsigned char *fallback,unsigned long long rgba_pitch,
    unsigned short *alpha,unsigned int *flags,int width,int height,int coded_width,int coded_height,int flip) {
    int x=blockIdx.x*blockDim.x+threadIdx.x,y=blockIdx.y*blockDim.y+threadIdx.y;
    if(x>=coded_width||y>=coded_height)return;
    float4 p;float3 q=hdr_pixel(source,x,y,width,height,flip,flags+1,&p);
    float Y=.2627f*q.x+.6780f*q.y+.0593f*q.z;
    *reinterpret_cast<unsigned short *>(reinterpret_cast<char *>(luma)+(size_t)y*pitch+x*2)=
        (unsigned short)(min(940,max(64,__float2int_rn(64.f+876.f*Y)))<<6);
    if(!(x&1)&&!(y&1)) {
        float u=0,v=0;
        for(int j=0;j<2;++j)for(int i=0;i<2;++i){float4 ignored;float3 c=hdr_pixel(source,x+i,y+j,width,height,flip,flags+1,&ignored);
            float yy=.2627f*c.x+.6780f*c.y+.0593f*c.z;u+=(c.z-yy)/1.8814f;v+=(c.x-yy)/1.4746f;}
        unsigned short *uv=reinterpret_cast<unsigned short *>(reinterpret_cast<char *>(chroma)+(size_t)(y/2)*pitch+x*2);
        uv[0]=(unsigned short)(min(960,max(64,__float2int_rn(512.f+224.f*u)))<<6);
        uv[1]=(unsigned short)(min(960,max(64,__float2int_rn(512.f+224.f*v)))<<6);
    }
    float a=fminf(1.f,fmaxf(0.f,p.w));
    float3 c=make_float3(a>0?fmaxf(p.x/a,0.f):0,a>0?fmaxf(p.y/a,0.f):0,a>0?fmaxf(p.z/a,0.f):0);
    float d=1.f+.2126f*c.x+.7152f*c.y+.0722f*c.z;
    uchar4 rgba=make_uchar4(__float2int_rn(fminf(gamma_sdr(c.x/d),1.f)*255.f),
        __float2int_rn(fminf(gamma_sdr(c.y/d),1.f)*255.f),__float2int_rn(fminf(gamma_sdr(c.z/d),1.f)*255.f),__float2int_rn(a*255.f));
    *reinterpret_cast<uchar4 *>(fallback+(size_t)y*rgba_pitch+x*4)=rgba;
    if(x<width && y<height){alpha[(size_t)y*width+x]=__half_as_ushort(__float2half(p.w));if(p.w!=1.f)atomicExch(flags,0);}
}
