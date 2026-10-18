#include "../src/storage/StorageVideoCapabilities.h"
#include <iostream>
#include <stdexcept>
extern "C" {
#include <libavcodec/avcodec.h>
}

int main() {
  using namespace storage_management;
  try {
    std::promise<bool> wake;
    auto woken = wake.get_future();
    const auto first = RequestStorageVideoCapabilities([&] {
      wake.set_value(RequestStorageVideoCapabilities().wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    });
    const auto second = RequestStorageVideoCapabilities();
    const auto capabilities = first.get();
    if (!woken.get()) throw std::runtime_error("capability wake runs only after results are ready");
    const auto av1Playback = avcodec_find_decoder(AV_CODEC_ID_AV1);
    if (!av1Playback || std::string(av1Playback->name) != "libdav1d")
      throw std::runtime_error("AV1 playback selects the bundled CPU decoder without a hardware context");
    if (!capabilities[0].available || !capabilities[0].playbackAvailable ||
        capabilities[0].encoder.empty() || capabilities[0].decoder.empty())
      throw std::runtime_error("H264 has a working encoder and the app playback decoder, with software fallback");
    for (std::size_t i = 0; i < capabilities.size(); ++i) {
      const auto &codec = capabilities[i];
      if (static_cast<std::size_t>(codec.codec) != i ||
          (codec.available && (!codec.playbackAvailable || codec.decoder.empty() || codec.encoder.empty())) ||
          (!codec.available && codec.reason.empty()))
        throw std::runtime_error("every codec reports checked encoding and playback capability or an unavailable reason");
      if (i && codec.available && !codec.hardware)
        throw std::runtime_error("HEVC and AV1 are offered only with working hardware encoding");
      if (second.get()[i].available != codec.available || second.get()[i].encoder != codec.encoder)
        throw std::runtime_error("concurrent capability consumers receive the same completed probe");
      std::cout << i << " encoder=" << codec.encoder << " playback=" << codec.decoder
                << " available=" << codec.available << '\n';
    }
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
