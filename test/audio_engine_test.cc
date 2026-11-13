// The audio engine (include/stdlib/audio_engine.h) driven as a device drives
// it, with no device: the main thread's calls, then buffers of random sizes
// (past kMixChunk too), on a clock the test moves by hand. What it plays is
// held to what renders offline sample for sample, and the audio thread's
// rule is held here too: a render allocates and frees nothing (the global
// operator new and delete below count while one runs).
//
// Built and run by CTest (see CMakeLists.txt).

#include <stdlib/audio_engine.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

std::atomic<bool> g_counting{false};
std::atomic<long> g_heap_calls{0};

void* counted_alloc(std::size_t n) {
  if (g_counting.load(std::memory_order_relaxed)) g_heap_calls++;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void counted_free(void* p) noexcept {
  if (p && g_counting.load(std::memory_order_relaxed)) g_heap_calls++;
  std::free(p);
}

}  // namespace

void* operator new(std::size_t n) { return counted_alloc(n); }
void* operator new[](std::size_t n) { return counted_alloc(n); }
void operator delete(void* p) noexcept { counted_free(p); }
void operator delete[](void* p) noexcept { counted_free(p); }
void operator delete(void* p, std::size_t) noexcept { counted_free(p); }
void operator delete[](void* p, std::size_t) noexcept { counted_free(p); }

namespace {

namespace ad = culebra::_audio_detail;
using Clock = ad::Engine::Clock;

int failures = 0;

#define CHECK(cond, ...)                \
  do {                                  \
    if (!(cond)) {                      \
      std::printf("FAIL %s: ", __func__); \
      std::printf(__VA_ARGS__);         \
      std::printf("\n");                \
      failures++;                       \
    }                                   \
  } while (0)

// The clock the engine dates what it is asked for by; the device's buffers
// take no time on it unless a test moves it.
Clock::time_point g_now{};
Clock::time_point fake_now() { return g_now; }

// The device: an engine on the clock above, and buffers of 1 to three
// kMixChunks of frames (the left and the right in turn) rendered and kept
// as two recordings, each render's heap calls counted. Positions are frames.
struct Device {
  std::unique_ptr<ad::Engine> engine = std::make_unique<ad::Engine>(fake_now);
  std::vector<float> left, right;
  std::vector<float> buffer = std::vector<float>(2 * 3 * ad::kMixChunk);
  std::mt19937 rng{12345};
  long heap_calls = 0;

  explicit Device(size_t most) {
    engine->open();
    left.reserve(most);
    right.reserve(most);
  }
  int64_t pos() const { return static_cast<int64_t>(left.size()); }
  void run(int64_t frames) {
    std::uniform_int_distribution<unsigned> size(1, 3 * ad::kMixChunk);
    while (frames > 0) {
      unsigned n = static_cast<unsigned>(std::min<int64_t>(size(rng), frames));
      g_heap_calls = 0;
      g_counting = true;
      engine->render(buffer.data(), n);
      g_counting = false;
      heap_calls += g_heap_calls;
      for (unsigned i = 0; i < n; i++) {
        left.push_back(buffer[2 * i]);
        right.push_back(buffer[2 * i + 1]);
      }
      frames -= n;
    }
  }
};

// A song's worth of Kauai's rows: tones on every channel, the noise one too,
// and samples, over `seconds`.
std::vector<double> song_rows(double seconds, unsigned seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> u(0.0, 1.0);
  std::vector<std::vector<double>> rows;
  for (double at = 0; at < seconds; at += 0.02 + 0.08 * u(rng)) {
    if (u(rng) < 0.6) {
      int channel = static_cast<int>(u(rng) * 5);
      rows.push_back({at, static_cast<double>(channel), 100 + 1500 * u(rng), 100 + 1500 * u(rng),
                      0.01 * u(rng), 0.05 * u(rng), 0.2 * u(rng), 0.1 * u(rng),
                      100 * u(rng), 100 * u(rng), static_cast<double>(static_cast<int>(u(rng) * 4))});
    } else {
      rows.push_back({at, -1, static_cast<double>(1 + static_cast<int>(u(rng) * 3)), 0.5 * u(rng),
                      0, 0, 0, 0, 0, 0, 0});
    }
  }
  std::vector<double> flat;
  for (auto& r : rows) flat.insert(flat.end(), r.begin(), r.end());
  return flat;
}

// The samples the rows above name, handles 1 to 3.
ad::PcmByHandle song_pcm() {
  ad::PcmByHandle pcm;
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> u(-1.0f, 1.0f);
  for (int h = 1; h <= 3; h++) {
    std::vector<float> x(static_cast<size_t>(4000 * h + 1234));
    for (auto& v : x) v = u(rng);
    pcm[h] = std::make_shared<const std::vector<float>>(std::move(x));
  }
  return pcm;
}

// `rows` as the stream plays them (schedule_new's), and rendered offline.
std::shared_ptr<ad::Schedule> song(const std::vector<double>& rows, const ad::PcmByHandle& pcm,
                                   double seconds) {
  return ad::build_schedule(rows.data(), static_cast<int64_t>(rows.size()), seconds, -1, pcm);
}
std::vector<float> rendered(const std::vector<double>& rows, const ad::PcmByHandle& pcm,
                            double seconds, double wet, double volume) {
  std::vector<float> out;
  ad::render_schedule(rows.data(), static_cast<int64_t>(rows.size()), seconds, wet, volume, 0.0,
                      pcm, out);
  return out;
}
// Rendered on both sides: the left (what `rendered` gives) and the right.
std::pair<std::vector<float>, std::vector<float>> rendered_sides(const std::vector<double>& rows,
                                                                 const ad::PcmByHandle& pcm, double seconds,
                                                                 double wet, double volume) {
  std::vector<float> left, right;
  ad::render_schedule(rows.data(), static_cast<int64_t>(rows.size()), seconds, wet, volume, 0.0,
                      pcm, left, &right);
  return {std::move(left), std::move(right)};
}
// How many samples of `a` and `b` differ.
long differing(const std::vector<float>& a, const std::vector<float>& b) {
  long n = 0;
  for (size_t i = 0; i < std::min(a.size(), b.size()); i++) n += a[i] != b[i];
  return n;
}

bool same_bits(float a, float b) { return std::memcmp(&a, &b, sizeof a) == 0; }

// Graph `id` played as Audio.Graph's play() plays it: into bus `into` (0 for
// the device), the graphs playing worked out in graph_played's order.
void play(ad::Engine& engine, int64_t id, int64_t into = 0) {
  auto order = ad::graph_played(id, into, ad::graph_def(id)->buses_read());
  engine.graph_play(id, into ? ad::bus_defs().at(into) : nullptr, *order);
}
void stop(ad::Engine& engine, int64_t id) {
  ad::graphs_playing().erase(id);
  engine.graph_stop(id);
}

// A graph's samples as the stream plays them: held to -1..1, as floats.
std::vector<float> as_played(const std::vector<double>& x) {
  std::vector<float> out;
  for (double v : x) out.push_back(static_cast<float>(std::clamp(0.0 + v, -1.0, 1.0)));
  return out;
}

// `heard` from `from` on is `want`, and silent before and after it.
void hears(const char* what, const std::vector<float>& heard, int64_t from,
           const std::vector<float>& want) {
  size_t wrong = 0, first = 0;
  for (size_t i = 0; i < heard.size(); i++) {
    auto k = static_cast<int64_t>(i) - from;
    float w = k >= 0 && k < static_cast<int64_t>(want.size()) ? want[static_cast<size_t>(k)] : 0.0f;
    if (!same_bits(heard[i], w) && !(heard[i] == 0 && w == 0) && wrong++ == 0) first = i;
  }
  CHECK(wrong == 0, "%s: %zu of %zu samples differ, the first at %zu", what, wrong,
        heard.size(), first);
}
// Both channels hear `want` (a mono sound is the same on both).
void hears(const char* what, const Device& device, int64_t from, const std::vector<float>& want) {
  std::string name = what;
  hears((name + ", left").c_str(), device.left, from, want);
  hears((name + ", right").c_str(), device.right, from, want);
}

// A song played on the stream is the song rendered once through: the same
// samples on each side from the one it starts on, whatever sizes the
// buffers come in. Its room's sides ring apart, and a render of the left
// alone is the left.
void schedule_plays_as_it_renders() {
  auto rows = song_rows(3.0, 1);
  auto pcm = song_pcm();
  auto [want, want_right] = rendered_sides(rows, pcm, 3.0, 0.3, 0.8);
  CHECK(want.size() > 3 * 44100, "the room rings on past the song (%zu samples)", want.size());
  hears("the left rendered alone", rendered(rows, pcm, 3.0, 0.3, 0.8), 0, want);
  CHECK(differing(want, want_right) > 44100, "the room's sides ring apart (%ld samples differ)",
        differing(want, want_right));

  Device device(want.size() + 50000);
  device.run(777);
  int64_t start = device.pos();
  device.engine->schedule_new(1, song(rows, pcm, 3.0));
  device.engine->schedule_volume(1, 0.8);
  device.engine->schedule_room(1, 0.3);
  device.engine->schedule_play(1);
  device.run(static_cast<int64_t>(want.size()) + 20000);
  hears("the song, left", device.left, start, want);
  hears("the song, right", device.right, start, want_right);
  CHECK(device.heap_calls == 0, "the renders made %ld heap calls", device.heap_calls);
}

// Each side of Kauai's room is the room it was before it had two: the tank
// below is the one Room ran (its feedback and damp, Jezar's delays), and
// ReverbTank's left side answers it bit for bit; the right rings apart.
void room_left_is_the_room_it_was() {
  struct OldRoom {
    std::vector<float> combs[8], passes[4];
    float damped[8] = {};
    size_t comb_at[8] = {}, pass_at[4] = {};
    OldRoom() {
      for (int k = 0; k < 8; k++) combs[k].assign(ad::ReverbTank::kCombs[k], 0.0f);
      for (int k = 0; k < 4; k++) passes[k].assign(ad::ReverbTank::kPasses[k], 0.0f);
    }
    double step(double x) {
      double into = x * 0.015;
      double echo = 0;
      for (int k = 0; k < 8; k++) {
        auto& buf = combs[k];
        double y = buf[comb_at[k]];
        damped[k] = static_cast<float>(y * (1.0 - 0.25) + damped[k] * 0.25);
        buf[comb_at[k]] = static_cast<float>(into + damped[k] * 0.86);
        comb_at[k] = (comb_at[k] + 1) % buf.size();
        echo += y;
      }
      for (int k = 0; k < 4; k++) {
        auto& buf = passes[k];
        double held = buf[pass_at[k]];
        buf[pass_at[k]] = static_cast<float>(echo + held * 0.5);
        pass_at[k] = (pass_at[k] + 1) % buf.size();
        echo = held - echo;
      }
      return echo;
    }
  };
  OldRoom old, old_room;
  ad::ReverbTank left(ad::kSampleRate, 0), right(ad::kSampleRate, 1);
  ad::Room room;
  room.wet.store(1.0);
  std::mt19937 rng(3);
  std::uniform_real_distribution<double> u(-1.0, 1.0);
  long wrong = 0, apart = 0, wrong_room = 0;
  for (int i = 0; i < 5 * ad::kSampleRate; i++) {
    double x = i < 2 * ad::kSampleRate ? u(rng) : 0.0;
    double a = old.step(x), b = left.step(x, ad::Room::kFeedback, ad::Room::kDamp);
    wrong += std::memcmp(&a, &b, sizeof a) != 0;
    apart += right.step(x, ad::Room::kFeedback, ad::Room::kDamp) != b;
    // The Room, which stops once it has rung kTail past its sound.
    room.send = x;
    double c = room.step().left, d = old_room.step(x);
    wrong_room += room.ringing() ? std::memcmp(&c, &d, sizeof c) != 0 : c != 0;
  }
  CHECK(wrong == 0, "the left side differs from the room it was in %ld samples", wrong);
  CHECK(apart > ad::kSampleRate, "the right side rings apart from the left (%ld samples differ)", apart);
  CHECK(wrong_room == 0, "the Room's left differs from the room it was in %ld samples", wrong_room);
}

// A tone starts on the sample the clock says it was asked for on, and sounds
// as the same note does in a schedule.
void tone_starts_when_asked() {
  std::vector<double> row = {0, ad::kNoise, 300, 2000, 0.01, 0.02, 0.1, 0.05, 80, 90, 2};
  ad::PcmByHandle none;
  auto want = rendered(row, none, 0.0, 0.0, 1.0);

  Device device(100000);
  device.run(5000);
  g_now += std::chrono::milliseconds(10);
  device.engine->tone(ad::kNoise, song(row, none, 0.0)->events[0].note);
  int64_t start = device.pos() + 441;
  device.run(30000);
  g_now = {};
  hears("the tone", device, start, want);
  CHECK(device.heap_calls == 0, "the renders made %ld heap calls", device.heap_calls);
}

// A graph played is the graph rendered, its changes too: those made while it
// plays, more of them than a value has room for at the start.
void graph_plays_as_it_renders() {
  const int64_t id = 9001;
  ad::Graph def(ad::kMixerRate);
  int wave = def.add(ad::PartKind::Wave, {}, ad::Oscillator{ad::WaveKind::Saw}, 220, 0);
  int lowpass = def.add(ad::PartKind::Filter, {wave}, ad::Svf(ad::FilterKind::Low, 0.9), 1200);
  def.output(lowpass);
  ad::graph_defs().insert_or_assign(id, def);

  Device device(200000);
  device.run(3000);
  int64_t start = device.pos();
  play(*device.engine, id);
  device.run(4410);
  auto& live = *ad::graph_def(id);
  for (int k = 0; k < 2 * static_cast<int>(ad::kGraphLiveRoom); k++) {
    ad::ParamEvent e{0.2 + 0.002 * k, k % 2 == 1, 200.0 + 3 * k};
    live.schedule(wave, 0, e);
    device.engine->graph_post(id, wave, 0, e);
  }
  device.run(60000);

  ad::Graph copy(live);
  std::vector<double> samples(static_cast<size_t>(device.pos() - start));
  copy.render(samples.data(), static_cast<int64_t>(samples.size()));
  auto want = as_played(samples);
  hears("the graph", device, start, want);
  CHECK(device.heap_calls == 0, "the renders made %ld heap calls", device.heap_calls);
  stop(*device.engine, id);
  ad::graph_defs().erase(id);
}

// A graph that reads a bus hears what a graph plays into it, the same
// stretch: the reader is played first and the writer then starts in the
// middle of a buffer, and the writer is still worked out first. The reader
// hears silence until the writer starts and after it stops; a mono writer
// fills both channels of a bus of two.
void bus_carries_a_graph() {
  const int64_t bus = 9401, writer = 9402, reader = 9403;
  ad::bus_defs()[bus] = std::make_shared<ad::Bus>(2);
  ad::Graph w(ad::kMixerRate);
  int saw = w.add(ad::PartKind::Wave, {}, ad::Oscillator{ad::WaveKind::Saw}, 220, 0);
  w.output(w.add(ad::PartKind::Filter, {saw}, ad::Svf(ad::FilterKind::Low, 0.9), 1500));
  ad::Graph r(ad::kMixerRate);
  int left = r.add(ad::PartKind::Input, {}, ad::BusRead{ad::bus_defs()[bus], bus, 0});
  int right = r.add(ad::PartKind::Input, {}, ad::BusRead{ad::bus_defs()[bus], bus, 1});
  r.output(r.add(ad::PartKind::Gain, {left}, std::monostate{}, 0.5),
           r.add(ad::PartKind::Gain, {right}, std::monostate{}, 0.25));
  ad::graph_defs().insert_or_assign(writer, w);
  ad::graph_defs().insert_or_assign(reader, r);

  Device device(120000);
  device.run(2000);
  play(*device.engine, reader);
  device.run(3000);
  g_now += std::chrono::milliseconds(10);
  int64_t start = device.pos() + 441;
  play(*device.engine, writer, bus);
  device.run(50000);
  int64_t stopped = device.pos();
  stop(*device.engine, writer);
  device.run(10000);
  g_now = {};

  std::vector<double> x(static_cast<size_t>(stopped - start));
  ad::Graph(w).render(x.data(), static_cast<int64_t>(x.size()));
  std::vector<double> l(x.size()), rr(x.size());
  for (size_t i = 0; i < x.size(); i++) {
    l[i] = x[i] * 0.5;
    rr[i] = x[i] * 0.25;
  }
  hears("the bus, left", std::vector<float>(device.left.begin(), device.left.begin() + stopped), start, as_played(l));
  hears("the bus, right", std::vector<float>(device.right.begin(), device.right.begin() + stopped), start,
        as_played(rr));
  CHECK(std::all_of(device.left.begin() + stopped + ad::kMixChunk, device.left.end(), [](float v) { return v == 0; }),
        "the bus is silent once its writer stops");
  CHECK(device.heap_calls == 0, "the renders made %ld heap calls", device.heap_calls);
  stop(*device.engine, reader);

  // A reader that starts in the middle of a buffer hears the writer's samples
  // from that sample on.
  Device late(80000);
  late.run(1000);
  int64_t written = late.pos();
  play(*late.engine, writer, bus);
  late.run(2500);
  g_now += std::chrono::milliseconds(10);
  int64_t heard = late.pos() + 441;
  play(*late.engine, reader);
  late.run(30000);
  g_now = {};
  std::vector<double> y(static_cast<size_t>(late.pos() - written));
  ad::Graph(w).render(y.data(), static_cast<int64_t>(y.size()));
  std::vector<double> half(y.begin() + (heard - written), y.end());
  for (double& v : half) v *= 0.5;
  hears("a reader from the middle of a buffer", late.left, heard, as_played(half));
  stop(*late.engine, reader);
  stop(*late.engine, writer);
  ad::graph_defs().erase(writer);
  ad::graph_defs().erase(reader);
  ad::bus_defs().erase(bus);
}

// A graph with a loop played is the graph rendered, and the loop (worked out a
// sample at a time, through a delay's two halves) makes no heap call on the
// audio thread: a saw into a mix that takes back its own echo through a
// low-pass, a delay of 200 samples and a gain, and a short delay of its own.
void loop_plays_as_it_renders() {
  const int64_t id = 9002;
  ad::Graph def(ad::kMixerRate);
  int saw = def.add(ad::PartKind::Wave, {}, ad::Oscillator{ad::WaveKind::Saw}, 220, 0);
  int mix = def.add(ad::PartKind::Gain, {saw}, std::monostate{}, 1.0);
  int line = def.add(ad::PartKind::Delay, {mix}, ad::DelayLine(ad::kMixerRate + 1), 200.0 / ad::kMixerRate);
  int lowpass = def.add(ad::PartKind::Filter, {line}, ad::Svf(ad::FilterKind::Low, 0.9), 2000);
  int back = def.add(ad::PartKind::Gain, {lowpass}, std::monostate{}, 0.6);
  CHECK(def.connect(back, mix, -1), "a loop through a delay is refused");
  int short_line = def.add(ad::PartKind::Delay, {back}, ad::DelayLine(ad::kMixerRate + 1), 37.0 / ad::kMixerRate);
  CHECK(def.connect(short_line, short_line, -1), "a delay taking itself in is refused");
  CHECK(!def.connect(back, back, -1), "a gain taking itself in is not refused");
  def.output(mix);
  ad::graph_defs().insert_or_assign(id, def);

  Device device(200000);
  device.run(3000);
  int64_t start = device.pos();
  play(*device.engine, id);
  device.run(30000);

  ad::Graph copy(*ad::graph_def(id));
  std::vector<double> samples(static_cast<size_t>(device.pos() - start));
  copy.render(samples.data(), static_cast<int64_t>(samples.size()));
  auto want = as_played(samples);
  hears("the loop", device, start, want);
  CHECK(device.heap_calls == 0, "the renders made %ld heap calls", device.heap_calls);
  stop(*device.engine, id);
  ad::graph_defs().erase(id);
}

// A graph with a left and a right output, played from partway into a buffer,
// is the graph rendered, each channel its own: a saw on the left and a sine
// on the right, both silent before it starts.
void stereo_graph_plays_as_it_renders() {
  const int64_t id = 9003;
  ad::Graph def(ad::kMixerRate);
  int saw = def.add(ad::PartKind::Wave, {}, ad::Oscillator{ad::WaveKind::Saw}, 220, 0);
  int left = def.add(ad::PartKind::Gain, {saw}, std::monostate{}, 0.3);
  int sine = def.add(ad::PartKind::Wave, {}, ad::Oscillator{ad::WaveKind::Sine}, 330, 0);
  int right = def.add(ad::PartKind::Gain, {sine}, std::monostate{}, 0.4);
  def.output(left, right);
  ad::graph_defs().insert_or_assign(id, def);

  Device device(100000);
  device.run(3000);
  g_now += std::chrono::milliseconds(10);
  play(*device.engine, id);
  int64_t start = device.pos() + 441;
  device.run(30000);
  g_now = {};

  ad::Graph copy(*ad::graph_def(id));
  size_t n = static_cast<size_t>(device.pos() - start);
  std::vector<double> l(n), r(n);
  copy.render(l.data(), r.data(), static_cast<int64_t>(n));
  hears("the stereo graph, left", device.left, start, as_played(l));
  hears("the stereo graph, right", device.right, start, as_played(r));
  CHECK(device.heap_calls == 0, "the renders made %ld heap calls", device.heap_calls);
  stop(*device.engine, id);
  ad::graph_defs().erase(id);
}

// Two songs at once: with no room the right is the left bit for bit (a sound
// in no graph is mono), and each in its room the sides ring apart.
void two_songs_on_both_channels() {
  auto pcm = song_pcm();
  auto rows1 = song_rows(1.0, 11), rows2 = song_rows(1.0, 12);
  for (double wet : {0.0, 0.4}) {
    Device device(200000);
    device.run(1000);
    for (int64_t id : {1, 2}) {
      device.engine->schedule_new(id, song(id == 1 ? rows1 : rows2, pcm, 1.0));
      device.engine->schedule_room(id, wet);
      device.engine->schedule_play(id);
      device.run(1234);
    }
    device.run(4 * 44100);
    auto loud = std::ranges::count_if(device.left, [](float v) { return v != 0; });
    CHECK(loud > 44100, "the songs sound (%ld samples)", static_cast<long>(loud));
    if (wet == 0) {
      hears("two songs, the right against the left", device.right, 0, device.left);
    } else {
      CHECK(differing(device.left, device.right) > 44100, "two songs in their rooms: %ld samples differ",
            differing(device.left, device.right));
    }
    CHECK(device.heap_calls == 0, "the renders made %ld heap calls", device.heap_calls);
  }
}

// More songs and graphs playing at once than the mixer's lists first held:
// the main thread hands the audio thread longer lists before they could fill,
// and frees the ones they replace.
void many_play_at_once() {
  auto pcm = song_pcm();
  Device device(400000);
  auto& engine = *device.engine;
  device.run(100);
  for (int64_t id = 1; id <= 40; id++) {
    engine.schedule_new(id, song(song_rows(0.3, static_cast<unsigned>(id)), pcm, 0.3));
    engine.schedule_play(id);
    device.run(50);
  }
  device.run(20000);
  CHECK(device.heap_calls == 0, "40 songs: the renders made %ld heap calls", device.heap_calls);
  device.heap_calls = 0;
  ad::Graph def(ad::kMixerRate);
  def.output(def.add(ad::PartKind::Wave, {}, ad::Oscillator{ad::WaveKind::Sine}, 330, 0));
  for (int64_t id = 9100; id < 9200; id++) {
    ad::graph_defs().insert_or_assign(id, def);
    play(engine, id);
    if (id % 10 == 0) device.run(50);
  }
  device.run(20000);
  CHECK(device.heap_calls == 0, "100 graphs: the renders made %ld heap calls", device.heap_calls);
  for (int64_t id = 1; id <= 40; id++) engine.schedule_free(id);
  for (int64_t id = 9100; id < 9200; id++) stop(engine, id);
  device.heap_calls = 0;
  device.run(5000);
  engine.release_retired();
  CHECK(device.heap_calls == 0, "stopped: the renders made %ld heap calls", device.heap_calls);
  for (int64_t id = 9100; id < 9200; id++) ad::graph_defs().erase(id);
}

// Past kMaxSamplers sounding at once the oldest goes, and it fades out
// rather than stopping dead: 64 samples hold a level, a 65th (silent) starts,
// and the level falls by the oldest's share without a step. A flood of
// samples starting together stays within the mixer's room. Played live,
// both are what renders offline.
void stolen_sample_fades() {
  ad::PcmByHandle pcm;
  pcm[1] = std::make_shared<const std::vector<float>>(2 * 44100, 0.5f);
  pcm[2] = std::make_shared<const std::vector<float>>(44100 / 2, 0.0f);
  auto rows_of = [](int held, int more) {
    std::vector<double> rows;
    for (int k = 0; k < held; k++) rows.insert(rows.end(), {0, -1, 1, 0.02, 0, 0, 0, 0, 0, 0, 0});
    for (int k = 0; k < more; k++) rows.insert(rows.end(), {0.5, -1, 2, 0.02, 0, 0, 0, 0, 0, 0, 0});
    return rows;
  };
  const size_t steal = 44100 / 2;

  auto rows = rows_of(static_cast<int>(ad::kMaxSamplers), 1);
  auto want = rendered(rows, pcm, 2.0, 0.0, 1.0);
  double step = 0;
  for (size_t i = steal - 2; i < steal + 200; i++)
    step = std::max(step, std::abs(static_cast<double>(want[i + 1]) - want[i]));
  CHECK(step < 0.001, "the level steps by %g where the oldest goes (its share is 0.01)", step);
  CHECK(std::abs(want[steal + 200] - 0.63) < 1e-5, "the oldest is gone after its fade: %g",
        static_cast<double>(want[steal + 200]));

  auto flood = rows_of(static_cast<int>(ad::kMaxSamplers), 3 * static_cast<int>(ad::kMaxSamplers));
  for (auto* r : {&rows, &flood}) {
    Device device(4 * 44100);
    device.run(300);
    int64_t start = device.pos();
    device.engine->schedule_new(1, song(*r, pcm, 2.0));
    device.engine->schedule_play(1);
    device.run(3 * 44100);
    hears(r == &rows ? "the stolen sample" : "the flood", device, start,
          rendered(*r, pcm, 2.0, 0.0, 1.0));
    CHECK(device.heap_calls == 0, "the renders made %ld heap calls", device.heap_calls);
  }
}

// A song with its sounds still to come: a tone ends, and then at 0.2 s a tone
// and a sample (slot 0) start together, and at 0.5 s another sample (slot 1);
// rendered offline with the slots' samples given by handle.
struct LateSong {
  std::vector<double> rows = {
      0.0, 0, 440, 440, 0, 0, 0.05, 0, 80, 80, 2,
      0.2, 1, 660, 660, 0, 0, 0.05, 0, 80, 80, 2,
      0.2, -1, 0, 0.5, 0, 0, 0, 0, 0, 0, 0,
      0.5, -1, 1, 0.5, 0, 0, 0, 0, 0, 0, 0,
  };
  ad::PcmByHandle pcm = [] {
    auto by = song_pcm();
    return ad::PcmByHandle{{0, by[1]}, {1, by[2]}};
  }();
  std::vector<float> want = rendered(rows, pcm, 0.8, 0.0, 1.0);
  static constexpr int64_t kFirst = 8820, kSecond = 22050;  // the samples' starts

  std::shared_ptr<ad::Schedule> open() const {
    return ad::open_schedule(rows.data(), static_cast<int64_t>(rows.size()), 2, 0.8, -1);
  }
  // What plays when it held `holds` (where, in samples of the song, and for how long).
  std::vector<float> held(std::vector<std::pair<int64_t, int64_t>> holds) const {
    auto out = want;
    for (auto it = holds.rbegin(); it != holds.rend(); ++it)
      out.insert(out.begin() + it->first, static_cast<size_t>(it->second), 0.0f);
    return out;
  }
};

// A song whose sounds come late waits for each: its clock stands still from
// the sample a sound should start on until it comes, and then it plays on as
// it renders, the tone due with that sound starting with it.
void open_schedule_waits_for_its_sounds() {
  LateSong song;
  Device device(song.want.size() + 60000);
  device.run(500);
  int64_t start = device.pos();
  auto& engine = *device.engine;
  engine.schedule_new(1, song.open());
  engine.schedule_play(1);
  device.run(13230);
  const int64_t first = 13230 - LateSong::kFirst;
  CHECK(engine.schedule_held(1) == first, "held %lld samples for slot 0, not %lld",
        static_cast<long long>(engine.schedule_held(1)), static_cast<long long>(first));
  engine.schedule_fill(1, 0, song.pcm.at(0));
  device.run(22050);
  const int64_t second = 13230 + 22050 - first - LateSong::kSecond;
  CHECK(engine.schedule_held(1) == first + second, "held %lld samples in all, not %lld",
        static_cast<long long>(engine.schedule_held(1)), static_cast<long long>(first + second));
  engine.schedule_fill(1, 1, song.pcm.at(1));
  device.run(static_cast<int64_t>(song.want.size()) + 10000);
  hears("the song that waited", device, start,
        song.held({{LateSong::kFirst, first}, {LateSong::kSecond, second}}));
  CHECK(device.heap_calls == 0, "the renders made %ld heap calls", device.heap_calls);
  // Played again, it counts what it holds from its new start.
  engine.schedule_play(1);
  device.run(100);
  CHECK(engine.schedule_held(1) == 0, "played again, held %lld samples",
        static_cast<long long>(engine.schedule_held(1)));
}

// Sounds come from the main thread while the audio thread renders: the song
// waits as long as it has to, and is the same song after.
void filled_while_it_renders() {
  LateSong song;
  const size_t most = 10 * 44100;
  Device device(most);
  auto& engine = *device.engine;
  engine.schedule_new(1, song.open());
  engine.schedule_play(1);
  // The audio thread renders until the song has played through after its
  // sounds came; it alone touches `device` until it is joined. A main thread
  // late past 30000 samples finds it paused there, as a stalled device would
  // be, so the song is still to play on when the sounds come.
  std::atomic<size_t> rendered_to{0};
  std::atomic<bool> filled{false}, done{false};
  std::thread audio([&] {
    while (!done.load(std::memory_order_acquire)) {
      if (!filled.load(std::memory_order_acquire) && device.left.size() >= 30000) {
        std::this_thread::yield();
        continue;
      }
      device.run(512);
      rendered_to.store(device.left.size(), std::memory_order_release);
    }
  });
  auto wait_for = [&](size_t n) {
    while (rendered_to.load(std::memory_order_acquire) < n) std::this_thread::yield();
  };
  wait_for(20000);
  engine.schedule_fill(1, 1, song.pcm.at(1));
  engine.schedule_fill(1, 0, song.pcm.at(0));
  filled = true;
  wait_for(rendered_to.load() + 512 + song.want.size());
  done = true;
  audio.join();
  int64_t held = engine.schedule_held(1);
  CHECK(held >= 20000 - LateSong::kFirst, "held %lld samples", static_cast<long long>(held));
  hears("the song filled as it rendered", device, 0, song.held({{LateSong::kFirst, held}}));
  CHECK(device.heap_calls == 0, "the renders made %ld heap calls", device.heap_calls);
}

}  // namespace

int main() {
  schedule_plays_as_it_renders();
  room_left_is_the_room_it_was();
  tone_starts_when_asked();
  graph_plays_as_it_renders();
  loop_plays_as_it_renders();
  stereo_graph_plays_as_it_renders();
  bus_carries_a_graph();
  two_songs_on_both_channels();
  many_play_at_once();
  stolen_sample_fades();
  open_schedule_waits_for_its_sounds();
  filled_while_it_renders();
  if (failures) {
    std::printf("audio_engine_test: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("audio_engine_test: OK\n");
  return 0;
}
