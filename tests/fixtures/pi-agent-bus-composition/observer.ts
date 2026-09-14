import { appendFileSync } from "node:fs";

export default function (pi) {
  const record = (value) => appendFileSync(process.env.COMPOSITION_EVENTS!, JSON.stringify(value) + "\n");
  pi.on("session_start", (_event, ctx) => record({ type: "started", mode: ctx.mode }));
  pi.on("input", (event, ctx) => {
    if (event.text === "composition-headless") {
      record({ type: "handled", mode: ctx.mode });
      return { action: "handled" };
    }
  });
  pi.registerCommand("composition-ping", {
    handler: async (_args, ctx) => {
      record({ type: "pong", mode: ctx.mode });
      ctx.ui.notify("composition pong", "info");
    },
  });
}
