// The upstream UI creates more than one microphone stream. Release every
// stream when its conversation socket closes, including late permission grants.
(() => {
  const streams = new Set();
  let generation = 0;
  const stop = () => {
    generation += 1;
    for (const stream of streams) {
      for (const track of stream.getTracks()) track.stop();
    }
    streams.clear();
  };
  const devices = navigator.mediaDevices;
  const getUserMedia = devices.getUserMedia.bind(devices);
  devices.getUserMedia = async (...args) => {
    const started = generation;
    const stream = await getUserMedia(...args);
    if (started !== generation) {
      for (const track of stream.getTracks()) track.stop();
      throw new DOMException("Conversation ended", "AbortError");
    }
    streams.add(stream);
    return stream;
  };
  const WebSocket = window.WebSocket;
  window.WebSocket = class extends WebSocket {
    constructor(...args) {
      super(...args);
      if (new URL(args[0], window.location.href).pathname === "/api/chat") {
        this.addEventListener("close", stop, { once: true });
      }
    }
  };
  window.addEventListener("pagehide", stop);
})();
