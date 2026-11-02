// tests/midi_test.sh's other end: a virtual CoreMIDI source, "culebra midi
// test", that waits for a line on the FIFO given (the reader saying it has
// opened the source) and then sends a fixed run: breath at 14 bits, a note
// with its CC 88 velocity, a legato step (the new note on and the old note
// off in one packet), a bend, a channel pressure, a program, a note on at
// velocity 0.
#include <CoreMIDI/CoreMIDI.h>

#include <chrono>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

static MIDIEndpointRef source;

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

int main(int argc, char **argv) {
  if (argc < 2) { return 2; }
  MIDIClientRef client;
  if (MIDIClientCreateWithBlock(CFSTR("culebra midi test"), &client, ^(const MIDINotification *) {}) != noErr ||
      MIDISourceCreateWithProtocol(client, CFSTR("culebra midi test"), kMIDIProtocol_1_0, &source) != noErr) {
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
  send({ump(0xE0, 0x7F, 0x7F), ump(0xD0, 64), ump(0xC0, 5), ump(0x90, 62, 0)});
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  return 0;
}
