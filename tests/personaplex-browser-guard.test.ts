import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import vm from "node:vm";

const source = readFileSync(new URL("../home/config/voice/personaplex-browser-guard.js", import.meta.url), "utf8");

function fixture() {
  const tracks: { stopped: boolean; stop(): void }[] = [];
  const streams = () => {
    const track = { stopped: false, stop() { this.stopped = true; } };
    tracks.push(track);
    return { getTracks: () => [track] };
  };
  class Socket extends EventTarget { constructor(..._args: unknown[]) { super(); } }
  const window = Object.assign(new EventTarget(), {
    WebSocket: Socket,
    location: { href: "http://127.0.0.1:8998/" },
  });
  const devices = { getUserMedia: async () => streams() };
  const context = { window, navigator: { mediaDevices: devices }, URL, DOMException };
  return { tracks, streams, window, devices, load: () => vm.runInNewContext(source, context) };
}

test("conversation disconnect releases every microphone stream", async () => {
  const f = fixture(); f.load();
  const ws = new f.window.WebSocket("ws://127.0.0.1:8998/api/chat");
  await f.devices.getUserMedia(); await f.devices.getUserMedia();
  ws.dispatchEvent(new Event("close"));
  assert.equal(f.tracks.length, 2);
  assert.ok(f.tracks.every(track => track.stopped));
});

test("late permission grant after disconnect is stopped and rejected", async () => {
  const f = fixture();
  let resolve!: (stream: ReturnType<typeof f.streams>) => void;
  f.devices.getUserMedia = () => new Promise(r => { resolve = r; });
  f.load();
  const ws = new f.window.WebSocket("ws://127.0.0.1:8998/api/chat");
  const pending = f.devices.getUserMedia();
  ws.dispatchEvent(new Event("close"));
  resolve(f.streams());
  await assert.rejects(pending, { name: "AbortError" });
  assert.ok(f.tracks[0].stopped);
});

test("leaving the page releases audio", async () => {
  const f = fixture(); f.load();
  await f.devices.getUserMedia();
  f.window.dispatchEvent(new Event("pagehide"));
  assert.ok(f.tracks[0].stopped);
});

test("unrelated socket close does not interrupt conversation", async () => {
  const f = fixture(); f.load();
  await f.devices.getUserMedia();
  const ws = new f.window.WebSocket("ws://127.0.0.1:8998/other");
  ws.dispatchEvent(new Event("close"));
  assert.equal(f.tracks[0].stopped, false);
});

test("a new conversation can acquire audio after disconnect", async () => {
  const f = fixture(); f.load();
  const ws = new f.window.WebSocket("ws://127.0.0.1:8998/api/chat");
  await f.devices.getUserMedia();
  ws.dispatchEvent(new Event("close"));
  await f.devices.getUserMedia();
  assert.equal(f.tracks[0].stopped, true);
  assert.equal(f.tracks[1].stopped, false);
});
