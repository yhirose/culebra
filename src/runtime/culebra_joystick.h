#pragma once
// Generic joysticks (flight sticks, yokes, throttles, pedals) read straight
// from SDL3, for the Canvas and Scene windows alike. raylib only knows
// gamepads -- a device SDL's mapping DB lists -- so a joystick outside that DB
// is invisible to pad_*; this reads every joystick SDL sees, gamepads
// included, by its raw axes, buttons and hats (the GLFW glfwGetJoystick* /
// pygame.joystick model). Values are raw: no dead zone, no curve, no
// remapping -- those belong to the program, keyed by the device's GUID.
//
// `index` is the device's place in SDL's list (connection order). A device is
// opened on first read and SDL keeps its state current through the event pump
// the window already runs; a missing index reads as nothing connected. A
// device unplugged and plugged back in comes back under a new id and is
// opened again; the old handle is released at SDL_Quit.
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_stdinc.h>

#include <cstdint>
#include <string>

namespace culebra::joystick {

inline SDL_JoystickID id_at(int64_t index) {
  int n = 0;
  SDL_JoystickID* ids = SDL_GetJoysticks(&n);
  SDL_JoystickID id = (ids && index >= 0 && index < n) ? ids[index] : 0;
  SDL_free(ids);
  return id;
}

inline SDL_Joystick* at(int64_t index) {
  SDL_JoystickID id = id_at(index);
  if (id == 0) return nullptr;
  SDL_Joystick* j = SDL_GetJoystickFromID(id);
  return j ? j : SDL_OpenJoystick(id);
}

inline int64_t count() {
  int n = 0;
  SDL_free(SDL_GetJoysticks(&n));
  return n;
}

inline std::string name(int64_t index) {
  SDL_JoystickID id = id_at(index);
  const char* s = id ? SDL_GetJoystickNameForID(id) : nullptr;
  return s ? s : "";
}

inline std::string guid(int64_t index) {
  SDL_JoystickID id = id_at(index);
  if (id == 0) return "";
  char buf[33];
  SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), buf, sizeof buf);
  return buf;
}

inline int64_t axes(int64_t index) {
  SDL_Joystick* j = at(index);
  return j ? SDL_GetNumJoystickAxes(j) : 0;
}
inline int64_t buttons(int64_t index) {
  SDL_Joystick* j = at(index);
  return j ? SDL_GetNumJoystickButtons(j) : 0;
}
inline int64_t hats(int64_t index) {
  SDL_Joystick* j = at(index);
  return j ? SDL_GetNumJoystickHats(j) : 0;
}

// -1..1 with 0 at SDL's centre; an axis the device lacks reads 0.
inline double axis(int64_t index, int64_t axis) {
  SDL_Joystick* j = at(index);
  if (!j || axis < 0 || axis >= SDL_GetNumJoystickAxes(j)) return 0.0;
  int v = SDL_GetJoystickAxis(j, static_cast<int>(axis));
  return v < 0 ? v / 32768.0 : v / 32767.0;
}

inline bool button(int64_t index, int64_t button) {
  SDL_Joystick* j = at(index);
  if (!j || button < 0 || button >= SDL_GetNumJoystickButtons(j)) return false;
  return SDL_GetJoystickButton(j, static_cast<int>(button));
}

// The hat's bits: 1 up, 2 right, 4 down, 8 left (SDL's and GLFW's values; a
// diagonal sets two). 0 is centred, and so is a hat the device lacks.
inline int64_t hat(int64_t index, int64_t hat) {
  SDL_Joystick* j = at(index);
  if (!j || hat < 0 || hat >= SDL_GetNumJoystickHats(j)) return 0;
  return SDL_GetJoystickHat(j, static_cast<int>(hat));
}

}  // namespace culebra::joystick
