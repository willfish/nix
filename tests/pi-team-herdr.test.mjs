import test from "node:test";
import assert from "node:assert/strict";
import net from "node:net";
import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { setTimeout as delay } from "node:timers/promises";
import { HerdrPanes, socketCall, balanceOwnedSubtree } from "../home/config/pi/extensions/subagent/herdr.js";

const pane = (pane_id) => ({ type: "pane", pane_id });
const split = (direction, first, second, ratio = 0.5) => ({ type: "split", direction, first, second, ratio });
const missing = () => Object.assign(new Error("pane not found"), { code: "pane_not_found" });
function ids(root) { return root.type === "pane" ? [root.pane_id] : [...ids(root.first), ...ids(root.second)]; }
function transform(root, id, fn) {
  if (root.type === "pane") return root.pane_id === id ? fn(root) : root;
  const first = transform(root.first, id, fn), second = transform(root.second, id, fn);
  return first && second ? { ...root, first, second } : first ?? second;
}
function fake(root = pane("parent")) {
  const state = { root, calls: [], next: 1, active: 0, peak: 0, failClose: null };
  state.call = async (method, params) => {
    state.calls.push({ method, params: structuredClone(params) });
    state.peak = Math.max(state.peak, ++state.active);
    try {
      await delay(1);
      if (method === "layout.export") {
        assert.equal(params.pane_id, "parent");
        return { type: "layout_export", layout: { tab_id: "w1:t1", root: structuredClone(state.root) } };
      }
      if (method === "pane.split") {
        assert.ok(ids(state.root).includes(params.target_pane_id));
        assert.equal(params.focus, false);
        assert.equal(params.ratio, 0.5);
        assert.equal(Object.hasOwn(params, "command"), false);
        const child = `w1:p${state.next++}`;
        state.root = transform(state.root, params.target_pane_id, (old) => split(params.direction, old, pane(child)));
        return { type: "pane_split", pane: { pane_id: child } };
      }
      if (method === "layout.set_split_ratio") {
        assert.equal(params.tab_id, "w1:t1");
        let node = state.root;
        for (const step of params.path) node = step ? node.second : node.first;
        assert.equal(node.type, "split");
        node.ratio = params.ratio;
        return {};
      }
      if (method === "pane.close") {
        if (state.failClose) throw state.failClose;
        if (!ids(state.root).includes(params.pane_id)) throw missing();
        state.root = transform(state.root, params.pane_id, () => null);
        return {};
      }
      if (method === "pane.get") {
        if (!ids(state.root).includes(params.pane_id)) throw missing();
        return { pane: { pane_id: params.pane_id } };
      }
      if (method === "pane.rename") return {};
      assert.fail(`Unexpected method ${method}`);
    } finally { state.active--; }
  };
  state.panes = new HerdrPanes({ parentPaneId: "parent", call: state.call });
  return state;
}
function heights(node, height = 1, result = {}) {
  if (node.type === "pane") result[node.pane_id] = height;
  else {
    assert.equal(node.direction, "down");
    heights(node.first, height * node.ratio, result);
    heights(node.second, height * (1 - node.ratio), result);
  }
  return result;
}

test("one through four children are equal height, explicit and unfocused", async () => {
  const state = fake();
  const opened = [];
  for (let n = 1; n <= 4; n++) {
    opened.push(await state.panes.open("/repo", { ROLE: "builder" }, `child ${n}`));
    assert.equal(state.root.direction, "right");
    assert.equal(state.root.ratio, 0.5);
    assert.equal(state.root.first.pane_id, "parent");
    for (const height of Object.values(heights(state.root.second))) assert.ok(Math.abs(height - 1 / n) < 1e-9);
  }
  assert.deepEqual([...state.panes.owned], opened);
  const splits = state.calls.filter((c) => c.method === "pane.split");
  assert.deepEqual(splits.map((c) => c.params.target_pane_id), ["parent", ...opened.slice(0, 3)]);
  assert.deepEqual(splits.map((c) => c.params.direction), ["right", "down", "down", "down"]);
  assert.deepEqual(splits[0].params.env, { ROLE: "builder" });
  assert.equal(splits[0].params.cwd, "/repo");
  for (let i = 0; i < state.calls.length; i++) {
    if (["pane.split", "layout.set_split_ratio"].includes(state.calls[i].method)) {
      assert.equal(state.calls[i + 1].method, "layout.export");
    }
  }
  await state.panes.close(opened[1]);
  assert.equal(state.root.ratio, 0.5);
  for (const height of Object.values(heights(state.root.second))) assert.ok(Math.abs(height - 1 / 3) < 1e-9);
});

test("pure helper changes only lowest owned down subtree and preserves source", () => {
  const children = split("down", pane("a"), split("down", pane("b"), pane("c"), 0.8), 0.8);
  const root = split("down", pane("manual"), split("right", pane("parent"), children, 0.62), 0.73);
  const before = structuredClone(root);
  assert.deepEqual(balanceOwnedSubtree(root, new Set(["a", "b", "c"])), [
    { path: [true, true, true], ratio: 0.5 }, { path: [true, true], ratio: 1 / 3 },
  ]);
  assert.deepEqual(root, before);
  assert.deepEqual(balanceOwnedSubtree(root, new Set(["a"])), []);
});

test("mixed manual and horizontal owned layouts are left alone", () => {
  const mixed = split("down", pane("a"), split("down", pane("manual"), pane("b"), 0.2), 0.8);
  assert.deepEqual(balanceOwnedSubtree(mixed, new Set(["a", "b"])), []);
  assert.deepEqual(balanceOwnedSubtree(split("right", pane("a"), pane("b"), 0.7), new Set(["a", "b"])), []);
});

test("opening with unrelated panes preserves their and manually adjusted parent ratios", async () => {
  const state = fake(split("down", pane("manual"), pane("parent"), 0.71));
  const a = await state.panes.open("/repo");
  state.root.second.ratio = 0.61;
  await state.panes.open("/repo");
  await state.panes.open("/repo");
  assert.equal(state.root.ratio, 0.71);
  assert.equal(state.root.second.ratio, 0.61);
  // User inserts a foreign pane into the team column. No mixed ratio is changed.
  state.root = transform(state.root, a, (old) => split("down", old, pane("foreign"), 0.23));
  const ratiosBefore = state.calls.filter((c) => c.method === "layout.set_split_ratio").length;
  await state.panes.open("/repo");
  assert.equal(state.calls.filter((c) => c.method === "layout.set_split_ratio").length, ratiosBefore);
});

test("mutations serialize, and failed close retains ownership without poisoning queue", async () => {
  const state = fake();
  const children = await Promise.all(Array.from({ length: 4 }, () => state.panes.open("/repo")));
  assert.equal(state.peak, 1);
  state.failClose = Object.assign(new Error("denied"), { code: "permission_denied" });
  await assert.rejects(state.panes.close(children[0]), /denied/);
  assert.ok(state.panes.owned.has(children[0]));
  state.failClose = null;
  await Promise.all(children.map((id) => state.panes.close(id)));
  assert.equal(state.peak, 1);
  assert.equal(state.panes.owned.size, 0);
  assert.deepEqual(state.root, pane("parent"));
});

test("interleaved close and open use fresh topology", async () => {
  const state = fake();
  const a = await state.panes.open("/repo");
  const [, b] = await Promise.all([state.panes.close(a), state.panes.open("/repo")]);
  const latest = state.calls.filter((c) => c.method === "pane.split").at(-1);
  assert.equal(latest.params.target_pane_id, "parent");
  assert.equal(latest.params.direction, "right");
  assert.deepEqual([...state.panes.owned], [b]);
  assert.equal(state.peak, 1);
});

test("foreign close is a no-op; manually closed children are forgotten", async () => {
  const state = fake();
  await state.panes.close("parent");
  assert.equal(await state.panes.exists("parent"), false);
  assert.equal(state.calls.length, 0);
  const a = await state.panes.open("/repo");
  assert.equal(await state.panes.exists(a), true);
  state.root = pane("parent");
  assert.equal(await state.panes.exists(a), false);
  assert.equal(state.panes.owned.has(a), false);
  const b = await state.panes.open("/repo");
  state.root = pane("parent");
  await state.panes.close(b);
  assert.equal(state.panes.owned.size, 0);
});

test("exists propagates transport errors rather than discarding ownership", async () => {
  const panes = new HerdrPanes({ parentPaneId: "parent", call: async () => { throw new Error("offline"); } });
  panes.owned.add("a");
  await assert.rejects(panes.exists("a"), /offline/);
  assert.ok(panes.owned.has("a"));
});

test("failed post-split setup closes owned pane and exports new topology", async () => {
  const state = fake();
  const call = state.call;
  state.panes.call = async (method, params) => {
    if (method === "pane.rename") throw new Error("rename failed");
    return call(method, params);
  };
  await assert.rejects(state.panes.open("/repo", {}, "label"), /rename failed/);
  assert.equal(state.panes.owned.size, 0);
  assert.deepEqual(state.root, pane("parent"));
  const closed = state.calls.findIndex((c) => c.method === "pane.close");
  assert.equal(state.calls[closed + 1].method, "layout.export");
});

test("failed open cleanup retains ownership for later retry", async () => {
  const state = fake();
  const call = state.call;
  state.failClose = new Error("close failed");
  state.panes.call = async (method, params) => {
    if (method === "pane.rename") throw new Error("rename failed");
    return call(method, params);
  };
  await assert.rejects(state.panes.open("/repo", {}, "label"), (error) => {
    assert.ok(error instanceof AggregateError);
    assert.equal(error.paneId, 'w1:p1');
    assert.equal(error.cleanupError, 'close failed');
    assert.deepEqual(error.errors.map((e) => e.message), ["rename failed", "close failed"]);
    return true;
  });
  assert.equal(state.panes.owned.size, 1);
  state.failClose = null;
  await state.panes.close([...state.panes.owned][0]);
  assert.equal(state.panes.owned.size, 0);
});

async function server(t, respond) {
  const directory = await mkdtemp(join(tmpdir(), "pi-herdr-test-"));
  const path = join(directory, "socket");
  const sockets = new Set();
  const instance = net.createServer((socket) => {
    sockets.add(socket);
    socket.on("error", () => {});
    socket.on("close", () => sockets.delete(socket));
    let text = "";
    socket.on("data", (chunk) => {
      text += chunk.toString();
      if (text.includes("\n")) {
        const request = JSON.parse(text.trim());
        respond(socket, request, text);
      }
    });
  });
  await new Promise((resolve, reject) => { instance.once("error", reject); instance.listen(path, resolve); });
  t.after(async () => {
    for (const socket of sockets) socket.destroy();
    await new Promise((resolve) => instance.close(resolve));
    await rm(directory, { recursive: true, force: true });
  });
  return path;
}

test("socket newline request and fragmented UTF-8 response", async (t) => {
  const path = await server(t, (socket, request, text) => {
    assert.equal(text.at(-1), "\n");
    assert.equal(request.method, "pane.get");
    assert.deepEqual(request.params, { pane_id: "w1:p1" });
    assert.equal(typeof request.id, "string");
    const response = Buffer.from(JSON.stringify({ id: request.id, result: { label: "café" } }) + "\n");
    const cut = response.indexOf(Buffer.from("é")) + 1;
    socket.write(response.subarray(0, cut));
    setTimeout(() => socket.end(response.subarray(cut)), 5);
  });
  assert.deepEqual(await socketCall(path, "pane.get", { pane_id: "w1:p1" }), { label: "café" });
});

for (const [name, respond, pattern, options] of [
  ["timeout", () => {}, /timed out/, { timeoutMs: 25 }],
  ["malformed JSON", (s) => s.end("oops\n"), /JSON|Unexpected/],
  ["wrong id", (s) => s.end('{"id":"other","result":{}}\n'), /ID mismatch/],
  ["missing result", (s, r) => s.end(JSON.stringify({ id: r.id }) + "\n"), /Malformed/],
  ["both result and error", (s, r) => s.end(JSON.stringify({ id: r.id, result: {}, error: {} }) + "\n"), /Malformed/],
  ["malformed error", (s, r) => s.end(JSON.stringify({ id: r.id, error: "bad" }) + "\n"), /Malformed/],
  ["truncated response", (s) => s.end('{"id":'), /complete response/],
  ["oversized response", (s) => s.write("x".repeat(100)), /size bound/, { maxResponseBytes: 32 }],
]) {
  test(`socket rejects ${name}`, async (t) => {
    const path = await server(t, respond);
    await assert.rejects(socketCall(path, "test", {}, options), pattern);
  });
}

test("socket preserves server error codes", async (t) => {
  const path = await server(t, (socket, request) => socket.end(JSON.stringify({
    id: request.id, error: { code: "pane_not_found", message: "pane not found" },
  }) + "\n"));
  await assert.rejects(socketCall(path, "pane.get", {}), { code: "pane_not_found", message: "pane not found" });
});

test("invalid bounds reject before connection", async () => {
  for (const options of [{ timeoutMs: 0 }, { timeoutMs: Infinity }, { maxResponseBytes: -1 }]) {
    await assert.rejects(socketCall("/unused", "test", {}, options), /Invalid socket/);
  }
});
