#pragma once

// The main thread's calls on an audio Engine (audio_engine.h) carried as
// numbers to an Engine on another thread, and the stream's place carried
// back: how a browser plays, the program in a worker and the Engine in an
// AudioWorklet, which share no memory. The worker writes each call with the
// stream sample it starts on, by its own reckoning of the stream's clock (as
// a native main thread's call is dated), and the worklet makes the same call
// on its Engine for that sample (WireEngine::apply). A schedule's sounds come
// as their samples beside the numbers. audio_engine_test holds an Engine
// driven this way to one driven directly, sample for sample.

#include <stdlib/audio_engine.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

namespace culebra {
namespace _audio_detail {

enum class WireOp : int {
  Tone,
  ScheduleOpen,
  ScheduleFill,
  SchedulePlay,
  ScheduleStop,
  ScheduleFree,
  ScheduleVolume,
  ScheduleRoom,
  GraphPlay,
  GraphStop,
  GraphPost,
  Reset,
};

// The messages, as the worker writes them: each the numbers of one call.
namespace wire {

inline std::vector<double> head(WireOp op, std::initializer_list<double> rest = {}) {
  std::vector<double> m{static_cast<double>(op)};
  m.insert(m.end(), rest);
  return m;
}
// A tone's note (make_note's) on `channel` from stream sample `start`.
inline std::vector<double> tone(int channel, const Note& n, int64_t start) {
  return head(WireOp::Tone, {static_cast<double>(start), static_cast<double>(channel), n.start_freq, n.end_freq,
                             static_cast<double>(n.attack), static_cast<double>(n.decay),
                             static_cast<double>(n.sustain), static_cast<double>(n.release),
                             static_cast<double>(n.total), n.vol, n.peak, n.duty});
}
// A schedule (open_schedule's list, `slots` of them empty).
inline std::vector<double> schedule_open(int64_t id, const double* events, int64_t count, int64_t slots,
                                         double length, double loop_at) {
  auto m = head(WireOp::ScheduleOpen, {static_cast<double>(id), static_cast<double>(slots), length, loop_at,
                                       static_cast<double>(count)});
  m.insert(m.end(), events, events + count);
  return m;
}
// Slot `slot` filled with the samples that come beside it.
inline std::vector<double> schedule_fill(int64_t id, int64_t slot) {
  return head(WireOp::ScheduleFill, {static_cast<double>(id), static_cast<double>(slot)});
}
inline std::vector<double> schedule_play(int64_t id, int64_t start) {
  return head(WireOp::SchedulePlay, {static_cast<double>(id), static_cast<double>(start)});
}
// Stop, free, volume or room.
inline std::vector<double> schedule(WireOp op, int64_t id, double x = 0) {
  return head(op, {static_cast<double>(id), x});
}
// Graph `id` played from `start` into bus `into` (0 for the device), the
// graphs then worked out in `order`: its definition (graph_def's) and the
// buses it touches, each with its channels, go with it.
inline std::vector<double> graph_play(int64_t id, int64_t into, const std::vector<int64_t>& order, int64_t start) {
  const Graph& def = *graph_def(id);
  std::vector<int64_t> buses = def.buses_read();
  if (into != 0) buses.push_back(into);
  auto m = head(WireOp::GraphPlay, {static_cast<double>(id), static_cast<double>(start), static_cast<double>(into),
                                    static_cast<double>(buses.size())});
  for (int64_t b : buses) m.insert(m.end(), {static_cast<double>(b), static_cast<double>(bus_defs().at(b)->channels)});
  m.push_back(static_cast<double>(order.size()));
  m.insert(m.end(), order.begin(), order.end());
  def.encode(m);
  return m;
}
inline std::vector<double> graph_stop(int64_t id) { return head(WireOp::GraphStop, {static_cast<double>(id)}); }
inline std::vector<double> graph_post(int64_t id, int part, int value, ParamEvent e) {
  return head(WireOp::GraphPost, {static_cast<double>(id), static_cast<double>(part), static_cast<double>(value), e.at,
                                  e.ramp ? 1.0 : 0.0, e.value, e.cancel ? 1.0 : 0.0});
}
// Everything let go: what plays stops at once, and the stream plays on. With
// a place, the stream is at sample `at` at `at_ms` on the wall clock (ms,
// Date.now()'s) from then on: the worker's reckoning, which the worklet
// takes up, so the two agree however late it came up.
inline std::vector<double> reset() { return head(WireOp::Reset); }
inline std::vector<double> reset(int64_t at, double at_ms) {
  return head(WireOp::Reset, {static_cast<double>(at), at_ms});
}

}  // namespace wire

// The worklet's side: an Engine, the calls it is sent, and what it tells the
// worker back. The stream's place carries on across a reset, which makes a
// new Engine: the worker's clock never goes back.
class WireEngine {
 public:
  WireEngine() : engine_(std::make_unique<Engine>(epoch)) { engine_->open(); }

  // One message's `n` numbers, `pcm` the samples a fill brings, applied at
  // `now_ms` on the wall clock (what a reset with a place reads); false when
  // they are not one, and nothing is done.
  bool apply(const double* m, size_t n, Pcm pcm = nullptr, double now_ms = 0) {
    NumberReader in(m, m + n);
    double x;
    if (!in.take(x)) return false;
    const auto op = static_cast<WireOp>(static_cast<int>(x));
    const double* a = in.at();
    const size_t args = in.left();
    // A handle or a stream sample: a whole number a double holds exactly.
    auto whole = [](double v) { return std::abs(v) < 0x1p53 && v == std::floor(v); };
    auto at = [&](double v) { return static_cast<int64_t>(v) - base_; };
    switch (op) {
      case WireOp::Tone: {
        if (args < 12 || !whole(a[0]) || !(a[1] >= 0 && a[1] <= 4) || !whole(a[4]) || !whole(a[5]) || !whole(a[6]) ||
            !whole(a[7]) || !whole(a[8]))
          return false;
        Note note;
        note.active = true;
        note.start_freq = a[2];
        note.end_freq = a[3];
        note.attack = static_cast<int64_t>(a[4]);
        note.decay = static_cast<int64_t>(a[5]);
        note.sustain = static_cast<int64_t>(a[6]);
        note.release = static_cast<int64_t>(a[7]);
        note.total = static_cast<int64_t>(a[8]);
        note.vol = a[9];
        note.peak = a[10];
        note.duty = a[11];
        engine_->tone(static_cast<int>(a[1]), note, at(a[0]));
        return true;
      }
      case WireOp::ScheduleOpen: {
        double id, slots, length, loop_at;
        size_t count;
        if (!in.take(id) || !in.take(slots) || !in.take(length) || !in.take(loop_at) || !in.count(count) ||
            !whole(id) || !whole(slots))
          return false;
        const double* events = in.skip(count);
        engine_->schedule_new(static_cast<int64_t>(id), open_schedule(events, static_cast<int64_t>(count),
                                                                      static_cast<int64_t>(slots), length, loop_at));
        schedules_.insert(static_cast<int64_t>(id));
        return true;
      }
      case WireOp::ScheduleFill:
        if (args < 2 || !whole(a[0]) || !whole(a[1]) || !pcm) return false;
        engine_->schedule_fill(static_cast<int64_t>(a[0]), static_cast<int64_t>(a[1]), std::move(pcm));
        return true;
      case WireOp::SchedulePlay:
        if (args < 2 || !whole(a[0]) || !whole(a[1])) return false;
        engine_->schedule_play(static_cast<int64_t>(a[0]), at(a[1]));
        return true;
      case WireOp::ScheduleStop:
      case WireOp::ScheduleFree:
      case WireOp::ScheduleVolume:
      case WireOp::ScheduleRoom: {
        if (args < 2 || !whole(a[0])) return false;
        const auto id = static_cast<int64_t>(a[0]);
        if (op == WireOp::ScheduleStop) engine_->schedule_stop(id);
        if (op == WireOp::ScheduleFree) {
          engine_->schedule_free(id);
          schedules_.erase(id);
        }
        if (op == WireOp::ScheduleVolume) engine_->schedule_volume(id, a[1]);
        if (op == WireOp::ScheduleRoom) engine_->schedule_room(id, a[1]);
        return true;
      }
      case WireOp::GraphPlay: return graph_play(in, whole);
      case WireOp::GraphStop: {
        if (args < 1 || !whole(a[0])) return false;
        const auto id = static_cast<int64_t>(a[0]);
        engine_->graph_stop(id);
        kinds_.erase(id);
        drop_unused_buses();
        return true;
      }
      case WireOp::GraphPost: {
        if (args < 7 || !whole(a[0])) return false;
        const auto id = static_cast<int64_t>(a[0]);
        auto kinds = kinds_.find(id);
        // A graph no longer playing: the change is the next play's, which brings it.
        if (kinds == kinds_.end()) return true;
        const double part = a[1], value = a[2];
        if (!(part >= 0 && part < static_cast<double>(kinds->second.size()) && whole(part))) return false;
        if (!(value >= 0 && value < value_count(kinds->second[static_cast<size_t>(part)]) && whole(value)))
          return false;
        engine_->graph_post(id, static_cast<int>(part), static_cast<int>(value),
                            ParamEvent{a[3], a[4] != 0, a[5], a[6] != 0});
        return true;
      }
      case WireOp::Reset:
        if (args >= 2) {
          if (!whole(a[0]) || !std::isfinite(a[1])) return false;
          base_ = static_cast<int64_t>(a[0]) + std::llround((now_ms - a[1]) * kSampleRate / 1000.0);
          step_at_ = base_;
          step_ms_ = now_ms;
        } else {
          base_ += engine_->played_to();
        }
        engine_ = std::make_unique<Engine>(epoch);
        engine_->open();
        schedules_.clear();
        kinds_.clear();
        buses_.clear();
        return true;
    }
    return false;
  }

  // `frames` frames, the left and the right in turn, rendered at `now_ms`
  // on the wall clock. The stream keeps step with the wall clock the worker
  // dates its calls by: from where the two last were in step (a reset with a
  // place, or the last render), a stream further behind than kOutOfStep was
  // held still (an AudioContext suspended until the page's first gesture),
  // and passes on over that time unheard (Engine::pass), so what was asked
  // for meanwhile sounds now. A device's own pace and its buffers stay well
  // inside it, and taking up the step at each render keeps the two clocks'
  // drift from adding up.
  void render(float* out, unsigned frames, double now_ms) {
    if (!std::isnan(step_ms_)) {
      const int64_t behind = step_at_ + std::llround((now_ms - step_ms_) * kSampleRate / 1000.0) - place();
      if (behind > kOutOfStep) engine_->pass(behind);
    }
    step_at_ = place();
    step_ms_ = now_ms;
    engine_->render(out, frames);
  }

  // What the worker reckons the stream's clock and a schedule's hold from,
  // into `out`: the stream sample the next render starts on, then each
  // schedule's samples held since it last started, as id and held.
  void status(std::vector<double>& out) {
    out.clear();
    out.push_back(static_cast<double>(place()));
    for (int64_t id : schedules_)
      out.insert(out.end(), {static_cast<double>(id), static_cast<double>(engine_->schedule_held(id))});
  }

 private:
  // Samples behind the wall clock that tell a stream held still (250 ms),
  // well past what a device renders in one burst.
  static constexpr int64_t kOutOfStep = kSampleRate / 4;

  // The Engine's own clock dates nothing here (every call brings its start).
  static Engine::Clock::time_point epoch() { return {}; }
  // The stream sample the next render starts on.
  int64_t place() { return base_ + engine_->played_to(); }

  template <class Whole>
  bool graph_play(NumberReader& in, Whole whole) {
    double id, start, into;
    size_t buses;
    if (!in.take(id) || !in.take(start) || !in.take(into) || !in.count(buses, 2) || !whole(id) || !whole(start) ||
        !whole(into))
      return false;
    for (const double* b = in.skip(2 * buses); buses > 0; buses--, b += 2) {
      if (!whole(b[0]) || !(b[1] == 1 || b[1] == 2)) return refused();
      auto& bus = buses_[static_cast<int64_t>(b[0])];
      if (!bus || bus->channels != static_cast<int>(b[1])) bus = std::make_shared<Bus>(static_cast<int>(b[1]));
    }
    size_t count;
    if (!in.count(count)) return refused();
    std::vector<int64_t> order;
    for (const double* o = in.skip(count); count > 0; count--, o++) {
      if (!whole(*o)) return refused();
      order.push_back(static_cast<int64_t>(*o));
    }
    auto def = Graph::decode(in, buses_);
    const auto to = static_cast<int64_t>(into);
    if (!def || (to != 0 && !buses_.contains(to))) return refused();
    auto& kinds = kinds_[static_cast<int64_t>(id)];
    kinds.clear();
    for (size_t p = 0; p < def->size(); p++) kinds.push_back(def->kind(static_cast<int>(p)));
    engine_->graph_play(static_cast<int64_t>(id), std::move(*def), to != 0 ? buses_.at(to) : nullptr, order,
                        static_cast<int64_t>(start) - base_);
    drop_unused_buses();
    return true;
  }
  // A play refused: the buses it brought go again unless something holds them.
  bool refused() {
    drop_unused_buses();
    return false;
  }
  // A bus no copy playing holds any more: a later play brings it again if it
  // needs one.
  void drop_unused_buses() {
    std::erase_if(buses_, [](const auto& entry) { return entry.second.use_count() == 1; });
  }

  std::unique_ptr<Engine> engine_;
  int64_t base_ = 0;  // the stream samples played by the Engines before this one
  // Where the stream and the wall clock were last in step: the stream sample
  // and the time (none before the first render or reset with a place).
  int64_t step_at_ = 0;
  double step_ms_ = std::nan("");
  std::set<int64_t> schedules_;
  // The kind of each part of each graph playing, which a change is checked
  // against, and the buses they touch, by the worker's handles.
  std::unordered_map<int64_t, std::vector<PartKind>> kinds_;
  BusMap buses_;
};
}  // namespace _audio_detail
}  // namespace culebra
