#include "native_image_decode.h"
#include <cstdlib>
#include <cstring>
#include <climits>
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "../../third_party/stb/stb_image.h"
namespace nativeimage {
unsigned char* decodeEncodedPMA(const void* data, size_t size, int& width, int& height) {
    width=height=0;
    if(!data || size>INT_MAX)return nullptr;
    int w=0,h=0,channels=0;
    auto bytes=static_cast<const unsigned char*>(data);
    if(!stbi_info_from_memory(bytes,static_cast<int>(size),&w,&h,&channels)||w<=0||h<=0||w>16384||h>16384)return nullptr;
    unsigned char* decoded=stbi_load_from_memory(bytes,static_cast<int>(size),&w,&h,&channels,4);
    if(!decoded)return nullptr;
    const size_t row=static_cast<size_t>(w)*4;
    auto out=static_cast<unsigned char*>(std::malloc(row*h));
    if(out)for(int y=0;y<h;y++)std::memcpy(out+row*y,decoded+row*(h-1-y),row);
    stbi_image_free(decoded);
    if(out){width=w;height=h;}
    return out;
}
}
