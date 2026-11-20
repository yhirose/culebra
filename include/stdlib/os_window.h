// The one OS window a Canvas or Scene process opens, asked of SDL directly.
//
// raylib keeps its own copy of the fullscreen state, a flag only its own
// ToggleFullscreen() flips. A window the OS takes fullscreen — macOS's green
// button or Window > Enter Full Screen (Ctrl+Cmd+F), a Linux window manager's
// key — reads as windowed to IsWindowFullscreen(), and the next toggle asks
// for the state the window is already in. SDL hears the OS's events, so both
// backends ask it instead, here.
//
// raylib's GetWindowHandle() returns the platform window (an NSWindow* on
// macOS), not the SDL_Window*, so the window comes from SDL's own list.
#pragma once

#include <SDL3/SDL_video.h>

namespace culebra::os_window {

// The process's window, or nullptr while there is none (or not exactly one).
inline SDL_Window* the() {
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  if (windows == nullptr) return nullptr;
  SDL_Window* w = count == 1 ? windows[0] : nullptr;
  SDL_free(windows);
  return w;
}

// `w` is the() unless a caller already has it (the lookup lists SDL's windows).
inline bool fullscreen(SDL_Window* w = the()) {
  return w != nullptr && (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN) != 0;
}

// Waits for the OS to finish the change (macOS animates into its own Space),
// so the next fullscreen() reads the new state and a second toggle in quick
// succession undoes the first rather than repeating it.
inline void set_fullscreen(bool on, SDL_Window* w = the()) {
  if (w == nullptr || on == fullscreen(w)) return;
  SDL_SetWindowFullscreen(w, on);
  SDL_SyncWindow(w);
}

}  // namespace culebra::os_window
