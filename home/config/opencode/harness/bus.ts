import legacyBridge from "./bus-source.ts";

// Reuse the existing Switchboard client and tool bodies. Each V2 session owns
// its own runtime, including child sessions hosted by the same server process.
export async function setupBus(ctx) {
  const bridges = new Map();
  const client = { session: { promptAsync: ({ path, body }) => ctx.session.prompt({ sessionID: path.id,
    text: body.parts.filter(part => part.type === "text").map(part => part.text).join("\n") }) } };
  const make = () => legacyBridge({ directory: ctx.location.directory, client });
  const definitions = await make();
  async function bridgeFor(sessionID) {
    if (!bridges.has(sessionID)) bridges.set(sessionID, make().then(async bridge => {
      await bridge.event({ event: { type: "session.created", properties: { info: { id: sessionID } } } });
      const session = await ctx.session.get({ sessionID });
      if (session.model) await bridge.event({ event: { type: "session.next.model.switched", properties: {
        sessionID, model: { providerID: session.model.providerID, id: session.model.id },
      } } });
      return bridge;
    }));
    const bridge = await bridges.get(sessionID);
    return bridge;
  }
  await ctx.tool.transform(editor => {
    for (const [name, definition] of Object.entries(definitions.tool)) editor.add({
      name, description: definition.description, options: { namespace: "coordination" },
      input: { type: "object", properties: definition.args, required: Object.keys(definition.args), additionalProperties: false },
      async execute(input, context) {
        await ctx.permission.assert({ action: name, resources: ["*"], sessionID: context.sessionID, agent: context.agent });
        const bridge = await bridgeFor(context.sessionID);
        const result = await definitionFor(bridge, name).execute(input, { abort: context.signal });
        return { content: result.output, metadata: result.metadata };
      },
    });
  });
  await ctx.session.hook("prompt", async event => {
    const bridge = await bridgeFor(event.sessionID);
    const message = { parts: [{ type: "text", text: event.prompt.text }] };
    await bridge["chat.message"]({ sessionID: event.sessionID }, message);
    event.prompt.text = message.parts.map(part => part.text).join("\n\n");
  });
  const controller = new AbortController();
  const types = ["session.execution.started", "session.execution.succeeded", "session.execution.failed", "session.execution.interrupted", "session.model.selected", "session.deleted"];
  const events = (async () => {
    for await (const event of ctx.event.subscribe({ types, signal: controller.signal })) {
      const id = event.data.sessionID;
      if (!bridges.has(id)) continue;
      const bridge = await bridges.get(id);
      if (event.type === "session.deleted") {
        await bridge.event({ event: { type: "session.deleted", properties: { sessionID: id } } });
        bridges.delete(id);
      } else if (event.type === "session.model.selected") {
        await bridge.event({ event: { type: "session.next.model.switched", properties: {
          sessionID: id, model: { providerID: event.data.model.providerID, id: event.data.model.id },
        } } });
      } else {
        await bridge.event({ event: { type: "session.status", properties: { sessionID: id,
          status: { type: event.type === "session.execution.started" ? "busy" : "idle" },
        } } });
      }
    }
  })();
  events.catch(error => { if (!controller.signal.aborted) console.error("Switchboard event subscription failed", error); });
  return () => {
    controller.abort();
    for (const [id, bridge] of bridges) void bridge.then(value => value.event({ event: { type: "session.deleted", properties: { sessionID: id } } }));
    bridges.clear();
  };
}

const definitionFor = (bridge, name) => bridge.tool[name];
