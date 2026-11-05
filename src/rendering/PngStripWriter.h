#pragma once
#include <zlib.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

// One PNG, with bounded compression memory regardless of image height. Publish
// only after all declared rows have been received and the stream has closed.
class PngStripWriter {
public:
  PngStripWriter(std::filesystem::path path, int width, int height)
      : path_(std::move(path)), partial_(path_.string()+".partial"), width_(width), height_(height) {
    if (width<=0 || width>16384 || height<=0) { fail("Invalid receipt dimensions"); return; }
    out_.open(partial_,std::ios::binary);
    if (!out_) { fail("Cannot create receipt image"); return; }
    if (deflateInit(&stream_, Z_BEST_SPEED)!=Z_OK) { fail("Cannot initialize PNG compression"); return; }
    initialized_=true;
    const unsigned char signature[]{137,80,78,71,13,10,26,10};
    out_.write(reinterpret_cast<const char*>(signature),8);
    std::array<unsigned char,13> header{};
    encode(header.data(),width); encode(header.data()+4,height);
    header[8]=8; header[9]=2; // Opaque RGB.
    chunk("IHDR",header);
    row_.resize(static_cast<std::size_t>(width)*3+1);
  }
  ~PngStripWriter() {
    if (initialized_) deflateEnd(&stream_);
    out_.close();
    if (!finished_) { std::error_code ignored; std::filesystem::remove(partial_,ignored); }
  }
  PngStripWriter(const PngStripWriter &)=delete;
  PngStripWriter &operator=(const PngStripWriter &)=delete;
  const std::string &error() const { return error_; }
  bool append(std::span<const unsigned char> bgra, int rows) {
    if (!error_.empty() || finished_) return false;
    if (rows<=0 || rows>height_-written_ || bgra.size()!=static_cast<std::size_t>(width_)*rows*4)
      return fail("Invalid receipt strip");
    for (int y=0;y<rows;++y) {
      row_[0]=0;
      for (int x=0;x<width_;++x) {
        const auto src=(static_cast<std::size_t>(y)*width_+x)*4;
        const auto dst=static_cast<std::size_t>(x)*3+1;
        row_[dst]=bgra[src+2]; row_[dst+1]=bgra[src+1]; row_[dst+2]=bgra[src];
      }
      stream_.next_in=row_.data(); stream_.avail_in=static_cast<uInt>(row_.size());
      do { if (!pump(Z_NO_FLUSH)) return false; } while (stream_.avail_in);
    }
    written_+=rows;
    return true;
  }
  bool finish() {
    if (!error_.empty() || finished_) return false;
    if (written_!=height_) return fail("Receipt is incomplete");
    stream_.next_in=nullptr; stream_.avail_in=0;
    do { if (!pump(Z_FINISH)) return false; } while (!streamEnded_);
    if (!chunk("IEND",{})) return false;
    out_.close();
    if (out_.fail()) return fail("Failed to close receipt image");
    std::error_code error;
    std::filesystem::rename(partial_,path_,error);
    if (error) return fail("Failed to publish receipt image: "+error.message());
    finished_=true;
    return true;
  }
private:
  static void encode(unsigned char *out,std::uint32_t value) {
    for (int i=0;i<4;++i) out[i]=static_cast<unsigned char>(value>>(24-i*8));
  }
  void u32(std::uint32_t value) {
    unsigned char bytes[4]; encode(bytes,value);
    out_.write(reinterpret_cast<const char*>(bytes),4);
  }
  bool fail(std::string message) { error_=std::move(message); return false; }
  bool chunk(const char *type,std::span<const unsigned char> bytes) {
    u32(static_cast<std::uint32_t>(bytes.size())); out_.write(type,4);
    if (!bytes.empty()) out_.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
    auto crc=crc32(0,reinterpret_cast<const Bytef*>(type),4);
    if (!bytes.empty()) crc=crc32(crc,bytes.data(),static_cast<uInt>(bytes.size()));
    u32(crc);
    return out_.good() || fail("Failed to write receipt image");
  }
  bool pump(int flush) {
    stream_.next_out=compressed_.data(); stream_.avail_out=compressed_.size();
    const int result=deflate(&stream_,flush);
    if (result!=Z_OK && result!=Z_STREAM_END) return fail("PNG compression failed");
    streamEnded_=result==Z_STREAM_END;
    const auto size=compressed_.size()-stream_.avail_out;
    return !size || chunk("IDAT",std::span(compressed_.data(),size));
  }
  std::filesystem::path path_,partial_;
  int width_,height_,written_=0;
  std::ofstream out_;
  z_stream stream_{};
  bool initialized_=false,finished_=false,streamEnded_=false;
  std::string error_;
  std::vector<unsigned char> row_;
  // Leave the application thread's stack available for image decoding and
  // rendering while a receipt is open. Compression storage remains bounded.
  std::vector<unsigned char> compressed_ = std::vector<unsigned char>(65536);
};
