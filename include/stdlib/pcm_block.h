// The block an Audio.Pcm fills between submits: at most `buffer` frames of
// interleaved floats, clamped to -1..1. Raylib-free, so a backend with no audio
// built in counts a push exactly as a stream whose device is missing does.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace culebra::rt {

class PcmBlock {
 public:
  PcmBlock(int64_t rate, int64_t channels, int64_t buffer)
      : rate_((int)(rate < 1 ? 1 : rate)),
        channels_(channels == 2 ? 2 : 1),
        buffer_((int)(buffer < 1 ? 1 : buffer)) {
    pend_.reserve((size_t)cap());  // steady-state size; skips early regrowth
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
  int64_t cap() const { return (int64_t)buffer_ * channels_; }
};

}  // namespace culebra::rt
