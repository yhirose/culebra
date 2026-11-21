// The worker's half of Page.socket: what page_web.h reads (self.__page) and
// what the page sends it. A program's Page.socket() opens the page's side
// ("pageOpen" to the page); then the page's messages wait here until
// receive() takes them, and one coming wakes a wait for it. What the program
// sends goes to the page as "pageSend", and its close as "pageClose".

// Installs it on `scope` (the worker's self), `wake` waking a wait
// (worker-wake.js); answers the handler of the page's messages, true for one
// of Page.socket's.
export function workerPage(scope, wake) {
  // `session` is the open socket's number (0: none), `sessions` the last given.
  const page = { session: 0, sessions: 0, closed: false, queue: [] };
  scope.__page = page;
  return (data) => {
    switch (data.type) {
      case "pageMessage":
        if (page.session !== 0 && !page.closed) {
          page.queue.push(data.text);
          wake();
        }
        return true;
      case "pageGone":
        page.closed = true;
        wake();
        return true;
      default:
        return false;
    }
  };
}
