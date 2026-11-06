import { Plugin } from "@opencode/plugin";
import { Schema } from "effect";
import { readFileSync } from "node:fs";
import { homedir } from "node:os";
import { join } from "node:path";
import { setupSkills } from "./skills.ts";
import { setupBus } from "./bus.ts";
import { setupSpeak } from "./speak.ts";
import { setupAutoSpeak, VOICE_RPC_ID } from "./auto-speak.ts";
import { createTabSynchronizer, sessionTabTitle } from "./herdr-tab.ts";

function setupHerdrServer(ctx) {
  if (
    process.argv.includes("--service") ||
    process.env.HC_TERMINAL_ID ||
    process.env.HERDR_ENV !== "1" ||
    !process.env.HERDR_SOCKET_PATH ||
    !process.env.HERDR_PANE_ID
  ) {
    return () => {};
  }
  const tab = createTabSynchronizer({ env: process.env });
  const controller = new AbortController();
  const events = (async () => {
    for await (const event of ctx.event.subscribe({
      types: ["session.created", "session.renamed"],
      signal: controller.signal,
    })) {
      const id = event.data.sessionID;
      const title = sessionTabTitle(event.data.title);
      if (!id || !title) continue;
      if (event.type === "session.created" && event.data.parentID) continue;
      const session = await ctx.session.get({ sessionID: id }).catch(() => undefined);
      if (!session || session.parentID) continue;
      tab.sync(id, title);
    }
  })();
  events.catch(() => {});
  return () => {
    controller.abort();
    tab.stop();
  };
}

export default Plugin.define({
  id: "dotfiles.workflow",
  async setup(ctx) {
    await setupSkills(ctx);
    await setupSpeak(ctx);
    const voice = setupAutoSpeak(ctx);
    await ctx.rpc.register({
      id: VOICE_RPC_ID,
      methods: { control: {
        input: Schema.Struct({ sessionID: Schema.String, action: Schema.Literals(["on", "off", "status"]) }),
        output: Schema.Struct({ enabled: Schema.Boolean, error: Schema.String }),
      } },
      events: {},
    }, { control: async ({ sessionID, action }) => voice.control(sessionID, action) });
    const stopBus = ctx.options.busEnabled ? await setupBus(ctx) : () => {};
    const stopHerdr = setupHerdrServer(ctx);
    let orchestrator = "";
    try { orchestrator = readFileSync(join(homedir(), ".config/opencode/ORCHESTRATOR.md"), "utf8"); } catch { /* optional */ }
    await ctx.session.hook("context", async event => {
      const session = await ctx.session.get({ sessionID: event.sessionID });
      if (!session.parentID && orchestrator) event.system.push({ type: "text", text: orchestrator });
    });
    return () => {
      voice.stop();
      stopHerdr();
      stopBus();
    };
  },
});
