#include "rendering/PngStripWriter.h"
#include "rendering/ImageReadback.h"
#include <chrono>
#include <cassert>
#include <fstream>
#include <iostream>
int main() {
  std::vector<unsigned char> bottomUp{9,9,9,255, 8,8,8,255, 0,0,255,255};
  rendering::normalizeBgraReadback(bottomUp,1,3,true);
  assert(bottomUp[0]==0 && bottomUp[2]==255 && bottomUp[8]==9);
  const auto normalized=bottomUp;
  rendering::normalizeBgraReadback(bottomUp,1,3,false);
  assert(bottomUp==normalized);
  const auto path = std::filesystem::temp_directory_path() / ("asobmashow-receipt-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".png");
  {
    PngStripWriter writer(path, 2, 3);
    const std::vector<unsigned char> red{0,0,255,255,0,0,255,255};
    const std::vector<unsigned char> blue{255,0,0,255,255,0,0,255};
    assert(writer.append(red,1)); assert(writer.append(blue,1)); assert(writer.append(red,1));
    assert(writer.finish());
  }
  std::ifstream file(path,std::ios::binary);
  std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)),{}), compressed;
  const auto u32 = [&](std::size_t i) { return (unsigned(bytes[i])<<24)|(unsigned(bytes[i+1])<<16)|(unsigned(bytes[i+2])<<8)|bytes[i+3]; };
  assert(bytes.size()>40 && u32(16)==2 && u32(20)==3);
  for (std::size_t i=8;i+12<=bytes.size();) {
    auto length=u32(i);
    auto crc=crc32(0,bytes.data()+i+4,length+4);
    assert(crc==u32(i+8+length));
    if (std::string(reinterpret_cast<char*>(bytes.data()+i+4),4)=="IDAT")
      compressed.insert(compressed.end(),bytes.begin()+i+8,bytes.begin()+i+8+length);
    i+=12+length;
  }
  unsigned char pixels[21]{}; uLongf size=sizeof(pixels);
  assert(uncompress(pixels,&size,compressed.data(),compressed.size())==Z_OK && size==21);
  assert(pixels[1]==255 && pixels[3]==0 && pixels[8]==0 && pixels[10]==255 && pixels[15]==255);
  std::filesystem::remove(path);
  {
    PngStripWriter unfinished(path,2,3);
    assert(!unfinished.finish());
  }
  assert(!std::filesystem::exists(path) && !std::filesystem::exists(path.string()+".partial"));
  {
    PngStripWriter tall(path,1,70000);
    std::vector<unsigned char> strip(1000*4,255);
    for (int i=0;i<70;++i) assert(tall.append(strip,1000));
    assert(!tall.append(strip,1)); // Extra rows rejected, never silently truncated.
  }
  assert(!std::filesystem::exists(path));
  {
    PngStripWriter tall(path,1,70000);
    std::vector<unsigned char> strip(1000*4,255);
    for (int i=0;i<70;++i) assert(tall.append(strip,1000));
    assert(tall.finish());
  }
  std::filesystem::remove(path);
  std::cout << "Receipt PNG tests passed\n";
}
