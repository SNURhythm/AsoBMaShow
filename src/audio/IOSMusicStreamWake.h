#pragma once
#include <mutex>
#include <semaphore>

namespace ios_music_stream {
class FeederWake {
public:
  // Retain signals arriving between the feeder's state check and its wait.
  // Completion handlers never acquire the transport mutex.
  void Notify() { wake.release(); }
  void Wait(std::unique_lock<std::mutex> &lock) {
    lock.unlock();
    wake.acquire();
    lock.lock();
  }
private:
  std::counting_semaphore<> wake{0};
};
}
