import { execFile } from "node:child_process";
import { promisify } from "node:util";

const execute = promisify(execFile);
const clean = (value: unknown) => typeof value === "string"
  ? value.replace(/[\u0000-\u001f\u007f-\u009f]/g, "").trim() : "";

export function modelLabel(model: { modelID?: string; variant?: string } | undefined) {
  if (!model?.modelID) return "";
  return Array.from(`opencode · ${clean(model.variant) || "default"} · ${clean(model.modelID)}`).slice(0, 160).join("");
}

/** Presentation labels only. OpenCode's native OSC reporting remains the status authority. */
export function createDesktopMetadata(
  env: Record<string, string | undefined>,
  send = async (label: string) => {
    await execute("hc", ["label", env.HC_TERMINAL_ID!, `user.agent.display=${label}`], {
      env, timeout: 2000, maxBuffer: 1024 * 1024,
    });
  },
) {
  let target = "";
  let sent: string | undefined;
  let busy = false;
  let stopped = false;
  async function drain() {
    if (!env.HC_TERMINAL_ID || busy || target === sent) return;
    busy = true;
    const label = target;
    try { await send(label); sent = label; } catch { /* Herdr must not interrupt OpenCode. */ }
    finally {
      busy = false;
      if (target !== label) void drain();
    }
  }
  return {
    sync(model: { modelID?: string; variant?: string } | undefined) {
      if (stopped) return;
      target = modelLabel(model);
      void drain();
    },
    stop() {
      stopped = true;
      target = "";
      void drain();
    },
  };
}
