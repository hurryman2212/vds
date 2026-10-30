#pragma once
#include <algorithm>
#include <chrono>
#include <cstddef>

namespace vds::win {
// Per-connection realtime jitter estimator. There is no fixed prefill: start
// immediately, learn positive arrival jitter, grow quickly, and shed latency
// slowly during stable delivery. A long interruption starts a fresh timeline.
class AdaptivePlayout {
public:
  using Clock = std::chrono::steady_clock;
  static constexpr auto packet = std::chrono::microseconds(10667);
  static constexpr auto ceiling = std::chrono::microseconds(60000);
  void observe(Clock::time_point now) {
    if (last_ == Clock::time_point{}) { last_ = now; return; }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now-last_);
    last_ = now;
    if (elapsed > std::chrono::milliseconds(250) || elapsed.count() < 0) {
      target_ = {}; stable_since_ = now; return;
    }
    const auto jitter = std::max(std::chrono::microseconds::zero(), elapsed-packet);
    if (jitter > std::chrono::milliseconds(3)) {
      target_ = std::min(ceiling, std::max(target_, jitter+std::chrono::milliseconds(2)));
      stable_since_ = now;
    } else if (stable_since_ != Clock::time_point{} && now-stable_since_ >= std::chrono::seconds(2)) {
      target_ = std::max(std::chrono::microseconds::zero(), target_-std::chrono::milliseconds(2));
      stable_since_ = now-std::chrono::milliseconds(1800); // 2 ms reduction per 200 ms.
    }
  }
  auto target() const { return target_; }
  auto max_age() const { return target_+2*packet; }
  std::size_t queue_limit() const {
    return static_cast<std::size_t>((target_.count()+packet.count()-1)/packet.count())+2;
  }
private:
  Clock::time_point last_{}, stable_since_{};
  std::chrono::microseconds target_{};
};
}
