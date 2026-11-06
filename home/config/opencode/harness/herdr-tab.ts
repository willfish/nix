import { createConnection } from "node:net";
import { randomUUID } from "node:crypto";
import { execFile } from "node:child_process";
import { promisify } from "node:util";

type Environment = Record<string, string | undefined>;
type Call = (method: string, params: Record<string, string>) => Promise<unknown>;

export const HERDR_LABEL_ENTRY = "agent-bus-herdr-label";
export type TabOwnership = { socket: string; pane: string; tab: string; label: string };
type Pane = { pane_id?: string; tab_id?: string; workspace_id?: string };
const execute = promisify(execFile);
type AppCall = (args: string[]) => Promise<string>;

export function herdrTarget(env: Environment): string | undefined {
  if (env.HC_TERMINAL_ID) return `herdr-app:${env.HERDR_APP_HOME ?? "default"}:${env.HC_TERMINAL_ID}`;
  if (env.HERDR_ENV === "1" && env.HERDR_SOCKET_PATH && env.HERDR_PANE_ID)
    return `${env.HERDR_SOCKET_PATH}:${env.HERDR_PANE_ID}`;
}

async function nameDesktopTab(label: string, env: Environment, options: {
  previous?: TabOwnership;
  isCurrent?: () => boolean;
  appCall?: AppCall;
}): Promise<TabOwnership | void> {
  const current = options.isCurrent ?? (() => true);
  const call = options.appCall ?? (async args => (await execute("herdr-app", args, {
    env, timeout: 2000, maxBuffer: 1024 * 1024,
  })).stdout);
  const locate = async () => {
    const result = JSON.parse(await call(["ls"]));
    for (const space of result.spaces ?? []) {
      for (const tab of space.tabs ?? []) {
        // The first terminal owns the tab, never whichever pane happens to have focus.
        if (tab.terminals?.[0]?.terminal === env.HC_TERMINAL_ID) return tab;
      }
    }
  };
  const tab = await locate();
  if (!tab || !current() || typeof tab.name !== "string") return;
  const socket = `herdr-app:${env.HERDR_APP_HOME ?? "default"}`;
  const previous = options.previous;
  const owned = previous?.socket === socket && previous.pane === env.HC_TERMINAL_ID &&
    previous.tab === tab.id && previous.label === tab.name;
  if (!/^[0-9]+$/.test(tab.name) && !owned) return;
  const next = `${tab.name.match(/^[0-9]+/)?.[0]} ${Array.from(label).slice(0, 48).join("")}`;
  const ownership = { socket, pane: env.HC_TERMINAL_ID!, tab: tab.id, label: next };
  if (next === tab.name) return ownership;
  const latest = await locate();
  if (latest?.id !== tab.id || latest.name !== tab.name || !current()) return;
  await call(["do", "tab.rename", JSON.stringify({ id: tab.id, name: next })]);
  return current() ? ownership : undefined;
}

const FALLBACK_TITLE = /^(New session|Child session)( - \d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z)?$/;

export function sessionTabTitle(value: unknown): string | undefined {
  if (typeof value !== "string") return;
  const label = value.trim();
  if (!label || FALLBACK_TITLE.test(label) || /[\u0000-\u001f\u007f-\u009f\u2028\u2029<>]/.test(label)) return;
  if (Array.from(label).some(char => /^[\ud800-\udfff]$/.test(char))) return;
  return Array.from(label).slice(0, 200).join("");
}

/** Only the oldest pane owns a shared tab. Manual names always win over automation. */
export async function nameUnlabelledTab(
  label: string,
  env: Environment,
  call: Call = (method, params) => herdrCall(env.HERDR_SOCKET_PATH!, method, params),
  options: {
    previous?: TabOwnership;
    isCurrent?: () => boolean;
    appCall?: AppCall;
  } = {},
): Promise<TabOwnership | void> {
  const current = options.isCurrent ?? (() => true);
  if (!label || !current()) return;
  if (/[\u0000-\u001f\u007f-\u009f<>]/.test(label)) return;
  // Desktop shells can inherit stale legacy variables. The core terminal wins.
  if (env.HC_TERMINAL_ID) return nameDesktopTab(label, env, options);
  if (env.HERDR_ENV !== "1" || !env.HERDR_SOCKET_PATH || !env.HERDR_PANE_ID) return;
  const paneResult = (await call("pane.get", { pane_id: env.HERDR_PANE_ID })) as { pane?: Pane };
  const pane = paneResult?.pane;
  if (!pane?.tab_id || !pane.pane_id || !pane.workspace_id || !current()) return;
  // Resolve the tab from the caller pane, never from UI focus or the inherited tab ID.
  const peers = (await call("pane.list", { workspace_id: pane.workspace_id })) as { panes?: Pane[] };
  const members = peers?.panes
    ?.filter(p => p.tab_id === pane.tab_id && typeof p.pane_id === "string")
    .sort((a, b) => a.pane_id!.localeCompare(b.pane_id!, "en", { numeric: true }));
  if (members?.[0]?.pane_id !== pane.pane_id || !current()) return;
  const tabResult = (await call("tab.get", { tab_id: pane.tab_id })) as { tab?: { label?: unknown } };
  const shown = tabResult?.tab?.label;
  const previous = options.previous;
  const owned =
    previous?.socket === env.HERDR_SOCKET_PATH &&
    previous.pane === pane.pane_id &&
    previous.tab === pane.tab_id &&
    previous.label === shown;
  if (typeof shown !== "string" || (!/^[0-9]+$/.test(shown) && !owned) || !current()) return;
  const prefix = shown.match(/^[0-9]+/)?.[0];
  if (!prefix) return;
  const next = `${prefix} ${Array.from(label).slice(0, 48).join("")}`;
  const ownership = { socket: env.HERDR_SOCKET_PATH, pane: pane.pane_id, tab: pane.tab_id, label: next };
  if (next === shown) return ownership;
  // Herdr has no conditional rename. Recheck immediately before the write and fence stale callbacks.
  const latestPane = (await call("pane.get", { pane_id: pane.pane_id })) as { pane?: Pane };
  const latestTab = (await call("tab.get", { tab_id: pane.tab_id })) as { tab?: { label?: unknown } };
  if (latestPane?.pane?.tab_id !== pane.tab_id || latestTab?.tab?.label !== shown || !current()) return;
  await call("tab.rename", { tab_id: pane.tab_id, label: next });
  return current() ? ownership : undefined;
}

export function createTabSynchronizer(
  options: {
    env?: Environment;
    nameTab?: typeof nameUnlabelledTab;
    initialOwnership?: TabOwnership;
    onOwnership?: (ownership: TabOwnership) => void;
  } = {},
) {
  const env = options.env ?? process.env;
  const nameTab = options.nameTab ?? nameUnlabelledTab;
  let ownership = options.initialOwnership;
  let target: { sessionID: string; label: string } | undefined;
  let busy = false;
  let dirty = false;
  let stopped = false;

  function drain() {
    if (stopped || busy || !target) return;
    busy = true;
    dirty = false;
    const captured = target;
    const isCurrent = () => !stopped && target?.sessionID === captured.sessionID && target?.label === captured.label;
    void nameTab(captured.label, env, undefined, { previous: ownership, isCurrent })
      .then(next => {
        if (!next || !isCurrent()) return;
        if (JSON.stringify(next) !== JSON.stringify(ownership)) {
          ownership = next;
          options.onOwnership?.(next);
        }
      })
      .catch(() => {})
      .finally(() => {
        busy = false;
        if (dirty && !stopped) drain();
      });
  }

  return {
    sync(sessionID: string | undefined, rawTitle: unknown) {
      if (stopped) return;
      const label = sessionTabTitle(rawTitle);
      if (!sessionID || !label) return;
      target = { sessionID, label };
      if (busy) {
        dirty = true;
        return;
      }
      drain();
    },
    stop() {
      stopped = true;
    },
  };
}

function herdrCall(socketPath: string, method: string, params: Record<string, string>): Promise<unknown> {
  return new Promise((resolve, reject) => {
    const id = randomUUID();
    const socket = createConnection(socketPath);
    let data = "";
    let settled = false;
    const finish = (error?: Error, result?: unknown) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      socket.destroy();
      if (error) reject(error);
      else resolve(result);
    };
    const timer = setTimeout(() => finish(new Error("Herdr request timed out")), 500);
    timer.unref?.();
    socket.on("connect", () => socket.write(`${JSON.stringify({ id, method, params })}\n`));
    socket.on("error", error => finish(error));
    socket.on("end", () => finish(new Error("Herdr closed before response")));
    socket.on("data", (chunk: Buffer) => {
      data += chunk.toString("utf8");
      if (Buffer.byteLength(data) > 65536) {
        finish(new Error("Herdr response too large"));
        return;
      }
      const newline = data.indexOf("\n");
      if (newline < 0) return;
      try {
        const response = JSON.parse(data.slice(0, newline));
        if (response.id !== id || !Object.hasOwn(response, "result") || Object.hasOwn(response, "error"))
          throw new Error("Invalid Herdr response");
        finish(undefined, response.result);
      } catch (error) {
        finish(error as Error);
      }
    });
  });
}
