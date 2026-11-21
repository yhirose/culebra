// The audio engine (include/stdlib/audio_engine.h) driven as a device drives
// it, with no device: the main thread's calls, then buffers of random sizes
// (past kMixChunk too), on a clock the test moves by hand. What it plays is
// held to what renders offline sample for sample, and the audio thread's
// rule is held here too: a render allocates and frees nothing (the global
// operator new and delete below count while one runs).
//
// Built and run by CTest (see CMakeLists.txt).

#include <stdlib/audio_engine.h>
#include <stdlib/audio_wire.h>

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

// Parts of `g` as Audio.Graph's part() makes them.
int part(ad::Graph& g, ad::PartFamily family, int kind, std::vector<int> in, std::vector<double> options) {
  return g.add(ad::PartSpec{family, kind, std::move(options)}, std::move(in));
}
int wave(ad::Graph& g, ad::WaveKind kind, double freq) {
  return part(g, ad::PartFamily::Wave, static_cast<int>(kind), {}, {freq, 0, 0});
}
int lowpass(ad::Graph& g, int in, double cutoff, double q) {
  return part(g, ad::PartFamily::Filter, static_cast<int>(ad::FilterKind::Low), {in}, {cutoff, q});
}
int gain(ad::Graph& g, int in, double v) { return part(g, ad::PartFamily::Gain, 0, {in}, {v}); }
int delay(ad::Graph& g, int in, double time) { return part(g, ad::PartFamily::Delay, 0, {in}, {time}); }
int input(ad::Graph& g, int64_t bus, int channel) {
  return part(g, ad::PartFamily::Input, 0, {}, {static_cast<double>(bus), static_cast<double>(channel)});
}

// Graph `id` played as Audio.Graph's play() plays it: into bus `into` (0 for
// the device), the graphs playing worked out in graph_played's order.
void play(ad::Engine& engine, int64_t id, int64_t into = 0) {
  auto order = ad::graph_played(id, into, ad::graph_def(id)->buses_read());
  engine.graph_play(id, *ad::graph_def(id), into ? ad::bus_defs().at(into) : nullptr, *order);
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
  int saw = wave(def, ad::WaveKind::Saw, 220);
  def.output(lowpass(def, saw, 1200, 0.9));
  ad::graph_defs().insert_or_assign(id, def);

  Device device(200000);
  device.run(3000);
  int64_t start = device.pos();
  play(*device.engine, id);
  device.run(4410);
  auto& live = *ad::graph_def(id);
  for (int k = 0; k < 2 * static_cast<int>(ad::kGraphLiveRoom); k++) {
    ad::ParamEvent e{0.2 + 0.002 * k, k % 2 == 1, 200.0 + 3 * k};
    live.schedule(saw, 0, e);
    device.engine->graph_post(id, saw, 0, e);
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

// A graph whose start the stream has passed (a browser's mixer comes up
// after the worker started it) reads the time since that start, as the
// program reckons it: a change for a moment after now sounds then, not the
// time it was late later.
void late_graph_reads_its_start() {
  const int64_t id = 9006;
  const int64_t late = 5000, soon = 200;
  ad::Graph def(ad::kMixerRate);
  int g = gain(def, wave(def, ad::WaveKind::Sine, 440), 0.0);
  def.output(g);
  def.schedule(g, 0, ad::ParamEvent{static_cast<double>(late + soon) / ad::kMixerRate, false, 1.0, false});
  ad::graph_defs().insert_or_assign(id, def);

  Device device(20000);
  device.run(8000);
  const int64_t now = device.pos();
  auto order = ad::graph_played(id, 0, def.buses_read());
  device.engine->graph_play(id, def, nullptr, *order, now - late);
  device.run(late + 2 * soon);
  int64_t first = now;
  while (first < device.pos() && device.left[static_cast<size_t>(first)] == 0) first++;
  CHECK(first - now >= soon - 1 && first - now <= soon + 1,
        "a graph started %lld samples late sounds %lld samples in, not %lld", static_cast<long long>(late),
        static_cast<long long>(first - now), static_cast<long long>(soon));
  stop(*device.engine, id);
  ad::graph_defs().erase(id);
}

// The stream's clock never goes back. A buffer called back late dates the
// stream behind where the clock had run on to; a graph started in that gap
// still reads its own clock as 0 or more once the late buffer is in.
void clock_never_goes_back() {
  const int64_t id = 9005;
  ad::Graph def(ad::kMixerRate);
  def.output(wave(def, ad::WaveKind::Sine, 220));
  ad::graph_defs().insert_or_assign(id, def);

  Device device(10000);
  device.run(3000);
  g_now += std::chrono::milliseconds(50);  // the next buffer comes late
  play(*device.engine, id);
  double before = device.engine->graph_now(id);
  device.run(1);  // that buffer, dated where the stream really is
  double after = device.engine->graph_now(id);
  CHECK(after >= before, "the graph's clock went from %f back to %f", before, after);
  CHECK(after >= 0, "the graph's clock reads %f, before its start", after);
  g_now = {};
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
  int saw = wave(w, ad::WaveKind::Saw, 220);
  w.output(lowpass(w, saw, 1500, 0.9));
  ad::Graph r(ad::kMixerRate);
  int left = input(r, bus, 0);
  int right = input(r, bus, 1);
  r.output(gain(r, left, 0.5), gain(r, right, 0.25));
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
  int saw = wave(def, ad::WaveKind::Saw, 220);
  int mix = gain(def, saw, 1.0);
  int line = delay(def, mix, 200.0 / ad::kMixerRate);
  int back = gain(def, lowpass(def, line, 2000, 0.9), 0.6);
  CHECK(def.connect(back, mix, -1), "a loop through a delay is refused");
  int short_line = delay(def, back, 37.0 / ad::kMixerRate);
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
  int saw = wave(def, ad::WaveKind::Saw, 220);
  int left = gain(def, saw, 0.3);
  int sine = wave(def, ad::WaveKind::Sine, 330);
  int right = gain(def, sine, 0.4);
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
  def.output(wave(def, ad::WaveKind::Sine, 330));
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

// A definition written as numbers and made again from them (an engine on
// another thread of a browser does) is the same graph: every family of part,
// lines into what parts take in and into their values, a loop through a
// delay, two outputs and each value's changes, a ramp and a cancel among
// them, render the same samples; a graph that reads a bus writes the same
// numbers again; and numbers cut short make none.
void graph_travels_as_numbers() {
  using F = ad::PartFamily;
  ad::Graph g(ad::kMixerRate);
  int saw = part(g, F::Wave, static_cast<int>(ad::WaveKind::Saw), {}, {110, 0.3, 0});
  int noise = part(g, F::Noise, 0, {}, {7});
  int band = part(g, F::Filter, static_cast<int>(ad::FilterKind::Band), {saw, noise}, {900, 2.0});
  int ring = part(g, F::Resonate, 0, {band}, {0.2, 440, 8, 0.5, 1320, 20, 0.25});
  int string = part(g, F::Pluck, 0, {part(g, F::Impulse, 0, {}, {2, 0.25})}, {220, 0.995});
  int mix = part(g, F::Gain, 0, {ring, string}, {0.5});
  int echo = part(g, F::Delay, 0, {mix}, {0.01});
  CHECK(g.connect(part(g, F::Gain, 0, {echo}, {0.4}), mix, -1), "a loop through a delay is refused");
  int wobble = part(g, F::LfNoise, static_cast<int>(ad::LfNoiseKind::Cubic), {}, {3, 11});
  g.connect(part(g, F::Gain, 0, {wobble}, {200}), band, 0);
  int dust = part(g, F::Dust, 0, {}, {30, 5});
  int env = part(g, F::Decay, 0, {part(g, F::Lag, 0, {dust}, {0.01})}, {0.2});
  int held = part(g, F::Latch, 0, {part(g, F::Round, 0, {wobble}, {0.25})}, {0});
  g.connect(dust, held, 0);
  int shaped = part(g, F::Math, static_cast<int>(ad::MathKind::Clip),
                    {part(g, F::Drive, 0, {mix, env, held}, {2})}, {-0.8, 0.8});
  int left = part(g, F::Limit, 0, {shaped}, {0.9, 0.05});
  int right = part(g, F::Room, 0, {left}, {0.6, 0.4, 1});
  g.output(left, right);
  g.schedule(saw, 0, {0.05, false, 220});
  g.schedule(saw, 0, {0.2, true, 330});
  g.schedule(saw, 0, g.held(saw, 0, 0.1));
  g.schedule(right, 0, {0.15, true, 0.9});

  // The first `n` numbers of `xs` decoded, every one of them read.
  auto decoded = [](const std::vector<double>& xs, size_t n) -> std::optional<ad::Graph> {
    ad::NumberReader in(xs.data(), xs.data() + n);
    auto made = ad::Graph::decode(in);
    if (made && in.left() != 0) return std::nullopt;
    return made;
  };
  std::vector<double> numbers;
  g.encode(numbers);
  auto again = decoded(numbers, numbers.size());
  CHECK(again, "the numbers made no graph, or not all of them were read");
  if (!again) return;
  const int64_t n = 30000;
  std::vector<double> l1(n), r1(n), l2(n), r2(n);
  ad::Graph(g).render(l1.data(), r1.data(), n);
  again->render(l2.data(), r2.data(), n);
  CHECK(l1 == l2 && r1 == r2, "the graph made again renders other samples");

  const int64_t bus = 9501;
  ad::bus_defs()[bus] = std::make_shared<ad::Bus>(2);
  ad::Graph reader(ad::kMixerRate);
  reader.output(gain(reader, input(reader, bus, 1), 0.5));
  std::vector<double> first, second;
  reader.encode(first);
  auto read_again = decoded(first, first.size());
  CHECK(read_again, "a graph reading a bus made no graph");
  if (read_again) read_again->encode(second);
  CHECK(first == second, "a graph reading a bus writes other numbers again");
  ad::bus_defs()[bus] = std::make_shared<ad::Bus>(1);
  CHECK(!decoded(first, first.size()), "a graph reading the right of a bus of one was made");
  ad::bus_defs().erase(bus);
  CHECK(!decoded(first, first.size()), "a graph reading a bus freed was made");

  for (size_t cut : {size_t{0}, size_t{3}, numbers.size() / 2, numbers.size() - 1})
    CHECK(!decoded(numbers, cut), "%zu numbers of %zu made a graph", cut, numbers.size());
  // The first part is the saw: [rate, out, right, parts, family, kind, options, ...].
  auto bent = numbers;
  bent[5] = 4;
  CHECK(!decoded(bent, bent.size()), "a wave of no kind was made");
  bent = numbers;
  bent[6] = 2;
  CHECK(!decoded(bent, bent.size()), "a wave of two options was made");
}

// Message `m` taken by `far` as a worker sends it, at `ms` on the wall clock.
void wire_send(ad::WireEngine& far, const std::vector<double>& m, ad::Pcm samples = nullptr, double ms = 0) {
  CHECK(far.apply(m.data(), m.size(), std::move(samples), ms), "message %d was refused", static_cast<int>(m[0]));
}
// Song `id` as a worker sends it: opened, then its slots filled (the rows
// and samples as song_rows and song_pcm make them).
void wire_open_song(ad::WireEngine& far, int64_t id, const std::vector<double>& rows, const ad::PcmByHandle& pcm,
                    double seconds, double ms = 0) {
  auto slotted = ad::slotted_rows(rows.data(), static_cast<int64_t>(rows.size()),
                                  [&](int64_t handle) { return pcm.contains(handle); });
  wire_send(far,
            ad::wire::schedule_open(id, slotted.rows.data(), static_cast<int64_t>(slotted.rows.size()),
                                    static_cast<int64_t>(slotted.handles.size()), seconds, -1),
            nullptr, ms);
  for (size_t k = 0; k < slotted.handles.size(); k++)
    wire_send(far, ad::wire::schedule_fill(id, static_cast<int64_t>(k)), pcm.at(slotted.handles[k]), ms);
}
// Where `far` says the stream is.
int64_t wire_place(ad::WireEngine& far) {
  std::vector<double> status;
  far.status(status);
  return static_cast<int64_t>(status[0]);
}

// An Engine sent its calls as numbers (audio_wire.h, as a browser's worker
// sends them to its AudioWorklet) plays what one called directly plays,
// sample for sample, each call made for the same stream sample: tones, a
// song opened and filled, its volume and room, graphs into a bus and out of
// it with changes while they play, and stops. A reset silences it, and the
// stream's place carries on.
void wire_plays_as_called() {
  const int64_t bus = 9601, writer = 9602, reader = 9603, tune = 9604;
  ad::bus_defs()[bus] = std::make_shared<ad::Bus>(2);
  ad::Graph w(ad::kMixerRate);
  int saw = wave(w, ad::WaveKind::Saw, 220);
  w.output(lowpass(w, saw, 1500, 0.9));
  ad::Graph r(ad::kMixerRate);
  r.output(gain(r, input(r, bus, 0), 0.5), gain(r, input(r, bus, 1), 0.25));
  ad::graph_defs().insert_or_assign(writer, w);
  ad::graph_defs().insert_or_assign(reader, r);
  auto rows = song_rows(1.0, 3);
  auto pcm = song_pcm();

  Device direct(200000);
  ad::WireEngine far;
  std::vector<float> far_left, far_right, buffer(2 * 3 * ad::kMixChunk);
  std::mt19937 rng(99);
  // Both rendered the same `frames`, in buffers of the same sizes.
  auto run = [&](int64_t frames) {
    std::uniform_int_distribution<unsigned> size(1, 3 * ad::kMixChunk);
    while (frames > 0) {
      unsigned n = static_cast<unsigned>(std::min<int64_t>(size(rng), frames));
      direct.engine->render(buffer.data(), n);
      for (unsigned i = 0; i < n; i++) {
        direct.left.push_back(buffer[2 * i]);
        direct.right.push_back(buffer[2 * i + 1]);
      }
      far.render(buffer.data(), n, 0.0);
      for (unsigned i = 0; i < n; i++) {
        far_left.push_back(buffer[2 * i]);
        far_right.push_back(buffer[2 * i + 1]);
      }
      frames -= n;
    }
  };
  auto send = [&](const std::vector<double>& m, ad::Pcm samples = nullptr) { wire_send(far, m, std::move(samples)); };

  run(3000);
  int64_t at = direct.pos() + 100;
  ad::Note note = ad::make_note(440, 660, 2000, 3000, 4000, 2000, 80, 100, 2);
  direct.engine->tone(0, note, at);
  send(ad::wire::tone(0, note, at));
  ad::Note hiss = ad::make_note(800, 200, 0, 0, 3000, 3000, 50, 50, 0);
  direct.engine->tone(ad::kNoise, hiss, at + 500);
  send(ad::wire::tone(ad::kNoise, hiss, at + 500));
  run(5000);

  direct.engine->schedule_new(tune, song(rows, pcm, 1.0));
  wire_open_song(far, tune, rows, pcm, 1.0);
  direct.engine->schedule_volume(tune, 0.7);
  send(ad::wire::schedule(ad::WireOp::ScheduleVolume, tune, 0.7));
  direct.engine->schedule_room(tune, 0.3);
  send(ad::wire::schedule(ad::WireOp::ScheduleRoom, tune, 0.3));
  at = direct.pos() + 300;
  direct.engine->schedule_play(tune, at);
  send(ad::wire::schedule_play(tune, at));
  run(8000);

  at = direct.pos() + 50;
  auto order = ad::graph_played(reader, 0, r.buses_read());
  direct.engine->graph_play(reader, r, nullptr, *order, at);
  send(ad::wire::graph_play(reader, 0, *order, at));
  run(2000);
  at = direct.pos() + 700;
  order = ad::graph_played(writer, bus, w.buses_read());
  direct.engine->graph_play(writer, w, ad::bus_defs().at(bus), *order, at);
  send(ad::wire::graph_play(writer, bus, *order, at));
  run(4000);
  for (int k = 0; k < 2 * static_cast<int>(ad::kGraphLiveRoom); k++) {
    ad::ParamEvent e{0.2 + 0.002 * k, k % 2 == 1, 200.0 + 3 * k};
    direct.engine->graph_post(writer, saw, 0, e);
    send(ad::wire::graph_post(writer, saw, 0, e));
  }
  run(30000);
  direct.engine->graph_stop(writer);
  send(ad::wire::graph_stop(writer));
  direct.engine->schedule_stop(tune);
  send(ad::wire::schedule(ad::WireOp::ScheduleStop, tune));
  run(6000);
  direct.engine->graph_stop(reader);
  send(ad::wire::graph_stop(reader));
  direct.engine->schedule_free(tune);
  send(ad::wire::schedule(ad::WireOp::ScheduleFree, tune));
  run(3000);

  CHECK(differing(direct.left, far_left) == 0 && differing(direct.right, far_right) == 0,
        "the far engine played %ld left and %ld right samples otherwise", differing(direct.left, far_left),
        differing(direct.right, far_right));
  CHECK(std::any_of(far_left.begin(), far_left.end(), [](float v) { return v != 0; }), "the far engine was silent");

  // A reset lets go of what plays, and the stream's place carries on.
  at = direct.pos();
  send(ad::wire::tone(0, ad::make_note(440, 440, 0, 0, 40000, 0, 80, 80, 2), at));
  run(1000);
  auto place = [&] { return static_cast<long long>(wire_place(far)); };
  long long before = place();
  send(ad::wire::reset());
  CHECK(place() == before, "a reset moved the stream from %lld to %lld", before, place());
  size_t heard = far_left.size();
  run(2000);
  CHECK(std::all_of(far_left.begin() + static_cast<std::ptrdiff_t>(heard), far_left.end(),
                    [](float v) { return v == 0; }),
        "a tone sounds on after a reset");
  CHECK(place() == before + 2000, "the stream is at %lld after a reset", place());
  // A reset with a place takes it up: the stream is where the worker said,
  // and as much further on as the wall clock moved before it came.
  const auto anchored = ad::wire::reset(123456, 1000.0);
  CHECK(far.apply(anchored.data(), anchored.size(), nullptr, 1500.0), "a reset with a place was refused");
  CHECK(place() == 123456 + 22050, "a reset with a place put the stream at %lld", place());

  ad::graphs_playing().clear();
  ad::graph_defs().erase(writer);
  ad::graph_defs().erase(reader);
  ad::bus_defs().erase(bus);
}

// A browser's mixer, on the wall clock (ms, Date.now()'s) it is called on:
// reset to stream sample 0 at 1000 ms, as a worker resets it.
struct WallMixer {
  ad::WireEngine engine;
  std::vector<float> left;
  double ms = 1000.0;
  double pace = 1.0;  // the wall clock's ms to a device's, which run apart a little
  int burst = 4;      // the blocks of 128 frames a device asks for at once

  WallMixer() { send(ad::wire::reset(0, ms)); }
  void send(const std::vector<double>& m) { wire_send(engine, m, nullptr, ms); }
  // The stream sample a call made now is dated (the worker's reckoning).
  int64_t now() const { return std::llround((ms - 1000.0) * ad::kSampleRate / 1000.0); }
  // `frames` (a multiple of 128) rendered in step with the wall clock, as an
  // AudioContext renders: bursts of blocks at once, the clock moving on as
  // long as they last before the next.
  void run(int64_t frames) {
    float buffer[2 * 128];
    for (int64_t block = 0; block < frames / 128; block++) {
      engine.render(buffer, 128, ms);
      for (int i = 0; i < 128; i++) left.push_back(buffer[2 * i]);
      if (block % burst == burst - 1) ms += pace * burst * 128 * 1000.0 / ad::kSampleRate;
    }
  }
  // The first sample heard from `from` on; -1 for none.
  int64_t first_heard(size_t from) const {
    for (size_t i = from; i < left.size(); i++)
      if (left[i] != 0) return static_cast<int64_t>(i - from);
    return -1;
  }
  // The samples song `id` held, as the engine says.
  int64_t held(int64_t id) {
    std::vector<double> status;
    engine.status(status);
    for (size_t i = 1; i + 1 < status.size(); i += 2)
      if (status[i] == static_cast<double>(id)) return static_cast<int64_t>(status[i + 1]);
    return -1;
  }
  void open_song(int64_t id, const std::vector<double>& rows, const ad::PcmByHandle& pcm, double seconds) {
    wire_open_song(engine, id, rows, pcm, seconds, ms);
  }
};

// A browser's mixer held still (an AudioContext suspended until the page's
// first gesture) while the worker dates its calls by the wall clock: once it
// renders again the stream takes up the wall clock's place, as if it had
// played on unheard. What was asked for meanwhile sounds at once, a tone and
// a change for now to a graph; a song holds for the time it did not play
// (its clock stands still, which it reports) and plays on from where it was,
// from its top when it was started in that time. Rendering in step with the
// wall clock moves nothing.
void stalled_mixer_keeps_step() {
  const double stall = 2000.0;  // ms
  const int64_t stalled = std::llround(stall * ad::kSampleRate / 1000.0);
  {
    WallMixer far;
    far.run(2048);
    far.ms += stall;
    const size_t from = far.left.size();
    far.send(ad::wire::tone(0, ad::make_note(440, 440, 0, 0, 4000, 0, 80, 80, 2), far.now()));
    far.run(1024);
    const int64_t first = far.first_heard(from);
    CHECK(first >= 0 && first < 64, "a tone asked for after a stall sounds %lld samples in",
          static_cast<long long>(first));
  }
  {
    const int64_t id = 9701;
    ad::Graph def(ad::kMixerRate);
    const int level = gain(def, wave(def, ad::WaveKind::Sine, 440), 0.0);
    def.output(level);
    ad::graph_defs().insert_or_assign(id, def);
    WallMixer far;
    const int64_t start = far.now();
    far.send(ad::wire::graph_play(id, 0, *ad::graph_played(id, 0, def.buses_read()), start));
    far.run(2048);
    far.ms += stall;
    const size_t from = far.left.size();
    const double at = static_cast<double>(far.now() - start) / ad::kMixerRate;
    far.send(ad::wire::graph_post(id, level, 0, ad::ParamEvent{at, false, 0.5, false}));
    far.run(1024);
    const int64_t first = far.first_heard(from);
    CHECK(first >= 0 && first < 64, "a change for now after a stall sounds %lld samples in",
          static_cast<long long>(first));
    ad::graphs_playing().clear();
    ad::graph_defs().erase(id);
  }
  auto rows = song_rows(1.0, 5);
  auto pcm = song_pcm();
  const auto want = rendered(rows, pcm, 1.0, 0.0, 1.0);
  const int64_t length = static_cast<int64_t>(want.size()) + 8192;
  {
    WallMixer far;
    far.open_song(1, rows, pcm, 1.0);
    far.send(ad::wire::schedule_play(1, far.now()));
    far.ms += stall;
    far.run(length / 128 * 128);
    hears("a song started before a stall", far.left, 0, want);
    CHECK(far.held(1) == stalled, "a song started before a stall held %lld samples, not %lld",
          static_cast<long long>(far.held(1)), static_cast<long long>(stalled));
  }
  {
    WallMixer far;
    far.open_song(1, rows, pcm, 1.0);
    far.send(ad::wire::schedule_play(1, far.now()));
    far.run(128 * 80);
    CHECK(far.held(1) == 0 && wire_place(far.engine) == 128 * 80,
          "rendering in step held %lld samples, the stream at %lld", static_cast<long long>(far.held(1)),
          static_cast<long long>(wire_place(far.engine)));
    far.ms += stall;
    far.run(length / 128 * 128);
    hears("a song playing across a stall", far.left, 0, want);
    // The stall, give or take the burst the wall clock was read in.
    CHECK(far.held(1) >= stalled && far.held(1) <= stalled + 512, "a song playing across a stall held %lld samples",
          static_cast<long long>(far.held(1)));
  }
  {
    // A song of 0.1 s whose one sample (0.3 s) rings on past its end.
    const std::vector<double> ringing = {0, -1, 3, 0.5, 0, 0, 0, 0, 0, 0, 0};
    WallMixer far;
    far.open_song(1, ringing, pcm, 0.1);
    far.send(ad::wire::schedule_play(1, far.now()));
    far.run(128 * 50);
    far.ms += stall;
    far.run(128 * 8);
    CHECK(far.held(1) == 0, "a song played through before a stall held %lld samples",
          static_cast<long long>(far.held(1)));
  }
  {
    // A device that renders in long bursts (4096 frames, some 93 ms) and
    // runs slower than the wall clock (5% here, to show in seconds what a
    // real one's drift shows in hours) falls behind it by more than a
    // stall's mark, a little at each render: no stall.
    WallMixer far;
    far.burst = 32;
    far.pace = 1.05;
    const int64_t frames = 6 * ad::kSampleRate / 4096 * 4096;
    far.run(frames);
    CHECK(wire_place(far.engine) == frames, "a device slower than the wall clock moved on to %lld, not %lld",
          static_cast<long long>(wire_place(far.engine)), static_cast<long long>(frames));
  }
}

}  // namespace

int main() {
  graph_travels_as_numbers();
  wire_plays_as_called();
  schedule_plays_as_it_renders();
  room_left_is_the_room_it_was();
  tone_starts_when_asked();
  graph_plays_as_it_renders();
  loop_plays_as_it_renders();
  stereo_graph_plays_as_it_renders();
  clock_never_goes_back();
  late_graph_reads_its_start();
  stalled_mixer_keeps_step();
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
