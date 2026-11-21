// One wait in the worker for whatever the page hands it: a MIDI message of an
// input held open (web-midi.js), a message for Page.socket (web-page.js). A
// program that waits on one is woken by either, so a loop that reads both, as
// the FM Synth's does, sleeps until something comes. What the native side
// awaits (audio_web.h, page_web.h) is scope.__inputWait.

// Installs the wait on `scope` (the worker's self); answers what wakes it.
export function workerWake(scope) {
  let wake = null;
  scope.__inputWait = (ms) =>
    new Promise((resolve) => {
      const timer = setTimeout(() => {
        wake = null;
        resolve();
      }, ms);
      wake = () => {
        clearTimeout(timer);
        wake = null;
        resolve();
      };
    });
  return () => {
    if (wake) wake();
  };
}
