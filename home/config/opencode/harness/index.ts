import { Plugin } from "@opencode/plugin";
import { readFileSync } from "node:fs";
import { homedir } from "node:os";
import { join } from "node:path";
import { Workflow } from "./rpc.ts";
import { setupGoals } from "./goals.ts";
import { setupSkills } from "./skills.ts";
import { setupBus } from "./bus.ts";

export default Plugin.define({
  id: "dotfiles.workflow",
  async setup(ctx) {
    await setupSkills(ctx);
    const stopGoals = await setupGoals(ctx, Workflow);
    const stopBus = ctx.options.busEnabled ? await setupBus(ctx) : () => {};
    let orchestrator = "";
    try { orchestrator = readFileSync(join(homedir(), ".config/opencode/ORCHESTRATOR.md"), "utf8"); } catch { /* optional */ }
    await ctx.session.hook("context", async event => {
      const session = await ctx.session.get({ sessionID: event.sessionID });
      if (!session.parentID && orchestrator) event.system.push({ type: "text", text: orchestrator });
    });
    return () => { stopGoals(); stopBus(); };
  },
});
