// Unit test for the key-name state (include/stdlib/keynames.h): what
// Canvas.key and view.key / key_pressed / key_released report for a name,
// given what raylib reports for each physical key, and what pad_axis reports
// for a stored axis value.
//
// The query is a stub here, so the frames a window would produce are written
// out: which keys are down, which went down this frame, which came up. The
// case this exists for is a name over two keys ("shift", or "0" and its keypad
// twin), which must read as one key (a second Shift going down is not a new
// press), in the held state and in the queue of presses alike.
//
// Built and run by CTest (see CMakeLists.txt).

#include <stdlib/keynames.h>

#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace {

using culebra::keynames::KeyEdge;

int failures = 0;

void check(bool ok, const std::string& what) {
  if (ok) return;
  std::printf("FAIL: %s\n", what.c_str());
  failures++;
}

// One frame as raylib sees it. `pressed` keys are also down; `released` keys
// are not.
struct Frame {
  std::set<int> down, pressed, released;
  bool operator()(int code, KeyEdge edge) const {
    switch (edge) {
      case KeyEdge::Held: return down.count(code) > 0;
      case KeyEdge::Pressed: return pressed.count(code) > 0;
      case KeyEdge::Released: return released.count(code) > 0;
    }
    return false;
  }
};

// Held, pressed, released for one name in one frame, as "HPR" / "---".
std::string state(const char* name, const Frame& f) {
  std::string s;
  s += culebra::keynames::key_state(name, KeyEdge::Held, f) ? 'H' : '-';
  s += culebra::keynames::key_state(name, KeyEdge::Pressed, f) ? 'P' : '-';
  s += culebra::keynames::key_state(name, KeyEdge::Released, f) ? 'R' : '-';
  return s;
}

void eq(const char* name, const Frame& f, const std::string& want,
        const std::string& what) {
  std::string got = state(name, f);
  if (got == want) return;
  std::printf("FAIL: %s (%s)\n  got:  %s\n  want: %s\n", what.c_str(), name,
              got.c_str(), want.c_str());
  failures++;
}

void names() {
  auto e = culebra::keynames::key_codes_of("e");
  check(e.count == 1 && e.code[0] == KEY_E, "\"e\" is the E key");
  auto sh = culebra::keynames::key_codes_of("shift");
  check(sh.count == 2 && sh.code[0] == KEY_LEFT_SHIFT &&
            sh.code[1] == KEY_RIGHT_SHIFT,
        "\"shift\" is both Shift keys");
  auto ctrl = culebra::keynames::key_codes_of("ctrl");
  check(ctrl.count == 2 && ctrl.code[0] == KEY_LEFT_CONTROL &&
            ctrl.code[1] == KEY_RIGHT_CONTROL,
        "\"ctrl\" is both Control keys");
  auto alt = culebra::keynames::key_codes_of("alt");
  check(alt.count == 2 && alt.code[0] == KEY_LEFT_ALT &&
            alt.code[1] == KEY_RIGHT_ALT,
        "\"alt\" is both Alt keys");
  auto cmd = culebra::keynames::key_codes_of("cmd");
  check(cmd.count == 2 && cmd.code[0] == KEY_LEFT_SUPER &&
            cmd.code[1] == KEY_RIGHT_SUPER,
        "\"cmd\" is both Command (Super) keys");
  auto zero = culebra::keynames::key_codes_of("0");
  check(zero.count == 2 && zero.code[0] == KEY_ZERO && zero.code[1] == KEY_KP_0,
        "\"0\" is the 0 key and keypad 0");
  auto enter = culebra::keynames::key_codes_of("enter");
  check(enter.count == 2 && enter.code[0] == KEY_ENTER &&
            enter.code[1] == KEY_KP_ENTER,
        "\"enter\" is Enter and the keypad's Enter");
  // The queue direction reads the same table back.
  using culebra::keynames::key_name_of;
  struct Back {
    int code;
    const char* name;
  } back[] = {
      {KEY_LEFT_SHIFT, "shift"}, {KEY_RIGHT_CONTROL, "ctrl"},
      {KEY_LEFT_ALT, "alt"},     {KEY_RIGHT_SUPER, "cmd"},
      {KEY_KP_0, "0"},           {KEY_KP_9, "9"},
      {KEY_KP_DECIMAL, "."},     {KEY_KP_DIVIDE, "/"},
      {KEY_KP_MULTIPLY, "*"},    {KEY_KP_SUBTRACT, "-"},
      {KEY_KP_ADD, "+"},         {KEY_KP_EQUAL, "="},
      {KEY_KP_ENTER, "enter"},   {KEY_ENTER, "enter"},
      {KEY_SEVEN, "7"},          {KEY_E, "e"},
  };
  for (const auto& b : back)
    check(key_name_of(b.code) == b.name,
          std::string("code ") + std::to_string(b.code) + " reads back as \"" +
              b.name + "\"");
  check(culebra::keynames::key_codes_of("nosuchkey").count == 0,
        "an unknown name has no keys");
  check(culebra::keynames::key_codes_of("Shift").count == 0,
        "names are lower case");
}

// A one-key name reports exactly what raylib reports for its key.
void one_key() {
  eq("e", {}, "---", "up");
  eq("e", {{KEY_E}, {KEY_E}, {}}, "HP-", "goes down");
  eq("e", {{KEY_E}, {}, {}}, "H--", "held");
  eq("e", {{}, {}, {KEY_E}}, "--R", "comes up");
  eq("[", {{KEY_LEFT_BRACKET}, {KEY_LEFT_BRACKET}, {}}, "HP-", "a punctuation key");
  eq("nosuchkey", {{KEY_E}, {KEY_E}, {}}, "---", "an unknown name is never down");
}

// "shift" is one key made of two: down while either is, pressed when the
// first goes down, released when the last comes up.
void shift() {
  const int L = KEY_LEFT_SHIFT, R = KEY_RIGHT_SHIFT;
  eq("shift", {}, "---", "neither");
  eq("shift", {{L}, {L}, {}}, "HP-", "left goes down");
  eq("shift", {{R}, {R}, {}}, "HP-", "right goes down");
  eq("shift", {{L}, {}, {}}, "H--", "left held");
  eq("shift", {{L, R}, {R}, {}}, "H--", "right goes down while left is held");
  eq("shift", {{L, R}, {L, R}, {}}, "HP-", "both go down together");
  eq("shift", {{R}, {}, {L}}, "H--", "left comes up while right is held");
  eq("shift", {{}, {}, {R}}, "--R", "the last comes up");
  eq("shift", {{}, {}, {L, R}}, "--R", "both come up together");
  eq("shift", {{L}, {L}, {R}}, "H--", "one comes up as the other goes down");
  // A letter is its own key: Shift does not change what "e" reads.
  eq("e", {{L, KEY_E}, {KEY_E}, {}}, "HP-", "E with Shift held");
}

// A key and its keypad twin are one name, as the two sides of Shift are.
void keypad() {
  const int M = KEY_ZERO, K = KEY_KP_0;
  eq("0", {{K}, {K}, {}}, "HP-", "keypad 0 goes down");
  eq("0", {{M, K}, {K}, {}}, "H--", "keypad 0 goes down while 0 is held");
  eq("0", {{}, {}, {K}}, "--R", "keypad 0 comes up");
  eq("enter", {{KEY_KP_ENTER}, {KEY_KP_ENTER}, {}}, "HP-", "the keypad's Enter");
}

// The queue of presses: each key going down adds its name, except the second
// key of a name while the first is held.
void queue() {
  using culebra::keynames::pressed_names;
  const int L = KEY_LEFT_SHIFT, R = KEY_RIGHT_SHIFT;
  struct Case {
    std::vector<int> codes;
    Frame frame;
    std::vector<std::string> want;
    const char* what;
  } cases[] = {
      {{KEY_E}, {{KEY_E}, {KEY_E}, {}}, {"e"}, "a letter"},
      {{L}, {{L}, {L}, {}}, {"shift"}, "Shift goes down"},
      {{KEY_LEFT_SUPER}, {{KEY_LEFT_SUPER}, {KEY_LEFT_SUPER}, {}}, {"cmd"},
       "Command goes down"},
      {{R}, {{L, R}, {R}, {}}, {}, "right Shift while left is held"},
      {{L, R}, {{L, R}, {L, R}, {}}, {"shift"}, "both Shifts in one batch"},
      {{KEY_KP_0}, {{KEY_KP_0}, {KEY_KP_0}, {}}, {"0"}, "keypad 0"},
      {{KEY_KP_0}, {{KEY_ZERO, KEY_KP_0}, {KEY_KP_0}, {}}, {},
       "keypad 0 while 0 is held"},
      {{KEY_KP_ENTER}, {{KEY_KP_ENTER}, {KEY_KP_ENTER}, {}}, {"enter"},
       "the keypad's Enter"},
      {{L, KEY_E}, {{L, KEY_E}, {L, KEY_E}, {}}, {"shift", "e"},
       "Shift then E, in order"},
      {{999}, {{999}, {999}, {}}, {}, "an unnamed key adds nothing"},
  };
  for (const auto& c : cases) {
    auto got = pressed_names(c.codes, c.frame);
    if (got == c.want) continue;
    std::string g, w;
    for (const auto& n : got) g += "[" + n + "]";
    for (const auto& n : c.want) w += "[" + n + "]";
    std::printf("FAIL: queue: %s\n  got:  %s\n  want: %s\n", c.what, g.c_str(),
                w.c_str());
    failures++;
  }
}

// pad_axis_value (keynames.h says why triggers are rescaled): a trigger reads
// -1 released, 1 full; a stick passes through.
void axes() {
  using culebra::keynames::pad_axis_value;
  struct Case {
    int axis;
    double stored, want;
    const char* what;
  } cases[] = {
      {GAMEPAD_AXIS_RIGHT_TRIGGER, -1.0, -1.0, "rt before it first moves"},
      {GAMEPAD_AXIS_RIGHT_TRIGGER, 0.0, -1.0, "rt released after a pull"},
      {GAMEPAD_AXIS_RIGHT_TRIGGER, 0.5, 0.0, "rt half pulled"},
      {GAMEPAD_AXIS_RIGHT_TRIGGER, 1.0, 1.0, "rt fully pulled"},
      {GAMEPAD_AXIS_LEFT_TRIGGER, 0.0, -1.0, "lt released after a pull"},
      {GAMEPAD_AXIS_LEFT_TRIGGER, 1.0, 1.0, "lt fully pulled"},
      {GAMEPAD_AXIS_LEFT_X, 0.0, 0.0, "a stick centred"},
      {GAMEPAD_AXIS_LEFT_Y, -1.0, -1.0, "a stick pushed up"},
      {GAMEPAD_AXIS_RIGHT_X, 0.5, 0.5, "a stick half right"},
  };
  for (const auto& c : cases)
    check(pad_axis_value(c.axis, c.stored) == c.want, c.what);
}

}  // namespace

int main() {
  names();
  one_key();
  shift();
  keypad();
  queue();
  axes();
  if (failures == 0) std::printf("keynames_test: all passed\n");
  return failures == 0 ? 0 : 1;
}
