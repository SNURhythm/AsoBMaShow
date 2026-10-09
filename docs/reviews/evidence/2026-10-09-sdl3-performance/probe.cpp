// Identical harness compiled against each revision's actual application sources.
#include "input/SDLTouchInputSource.h"
#include "view/TextView.h"
#include "view/FontCacheSession.h"
#include "rendering/UniformCache.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <set>
#include <limits>
#include <sys/resource.h>
#if BENCH_SDL == 2
#include <SDL2/SDL_ttf.h>
constexpr auto touchDown = SDL_FINGERDOWN;
constexpr auto firstEvent = SDL_FIRSTEVENT, lastEvent = SDL_LASTEVENT;
constexpr auto touchMove = SDL_FINGERMOTION;
void destroySurface(SDL_Surface *s) { SDL_FreeSurface(s); }
#else
#include <SDL3_ttf/SDL_ttf.h>
constexpr auto touchDown = SDL_EVENT_FINGER_DOWN;
constexpr auto firstEvent = SDL_EVENT_FIRST, lastEvent = SDL_EVENT_LAST;
constexpr auto touchMove = SDL_EVENT_FINGER_MOTION;
void destroySurface(SDL_Surface *s) { SDL_DestroySurface(s); }
#endif
namespace rendering {
bgfx::VertexLayout PosTexCoord0Vertex::ms_decl;
bgfx::VertexLayout PosColorVertex::ms_decl;
bgfx::VertexLayout PosTexVertex::ms_decl;
int window_width = design_width, window_height = design_height;
int render_width = design_width, render_height = design_height;
float widthScale = 1, heightScale = 1, ui_scale_x = 1, ui_scale_y = 1;
int ui_offset_x = 0, ui_offset_y = 0, ui_view_width = design_width, ui_view_height = design_height;
}
using Clock = std::chrono::steady_clock;
std::uint64_t checksum = 0;
struct Measurement { std::string name; int operations; double nsPerOp; };
std::vector<Measurement> measurements;
template<class F> void measure(const char *name, int operations, F fn) {
  fn(); // One identical full-batch warmup, excluded from measurement.
  const auto start = Clock::now();
  fn();
  const auto ns = std::chrono::duration<double, std::nano>(Clock::now()-start).count();
  measurements.push_back({name, operations, ns / operations});
}
class TextProbe : public TextView {
public:
  using TextView::TextView;
  SDL_Surface *raster(const std::string &text) {
    ensureFontsForText(text);
    return renderFontSourceTextSurface(selectFont('A'), text);
  }
};
void require(bool result, const char *what) {
  if (!result) throw std::runtime_error(std::string(what)+": "+SDL_GetError());
}
SDL_Event makeTouch(unsigned n) {
  SDL_Event e{};
  e.type = n % 2 ? touchMove : touchDown;
#if BENCH_SDL == 2
  e.tfinger.touchId = 1; e.tfinger.fingerId = 42;
#else
  e.tfinger.touchID = 1; e.tfinger.fingerID = 42;
#endif
  e.tfinger.x = 0.25F; e.tfinger.y = 0.5F;
  return e;
}
void queuedEvents() {
  constexpr int batches = 10000, burst = 64;
  SDL_FlushEvents(firstEvent, lastEvent);
  measure("event_queue_64", batches*burst, [&] {
    unsigned observed = 0;
    for(int b=0; b<batches; ++b) {
      for(unsigned i=0; i<burst; ++i) {
        auto e=makeTouch(i);
        require(SDL_PushEvent(&e)==1, "push event");
      }
      SDL_Event e{};
      while(SDL_PollEvent(&e)) {
        if(e.type==touchDown || e.type==touchMove) ++observed;
      }
    }
    require(observed==batches*burst, "event count");
    checksum+=observed;
  });
}
int main(int argc, char **argv) {
  try {
    if(argc!=2) throw std::runtime_error("expected bundled font path");
#if BENCH_SDL == 2
    require(SDL_Init(SDL_INIT_EVENTS)==0, "init");
#else
    require(SDL_Init(SDL_INIT_EVENTS), "init");
#endif
    bgfx::Init init;
    init.type=bgfx::RendererType::Noop;
    init.resolution.width=1; init.resolution.height=1;
    require(bgfx::init(init), "bgfx noop init");
    queuedEvents();
    {
      SDLTouchInputSource source;
      std::uint64_t calls=0;
      source.setRawEventCallback([&](const SDL_Event &, std::uint64_t stamp) {
        ++calls; checksum+=(stamp!=0);
      });
      require(source.startListen(), "touch watch");
      constexpr int count=640000;
      measure("touch_watch_ingress", count, [&] {
        const auto before=calls;
        for(int n=0; n<count; ++n) {
          auto e=makeTouch(n);
          require(SDL_PushEvent(&e)==1, "touch push");
          SDL_FlushEvents(firstEvent, lastEvent);
        }
        require(calls-before==count, "ingress callback count");
      });
      source.stopListen();
    }
    std::set<std::uint64_t> uniqueTimestamps;
    constexpr int precisionEvents=500;
    for(int i=0;i<precisionEvents;++i) {
      const auto start=Clock::now();
      auto event=makeTouch(i);
      require(SDL_PushEvent(&event)==1,"precision event");
#if BENCH_SDL == 2
      uniqueTimestamps.insert(std::uint64_t(event.common.timestamp)*1000);
#else
      uniqueTimestamps.insert(event.common.timestamp/1000);
#endif
      SDL_FlushEvents(firstEvent,lastEvent);
      while(Clock::now()-start<std::chrono::microseconds(100)) {}
    }
    const std::vector<std::string> latin={
      "AsoBMaShow - Blue Zenith [ANOTHER]", "SCORE 0123456789  MAX COMBO 1234",
      "Music Select / Settings / Replay", "PERFECT GREAT GOOD BAD POOR"};
    const std::vector<std::string> cjk={
      "音楽選択 設定 リプレイ スコア 0123456789", "음악 선택 설정 리플레이 최고 기록",
      "楽曲一覧 難易度 テスト ANOTHER", "플레이 결과 PERFECT GREAT 1234"};
    {
      text_runtime::FontCacheSession session;
      TextProbe view(argv[1],24);
      view.setDeferredTextureMaterialization(true);
      std::vector<std::pair<std::string,std::vector<std::string>>> workloads={
        {"latin_repeated4",latin},{"cjk_repeated4",cjk}};
      for(const auto &[name,base]:std::vector<std::pair<std::string,std::vector<std::string>>>{{"latin_diverse128",latin},{"cjk_diverse128",cjk}}) {
        std::vector<std::string> diverse;
        for(int i=0;i<128;++i) diverse.push_back(base[i%base.size()]+" #"+std::to_string(i));
        workloads.emplace_back(name,std::move(diverse));
      }
      for(const auto &[label,texts]:workloads) {
        constexpr int layouts=16000, rasters=1000;
        const auto widthName="text_width_"+label;
        measure(widthName.c_str(),layouts,[&] {
          for(int i=0;i<layouts;++i) {
            const int width=view.measureTextWidth(texts[i%texts.size()]);
            require(width>0,"text width"); checksum+=width;
          }
        });
        const auto updateName="text_update_"+label;
        measure(updateName.c_str(),layouts,[&] {
          for(int i=0;i<layouts;++i) view.setText(texts[i%texts.size()]);
          checksum+=view.getText().size();
        });
        const auto rasterName="text_raster_"+label;
        measure(rasterName.c_str(),rasters,[&] {
          for(int i=0;i<rasters;++i) {
            auto *surface=view.raster(texts[i%texts.size()]);
            require(surface && surface->w>0 && surface->h>0,"text raster");
            checksum+=surface->w*surface->h; destroySurface(surface);
          }
        });
      }
      constexpr int views=10000;
      measure("cached_text_view_create",views,[&] {
        for(int i=0;i<views;++i) {
          TextView other(argv[1],24);
          other.setDeferredTextureMaterialization(true);
          checksum+=other.pointSize();
        }
      });
    }
    rendering::UniformCache::getInstance().destroyAll();
    bgfx::shutdown();
    SDL_Quit();
    rusage usage{}; getrusage(RUSAGE_SELF,&usage);
    std::cout << "{\"sdl_major\":" << BENCH_SDL << ",\"checksum\":"<<checksum
              <<",\"event_bytes\":"<<sizeof(SDL_Event)<<",\"precision_events\":"<<precisionEvents
              <<",\"unique_timestamp_micros\":"<<uniqueTimestamps.size()
              <<",\"peak_rss_bytes\":"<<usage.ru_maxrss<<",\"metrics\":[";
    for(size_t i=0;i<measurements.size();++i) {
      if(i) std::cout<<',';
      const auto &m=measurements[i];
      std::cout<<"{\"name\":\""<<m.name<<"\",\"operations\":"<<m.operations
               <<",\"ns_per_op\":"<<m.nsPerOp<<'}';
    }
    std::cout<<"]}\n";
  } catch(const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
}
