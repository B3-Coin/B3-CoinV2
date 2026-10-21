// One page refresh at a time, without losing a request or showing a
// superseded reply.

/** Runs one refresh at a time. A request made while one runs is not
 *  dropped: the refresh runs again afterwards, and `current()` turns false
 *  for the running one, whose reply may belong to the previous wallet and
 *  must not be shown. */
export function single(refresh: (current: () => boolean) => Promise<void>): () => Promise<void> {
  let running = false;
  let again = false;
  return async () => {
    if (running) {
      again = true;
      return;
    }
    running = true;
    try {
      do {
        again = false;
        await refresh(() => !again);
      } while (again);
    } finally {
      running = false;
    }
  };
}
