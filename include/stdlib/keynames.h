// Names for raylib's key, gamepad-button and gamepad-axis codes — the one
// vocabulary the Canvas and Scene backends share, so input code moves between
// the two namespaces unchanged. The key names are `Term.read_key`'s, so they
// move to the terminal too. It also puts gamepad axis values on one scale
// (pad_axis_value).
//
// A key name is a printable character ("a", " ", "-") or a special-key name
// ("left", "enter", "f1", …), or "shift" for either Shift key (Canvas and
// Scene only: Term reports Shift as a field of the key event). raylib's
// printable key codes are upper-case ASCII, so single characters map by
// arithmetic and only the specials need a table. Unknown names map to 0 / -1
// and are never held — a typo is a key that is never pressed, not an error,
// the same contract Canvas.key has always had.
#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

#include "raylib.h"

namespace culebra::keynames {

inline const std::unordered_map<std::string_view, int>& special_keys() {
  static const std::unordered_map<std::string_view, int> m = {
      {"space", KEY_SPACE},     // the readable alias for " "
      {"up", KEY_UP},           {"down", KEY_DOWN},
      {"left", KEY_LEFT},       {"right", KEY_RIGHT},
      {"enter", KEY_ENTER},     {"escape", KEY_ESCAPE},
      {"tab", KEY_TAB},         {"backspace", KEY_BACKSPACE},
      {"insert", KEY_INSERT},   {"delete", KEY_DELETE},
      {"home", KEY_HOME},       {"end", KEY_END},
      {"pageup", KEY_PAGE_UP},  {"pagedown", KEY_PAGE_DOWN},
      {"f1", KEY_F1},  {"f2", KEY_F2},   {"f3", KEY_F3},  {"f4", KEY_F4},
      {"f5", KEY_F5},  {"f6", KEY_F6},   {"f7", KEY_F7},  {"f8", KEY_F8},
      {"f9", KEY_F9},  {"f10", KEY_F10}, {"f11", KEY_F11}, {"f12", KEY_F12},
  };
  return m;
}

inline int key_code_of(std::string_view name) {
  if (name.size() == 1) {
    unsigned char c = static_cast<unsigned char>(name[0]);
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= ' ' && c <= '~') return c;
    return 0;
  }
  auto it = special_keys().find(name);
  return it == special_keys().end() ? 0 : it->second;
}

// The physical keys a name stands for: one, or both sides of a modifier
// ("shift" is either Shift). No codes is an unknown name.
struct KeyCodes {
  int code[2] = {0, 0};
  int count = 0;
};

inline KeyCodes key_codes_of(std::string_view name) {
  if (name == "shift") return {{KEY_LEFT_SHIFT, KEY_RIGHT_SHIFT}, 2};
  int c = key_code_of(name);
  if (c == 0) return {};
  return {{c, 0}, 1};
}

enum class KeyEdge { Held, Pressed, Released };

// A name's state from its keys' states; `query(code, edge)` answers for one
// physical key (raylib below, a stub in test/keynames_test.cc). The keys of
// one name act as one key: it goes down when the first of them does and
// comes up when the last does, so pressing the second Shift is no new press.
template <class Query>
bool key_state(std::string_view name, KeyEdge edge, Query query) {
  KeyCodes keys = key_codes_of(name);
  bool now = false, before = false;
  for (int i = 0; i < keys.count; i++) {
    bool down = query(keys.code[i], KeyEdge::Held);
    // Last frame's state: up if it went down now, down if it came up now.
    bool was = query(keys.code[i], KeyEdge::Pressed)    ? false
               : query(keys.code[i], KeyEdge::Released) ? true
                                                        : down;
    now = now || down;
    before = before || was;
  }
  switch (edge) {
    case KeyEdge::Held: return now;
    case KeyEdge::Pressed: return now && !before;
    case KeyEdge::Released: return !now && before;
  }
  return false;
}

inline bool raylib_key(int code, KeyEdge edge) {
  switch (edge) {
    case KeyEdge::Held: return IsKeyDown(code);
    case KeyEdge::Pressed: return IsKeyPressed(code);
    case KeyEdge::Released: return IsKeyReleased(code);
  }
  return false;
}

// What Canvas.key and view.key / key_pressed / key_released read.
inline bool key_state(std::string_view name, KeyEdge edge) {
  return key_state(name, edge, raylib_key);
}

// The queue direction: a pressed key code back to its name. Unmapped codes
// (modifiers, keypad) produce "" and are dropped from the queue, the same
// keys Term's parser has no name for. Space reads back as " ", its canonical
// spelling, not the alias.
inline std::string key_name_of(int code) {
  if (code >= 'A' && code <= 'Z') return std::string(1, static_cast<char>(code + 32));
  if (code >= ' ' && code <= '~') return std::string(1, static_cast<char>(code));
  for (const auto& [name, c] : special_keys())
    if (c == code && name != "space") return std::string(name);
  return "";
}

// Gamepad buttons by the names Canvas's PAD_* constants already spell, lower-
// cased: the face buttons by their Xbox letters (SDL's mapping DB normalizes
// every pad to that layout, so "a" is Cross on a PlayStation pad), the
// shoulders lb/rb, the triggers-as-buttons lt/rt, the stick clicks l3/r3.
inline int pad_button_of(std::string_view name) {
  static const std::unordered_map<std::string_view, int> m = {
      {"up", GAMEPAD_BUTTON_LEFT_FACE_UP},
      {"right", GAMEPAD_BUTTON_LEFT_FACE_RIGHT},
      {"down", GAMEPAD_BUTTON_LEFT_FACE_DOWN},
      {"left", GAMEPAD_BUTTON_LEFT_FACE_LEFT},
      {"y", GAMEPAD_BUTTON_RIGHT_FACE_UP},
      {"b", GAMEPAD_BUTTON_RIGHT_FACE_RIGHT},
      {"a", GAMEPAD_BUTTON_RIGHT_FACE_DOWN},
      {"x", GAMEPAD_BUTTON_RIGHT_FACE_LEFT},
      {"lb", GAMEPAD_BUTTON_LEFT_TRIGGER_1},
      {"lt", GAMEPAD_BUTTON_LEFT_TRIGGER_2},
      {"rb", GAMEPAD_BUTTON_RIGHT_TRIGGER_1},
      {"rt", GAMEPAD_BUTTON_RIGHT_TRIGGER_2},
      {"select", GAMEPAD_BUTTON_MIDDLE_LEFT},
      {"guide", GAMEPAD_BUTTON_MIDDLE},
      {"start", GAMEPAD_BUTTON_MIDDLE_RIGHT},
      {"l3", GAMEPAD_BUTTON_LEFT_THUMB},
      {"r3", GAMEPAD_BUTTON_RIGHT_THUMB},
  };
  auto it = m.find(name);
  return it == m.end() ? GAMEPAD_BUTTON_UNKNOWN : it->second;
}

// Mouse buttons. -1 names no button; a caller reads that as never pressed.
inline int mouse_button_of(std::string_view name) {
  if (name == "left") return MOUSE_BUTTON_LEFT;
  if (name == "right") return MOUSE_BUTTON_RIGHT;
  if (name == "middle") return MOUSE_BUTTON_MIDDLE;
  return -1;
}

// Gamepad axes: the two sticks by half, and the analogue triggers. -1 names
// no axis; a caller reads that as 0.0, so an unknown name is a centred stick.
inline int pad_axis_of(std::string_view name) {
  static const std::unordered_map<std::string_view, int> m = {
      {"lx", GAMEPAD_AXIS_LEFT_X},        {"ly", GAMEPAD_AXIS_LEFT_Y},
      {"rx", GAMEPAD_AXIS_RIGHT_X},       {"ry", GAMEPAD_AXIS_RIGHT_Y},
      {"lt", GAMEPAD_AXIS_LEFT_TRIGGER},  {"rt", GAMEPAD_AXIS_RIGHT_TRIGGER},
  };
  auto it = m.find(name);
  return it == m.end() ? -1 : it->second;
}

// An axis on the scale pad_axis promises: a stick -1..1 centred at 0, a
// trigger -1 released, 1 fully pulled (raylib's own convention). raylib's
// SDL backend divides every axis by 32767, but SDL reports a trigger
// 0..32767, so a stored trigger reads 0..1, or -1 until it first moves.
inline double pad_axis_value(int axis, double stored) {
  if (axis != GAMEPAD_AXIS_LEFT_TRIGGER && axis != GAMEPAD_AXIS_RIGHT_TRIGGER)
    return stored;
  return stored < 0.0 ? -1.0 : 2.0 * stored - 1.0;
}

}  // namespace culebra::keynames
