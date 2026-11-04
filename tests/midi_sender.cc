// tests/midi_test.sh's other end: a virtual CoreMIDI source and destination,
// both "culebra midi test". It waits for a line on the FIFO given (the
// reader saying it has opened the source) and then sends a fixed run: breath
// at 14 bits, a note with its CC 88 velocity, a legato step (the new note on
// and the old note off in one packet), a bend, a channel pressure, a program,
// a sysex in two packets, a note on at velocity 0. Then it prints, as hex
// bytes on one line, what the destination is sent until a note on of note 0
// comes (or 60 s pass: midi_test.sh stops it sooner when the reader fails,
// and a reader slowed by a loaded machine still finds the destination).
#include <CoreMIDI/CoreMIDI.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static MIDIEndpointRef source;
static std::mutex mutex;
static std::vector<uint8_t> heard;
static bool ended = false;

static uint32_t ump(uint8_t status, uint8_t d1, uint8_t d2 = 0) {
  return (0x2u << 28) | (uint32_t(status) << 16) | (uint32_t(d1) << 8) | d2;
}

static void send(std::vector<uint32_t> words) {
  alignas(8) Byte buf[1024];
  auto list = reinterpret_cast<MIDIEventList *>(buf);
  auto p = MIDIEventListInit(list, kMIDIProtocol_1_0);
  MIDIEventListAdd(list, sizeof buf, p, 0, words.size(), words.data());
  MIDIReceivedEventList(source, list);
}

// What the destination is sent, as MIDI 1.0 bytes: channel messages (UMP
// type 2) and sysexes (type 3, F0 on a start, F7 on an end).
static void hear(const MIDIEventList *list) {
  std::lock_guard<std::mutex> lock(mutex);
  auto p = &list->packet[0];
  for (UInt32 i = 0; i < list->numPackets; i++) {
    for (UInt32 w = 0; w < p->wordCount; w++) {
      auto word = p->words[w];
      if (word >> 28 == 0x2) {
        uint8_t status = word >> 16;
        heard.push_back(status);
        heard.push_back((word >> 8) & 0x7F);
        if ((status & 0xF0) != 0xC0 && (status & 0xF0) != 0xD0) { heard.push_back(word & 0x7F); }
        ended = ended || (status == 0x90 && ((word >> 8) & 0x7F) == 0);
      } else if (word >> 28 == 0x3 && w + 1 < p->wordCount) {
        auto status = (word >> 20) & 0xF, n = (word >> 16) & 0xF, next = p->words[++w];
        uint8_t data[6] = {uint8_t(word >> 8), uint8_t(word), uint8_t(next >> 24),
                           uint8_t(next >> 16), uint8_t(next >> 8), uint8_t(next)};
        if (status == 0 || status == 1) { heard.push_back(0xF0); }
        heard.insert(heard.end(), data, data + (n > 6 ? 6 : n));
        if (status == 0 || status == 3) { heard.push_back(0xF7); }
      }
    }
    p = MIDIEventPacketNext(p);
  }
}

int main(int argc, char **argv) {
  if (argc < 2) { return 2; }
  MIDIClientRef client;
  MIDIEndpointRef dest;
  if (MIDIClientCreateWithBlock(CFSTR("culebra midi test"), &client, ^(const MIDINotification *) {}) != noErr ||
      MIDISourceCreateWithProtocol(client, CFSTR("culebra midi test"), kMIDIProtocol_1_0, &source) != noErr ||
      MIDIDestinationCreateWithProtocol(client, CFSTR("culebra midi test"), kMIDIProtocol_1_0, &dest,
                                        ^(const MIDIEventList *list, void *) { hear(list); }) != noErr) {
    return 1;
  }
  std::ifstream ready(argv[1]);
  std::string line;
  std::getline(ready, line);
  // The connection is made in the background: give it a moment.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  send({ump(0xB0, 2, 0x40), ump(0xB0, 34, 0x10)});
  send({ump(0xB0, 88, 0x22), ump(0x90, 60, 80)});
  send({ump(0x90, 62, 90), ump(0x80, 60, 0)});
  send({ump(0xE0, 0x7F, 0x7F), ump(0xD0, 64), ump(0xC0, 5)});
  send({0x3016477Fu, 0x6D420000u});  // F0 47 7F 6D 42 00 00 ...
  send({0x30320007u, 0x00000000u, ump(0x90, 62, 0)});  // ... 00 07 F7
  for (int i = 0; i < 6000; i++) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (ended) { break; }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::lock_guard<std::mutex> lock(mutex);
  for (size_t i = 0; i < heard.size(); i++) { std::printf("%s%02X", i ? " " : "", heard[i]); }
  std::printf("\n");
  return 0;
}
