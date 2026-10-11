#include "skin/beatoraja/SkinBitmapFontParser.h"
#include "FileChecksum.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
using nlohmann::json;
namespace rendering {
int window_width=1280,window_height=720,render_width=1280,render_height=720;
float widthScale=1,heightScale=1,ui_scale_x=1,ui_scale_y=1;
int ui_offset_x=0,ui_offset_y=0,ui_view_width=1280,ui_view_height=720;
}
json canonical(const skin::SkinBitmapFontParseResult &r) {
  json out={{"error",r.error},{"font",nullptr}};
  if (!r.font) return out;
  const auto &f=*r.font;
  out["font"]={{"size",f.resource.originalSize},{"lineHeight",f.lineHeight},{"base",f.base},{"width",f.pageWidth},{"height",f.pageHeight},{"margin",f.margin},{"lr2",f.lr2Font},{"auxiliary",f.auxiliaryMetricsComplete},{"pages",f.pagePaths},{"glyphs",json::array()},{"kerning",json::array()}};
  for (const auto &[c,g]:f.glyphs) out["font"]["glyphs"].push_back({(unsigned)c,(unsigned)g.codepoint,g.page,g.region.x,g.region.y,g.region.w,g.region.h,g.xOffset,g.yOffset,g.xAdvance});
  for (const auto &[key,value]:f.kerning) out["font"]["kerning"].push_back({(unsigned)key.first,(unsigned)key.second,value});
  return out;
}
int main(int argc,char **argv) {
  std::vector<std::string> inputs;
  std::vector<std::filesystem::path> paths;
  for(int i=1;i<argc;++i) for(const auto &e:std::filesystem::recursive_directory_iterator(argv[i])) if(e.is_regular_file()&&e.path().extension()==".fnt") paths.push_back(e.path());
  std::sort(paths.begin(),paths.end());
  for(const auto &path:paths) {std::ifstream in(path,std::ios::binary); inputs.emplace_back(std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>());}
  const auto files=inputs.size();
  std::mt19937 random(42);
  for (int i=0;i<5000;++i) {
    std::string s="info size=8\ncommon lineHeight=10 base=8 scaleW=64 scaleH=64 pages=1\npage id=0 file=\"a b.png\"\n";
    std::vector<std::string> attrs={"id=65","x=0","y=1","width=2","height=3","xoffset=-1","yoffset=0","xadvance=4","page=0"};
    constexpr const char *extra[]={"xadvance=-8","xadvance=","xadvance=\"12\"","ignored=\"a b c\"","width=2147483648","x=oops","ignored", "=x", "id=-1", "page=", "bad=\"no closing quote", "width =3", "xadvance=9junk", "id=128578", "id=55296", "width=-2", "ignored=abc=def", "x=2\tx=5", "height=\"3\"xoffset=4"};
    for(unsigned j=0,n=random()%12;j<n;++j) attrs.push_back(extra[random()%std::size(extra)]);
    std::shuffle(attrs.begin(),attrs.end(),random);
    s+="char "; for(const auto &a:attrs) s+=a+" ";
    s+="\nkerning first=65 second=66 amount=-1 amount=-3\r\n";
    if(i%7==0) s+="page file=\"ignored page\"\n";
    if(i%17==0) s+="common lineHeight=20 base=15 scaleW=64 scaleH=64 pages=1\npage file=last.png\n";
    if(i%41==0) s.push_back('\0');
    inputs.push_back(std::move(s));
  }
  json records=json::array();
  std::uint64_t nanos=0;
  for (std::size_t i=0;i<inputs.size();++i) {
    const auto &s=inputs[i];
    const auto start=std::chrono::steady_clock::now();
    const auto parsed=skin::parseSkinBitmapFont({.id=1,.virtualPath="fixture.fnt"},std::as_bytes(std::span(s)),skin::SkinBitmapFontSourceFormat::BmFont,skin::SkinSafetyPolicy(skin::SkinSafetyLevel::BeatorajaCompatibility));
    nanos+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
    records.push_back({{"input",file_checksum::sha256(s)},{"result",file_checksum::sha256(canonical(parsed).dump())}});
  }
  std::cout<<json{{"files",files},{"generated",5000},{"parseNanos",nanos},{"records",records}}.dump()<<'\n';
}
