#pragma once

// The MIDI inputs and outputs a program has open, by handle, over midilib
// (vendor/cpp-midilib): what a native build's backend
// (src/runtime/culebra_rt_audio.cc) and a browser's (stdlib/audio_web.h) do
// alike. Where the ports and their messages come from is the backend's: the
// system's MIDI natively, the page's Web MIDI handed over through
// midilib::host in a browser; and so is how a program waits for one.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/shared.h"
#include "midilib.h"

namespace culebra {
namespace _audio_detail {
namespace midi {

struct In {
  In(const std::string& port, bool sysex) : port(port), input(port, 4096, sysex) {}
  std::string port;
  midilib::Input input;
  midilib::Controls controls;  // as the messages read so far have set them
};
inline std::unordered_map<int64_t, In>& ins() {
  static std::unordered_map<int64_t, In> open;
  return open;
}
inline std::unordered_map<int64_t, midilib::Output>& outs() {
  static std::unordered_map<int64_t, midilib::Output> open;
  return open;
}
inline In* input(int64_t id) {
  auto it = ins().find(id);
  return it == ins().end() ? nullptr : &it->second;
}

// The inputs (or outputs) there are, each its id and then its name; none
// where the backend cannot start (no CoreMIDI client, in a sandbox).
inline std::vector<std::string> ports(bool output) {
  std::vector<std::string> out;
  try {
    for (auto& p : output ? midilib::output_ports() : midilib::input_ports()) {
      out.push_back(p.id);
      out.push_back(p.name);
    }
  } catch (const midilib::Error&) {
    out.clear();
  }
  return out;
}

inline bool open(int64_t id, const std::string& port, bool sysex) {
  try {
    ins().try_emplace(id, port, sysex);
    return true;
  } catch (const midilib::Error&) {
    return false;
  }
}
inline bool open_output(int64_t id, const std::string& port) {
  try {
    outs().try_emplace(id, port);
    return true;
  } catch (const midilib::Error&) {
    return false;
  }
}
inline void close(int64_t id) {
  ins().erase(id);
  outs().erase(id);
}
inline void close_all() {
  ins().clear();
  outs().clear();
}

// What is wrong with `bytes`, which are not sent; "" when they were.
inline std::string send(int64_t id, const std::vector<uint8_t>& bytes) {
  auto it = outs().find(id);
  if (it == outs().end()) return "the output is closed";
  try {
    it->second.send(bytes);
    return "";
  } catch (const midilib::Error& e) {
    return std::string(str_strip_prefix(e.what(), "midilib: "));
  }
}

// What came since the last read, six numbers a message (seconds ago, the
// status and two data bytes, the size, and a note on's 14-bit velocity, 0
// for the rest), and for a sysex (status 240) its size in place of the data
// bytes, then its bytes.
inline void read(int64_t id, std::vector<double>& out) {
  auto* in = input(id);
  if (!in) return;
  auto now = midilib::now_ns();
  midilib::Message m;
  while (in->input.poll(m)) {
    double ago = static_cast<double>(now - m.time_ns) / 1e9;
    if (m.kind() == midilib::Kind::sysex) {
      out.insert(out.end(), {ago, 240.0, static_cast<double>(m.sysex.size()), 0.0, 0.0, 0.0});
      out.insert(out.end(), m.sysex.begin(), m.sysex.end());
      continue;
    }
    in->controls.apply(m);
    bool on = m.kind() == midilib::Kind::note_on;
    out.insert(out.end(), {ago, static_cast<double>(m.bytes[0]), static_cast<double>(m.bytes[1]),
                           static_cast<double>(m.bytes[2]), static_cast<double>(m.size),
                           on ? static_cast<double>(in->controls.velocity14(m.channel())) : 0.0});
  }
}

inline int64_t control(int64_t id, int64_t channel, int64_t cc) {
  auto* in = input(id);
  return in ? in->controls.value14(static_cast<int>(channel), static_cast<int>(cc)) : 0;
}
inline int64_t dropped(int64_t id) {
  auto* in = input(id);
  return in ? static_cast<int64_t>(in->input.dropped()) : 0;
}

}  // namespace midi
}  // namespace _audio_detail
}  // namespace culebra
