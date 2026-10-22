#include "common/native_image_decode.h"
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>
int main(){
    std::ifstream file(KIT_TEST_PMA_FIXTURE,std::ios::binary);
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(file)),{});
    assert(!data.empty());int w=0,h=0;
    auto pixels=nativeimage::decodeEncodedPMA(data.data(),data.size(),w,h);
    assert(pixels&&w==16&&h==16);
    for(int i=0;i<w*h;i++)assert(pixels[i*4]==100&&pixels[i*4+1]==50&&pixels[i*4+2]==25&&pixels[i*4+3]==128);
    std::free(pixels);
    const unsigned char invalid[]={0,1,2,3};
    assert(!nativeimage::decodeEncodedPMA(invalid,sizeof(invalid),w,h)&&w==0&&h==0);
}
