import net from "node:net";
import { randomUUID } from "node:crypto";

/** One bounded, newline-framed public herdr socket request. */
export function socketCall(socketPath, method, params, options = {}) {
  const { timeoutMs = 5000, maxResponseBytes = 1024 * 1024 } = options;
  if (!Number.isInteger(timeoutMs) || timeoutMs <= 0 || timeoutMs > 2147483647 ||
      !Number.isSafeInteger(maxResponseBytes) || maxResponseBytes <= 0) {
    return Promise.reject(new Error("Invalid socket timeout or response bound"));
  }
  return new Promise((resolve, reject) => {
    const id = randomUUID();
    const request = JSON.stringify({ id, method, params }) + "\n";
    const socket = net.createConnection(socketPath);
    let buffer = Buffer.alloc(0);
    let settled = false;
    const finish = (error, result) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      socket.destroy();
      if (error) reject(error);
      else resolve(result);
    };
    const timer = setTimeout(() => finish(new Error(`Herdr ${method} timed out`)), timeoutMs);
    socket.on("connect", () => socket.write(request));
    socket.on("error", (error) => finish(error));
    socket.on("end", () => finish(new Error("Herdr closed before a complete response")));
    socket.on("close", () => finish(new Error("Herdr socket closed before response")));
    socket.on("data", (chunk) => {
      if (buffer.length + chunk.length > maxResponseBytes) {
        finish(new Error("Herdr response exceeds size bound"));
        return;
      }
      buffer = Buffer.concat([buffer, chunk]);
      const newline = buffer.indexOf(10);
      if (newline < 0) return;
      try {
        const response = JSON.parse(buffer.subarray(0, newline).toString("utf8"));
        if (!response || response.id !== id) throw new Error("Herdr response ID mismatch");
        const hasResult = Object.hasOwn(response, "result");
        const hasError = Object.hasOwn(response, "error");
        if (hasResult === hasError) throw new Error("Malformed Herdr response envelope");
        if (hasError) {
          if (typeof response.error?.code !== "string" || typeof response.error?.message !== "string") {
            throw new Error("Malformed Herdr error response");
          }
          throw Object.assign(new Error(response.error.message), { code: response.error.code });
        }
        finish(null, response.result);
      } catch (error) {
        finish(error);
      }
    });
  });
}

function leaves(node) {
  if (node?.type === "pane" && typeof node.pane_id === "string") return [node.pane_id];
  if (node?.type === "split") return [...leaves(node.first), ...leaves(node.second)];
  throw new Error("Malformed Herdr layout tree");
}

/**
 * Ratio edits for the lowest subtree containing all locally present owned panes.
 * Mixed ownership or manually introduced right splits make that subtree ineligible.
 * Paths follow herdr's Vec<bool>: false = first, true = second. Input is untouched.
 */
export function balanceOwnedSubtree(root, owned) {
  const present = leaves(root).filter((id) => owned.has(id));
  if (present.length < 2) return [];
  let node = root;
  const path = [];
  while (node.type === "split") {
    const first = new Set(leaves(node.first));
    const second = new Set(leaves(node.second));
    if (present.every((id) => first.has(id))) { node = node.first; path.push(false); }
    else if (present.every((id) => second.has(id))) { node = node.second; path.push(true); }
    else break;
  }
  const eligible = (tree) => tree.type === "pane" ? owned.has(tree.pane_id) :
    tree.direction === "down" && eligible(tree.first) && eligible(tree.second);
  if (!eligible(node)) return [];
  const edits = [];
  const visit = (tree, at) => {
    if (tree.type === "pane") return 1;
    const first = visit(tree.first, [...at, false]);
    const second = visit(tree.second, [...at, true]);
    const ratio = first / (first + second);
    if (Math.abs(tree.ratio - ratio) > 1e-6) edits.push({ path: at, ratio });
    return first + second;
  };
  visit(node, path);
  return edits;
}

export class HerdrPanes {
  owned = new Set();
  #queue = Promise.resolve();

  constructor({ socketPath, parentPaneId, call }) {
    if (typeof parentPaneId !== "string" || !parentPaneId) throw new Error("Explicit parentPaneId required");
    this.parentPaneId = parentPaneId;
    this.call = call ?? ((method, params) => socketCall(socketPath, method, params));
  }

  #serialize(operation) {
    const result = this.#queue.then(operation);
    this.#queue = result.catch(() => {});
    return result;
  }

  async #export() {
    const { layout } = await this.call("layout.export", { pane_id: this.parentPaneId });
    leaves(layout?.root);
    if (typeof layout.tab_id !== "string") throw new Error("Malformed Herdr layout tab ID");
    return layout;
  }

  async #balance() {
    // Recompute paths after each edit, never reuse a cached topology.
    let layout = await this.#export();
    for (let count = 0; count < 32; count++) {
      const [edit] = balanceOwnedSubtree(layout.root, this.owned);
      if (!edit) return;
      await this.call("layout.set_split_ratio", { tab_id: layout.tab_id, ...edit });
      layout = await this.#export();
    }
    throw new Error("Herdr layout did not settle while balancing");
  }

  open(cwd, env = {}, label) {
    return this.#serialize(async () => {
      const layout = await this.#export();
      const target = leaves(layout.root).filter((id) => this.owned.has(id)).at(-1);
      const result = await this.call("pane.split", {
        target_pane_id: target ?? this.parentPaneId,
        direction: target ? "down" : "right", ratio: 0.5,
        focus: false, cwd, env,
      });
      const paneId = result?.pane?.pane_id;
      if (typeof paneId !== "string" || !paneId || paneId === this.parentPaneId || this.owned.has(paneId)) {
        throw new Error("Malformed Herdr split pane ID");
      }
      this.owned.add(paneId);
      try {
        await this.#balance();
        if (label !== undefined) await this.call("pane.rename", { pane_id: paneId, label });
        return paneId;
      } catch (error) {
        try { await this.#close(paneId); }
        catch (cleanupError) {
          throw Object.assign(new AggregateError([error, cleanupError], `Herdr open failed; cleanup failed for ${paneId}`), {
            paneId, cleanupError: String(cleanupError?.message ?? cleanupError),
          });
        }
        throw error;
      }
    });
  }

  close(paneId) { return this.#serialize(() => this.#close(paneId)); }

  async #close(paneId) {
    if (!this.owned.has(paneId)) return;
    try { await this.call("pane.close", { pane_id: paneId }); }
    catch (error) { if (error.code !== "pane_not_found") throw error; }
    this.owned.delete(paneId);
    await this.#balance();
  }

  exists(paneId) {
    return this.#serialize(async () => {
      if (!this.owned.has(paneId)) return false;
      try {
        await this.call("pane.get", { pane_id: paneId });
        return true;
      } catch (error) {
        if (error.code !== "pane_not_found") throw error;
        this.owned.delete(paneId);
        return false;
      }
    });
  }
}
