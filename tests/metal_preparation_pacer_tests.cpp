#include "video/MetalPreparationPacer.h"
#include <cassert>
#include <cmath>
#include <limits>

int main() {
  video::MetalPreparationPacer pacer;
  assert(pacer.startTime(10, 10.008, .008333) == 10);
  for (int i=0; i<29; ++i) pacer.observe(.0012);
  assert(pacer.startTime(10, 10.008, .008333) == 10);
  pacer.observe(.0012);
  assert(std::abs(pacer.startTime(10, 10.008, .008333)-10.0035)<1e-9);
  assert(pacer.startTime(10, 10.030, .033333)<=10.004);
  assert(std::abs(pacer.startTime(10, 10.030, .004)-10.002)<1e-9);
  pacer.observe(.008);
  assert(pacer.startTime(10, 10.008, .008333)==10);
  for (int i=0; i<119; ++i) pacer.observe(.001);
  assert(pacer.startTime(10, 10.008, .008333)==10);
  pacer.observe(.001);
  assert(pacer.startTime(10, 10.008, .008333)>10);
  pacer.observe(std::numeric_limits<double>::quiet_NaN());
  assert(pacer.startTime(10, 10.008, .008333)==10);
  for (int i=0; i<30; ++i) pacer.observe(.001);
  pacer.reset();
  assert(pacer.startTime(10, 10.008, .008333)==10);
  double now=10; int sleeps=0;
  auto clock=[&]{return now;};
  auto sleep=[&](double seconds){assert(seconds<=.001);now+=seconds;++sleeps;};
  assert(video::waitForMetalPreparation(10.0035,10.008,[]{return true;},clock,sleep));
  assert(now>=10.0035 && now<10.008 && sleeps>=4);
  now=10;sleeps=0;
  assert(!video::waitForMetalPreparation(10.004,10.008,[&]{return sleeps<2;},clock,sleep));
  assert(sleeps==2);
  now=10;
  assert(!video::waitForMetalPreparation(10.004,10.008,[]{return true;},clock,[&](double){now=10.009;}));
  assert(!video::waitForMetalPreparation(10,10,[]{return true;},clock,sleep));
}
