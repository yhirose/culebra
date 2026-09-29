// The block an Audio.PCM fills between submits: at most `buffer` frames of
// interleaved floats, clamped to -1..1. Raylib-free, so a backend with no audio
// built in counts a push exactly as a stream whose device is missing does.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

#include <base/shared.h>  // alloc_or_too_large / narrow_or_too_large

namespace culebra::rt {

// The rules every Audio stream applies to its arguments, PCM and Capture
// alike: below 1 is 1, past what the device API counts in is refused.
inline int pcm_count(int64_t v, std::string_view what) {
  return culebra::narrow_or_too_large(std::max<int64_t>(v, 1), what, 0, 0);
}
// 2 is stereo; anything else is mono.
inline int pcm_channels(int64_t channels) { return channels == 2 ? 2 : 1; }

class PcmBlock {
 public:
  PcmBlock(int64_t rate, int64_t channels, int64_t buffer)
      : rate_(pcm_count(rate, "Audio.PCM: rate")),
        channels_(pcm_channels(channels)),
        buffer_(pcm_count(buffer, kBuffer)) {
    // Steady-state size, which skips early regrowth; a buffer no machine
    // holds fails here.
    culebra::alloc_or_too_large(kBuffer, 0, 0,
                                [&] { pend_.reserve((size_t)cap()); });
  }

  // The whole frames among `count` values (stereo: interleaved L,R), as many
  // as the block has room for; answers how many it took, not dropping the rest.
  int64_t push_block(const double* values, int64_t count) {
    int64_t take = std::min(count, cap() - (int64_t)pend_.size()) / channels_;
    for (int64_t i = 0; i < take * channels_; i++) {
      pend_.push_back(std::clamp((float)values[i], -1.0f, 1.0f));
    }
    return take;
  }
  int64_t pending() const { return (int64_t)pend_.size() / channels_; }
  // A submit with nowhere to play: the block empties, nothing handed.
  void discard() { pend_.clear(); }
  // Seconds from a submit to the speaker: the two blocks ahead of it.
  double latency() const { return 2.0 * buffer_ / rate_; }

 protected:
  int rate_, channels_, buffer_;
  std::vector<float> pend_;

 private:
  static constexpr std::string_view kBuffer = "Audio.PCM: buffer";
  int64_t cap() const { return (int64_t)buffer_ * channels_; }
};

}  // namespace culebra::rt
