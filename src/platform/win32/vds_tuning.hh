#pragma once
#include <atomic>
#include <charconv>
#include <stdexcept>
#include <string_view>

namespace vds::win::tuning {
inline std::atomic<int> gain_percent{100};

inline int parse_gain(std::string_view text) {
  int value = -1;
  const auto [end, error] = std::from_chars(text.data(), text.data()+text.size(), value);
  if (error != std::errc{} || end != text.data()+text.size() || value < 0 || value > 300)
    throw std::runtime_error("gain must be an integer percentage from 0 to 300");
  return value;
}
}
