import { Plugin } from "@opencode/plugin/tui";
import { Workflow } from "./rpc.ts";
import { parseGoalCommand } from "./goal-source/index.ts";

export default Plugin.define({
  id: "dotfiles.workflow.tui",
  setup(context) {
    const [goals, updateGoals] = context.storage.memory("goals", { initial: {} });
    const loaded = new Set();
    const receive = state => updateGoals(draft => { draft[state.sessionID] = state; });
    const stop = context.client.rpc(Workflow).events.on("goal", event => receive(event.data));
    const current = () => {
      const route = context.ui.router.current();
      return route.type === "session" ? route.sessionID : undefined;
    };
    async function invoke(sessionID, command, replace = false) {
      const response = await context.client.rpc(Workflow).goal({ sessionID, command, replace });
      const state = response.data ?? response;
      receive(state);
      return state;
    }
    context.keymap.layer(() => ({
      mode: "global", priority: 20,
      commands: [{
        id: "dotfiles.goal", title: "Manage persistent goal", group: "Workflow", palette: true,
        slash: { name: "goal", arguments: true },
        async run(input) {
          try {
            let sessionID = current();
            if (!sessionID) {
              const created = await context.client.session.create({ location: context.location ?? context.data.location.default() });
              const session = created.data ?? created;
              sessionID = session.id;
              context.ui.router.navigate({ type: "session", sessionID });
            }
            let command = input?.trim() || "status";
            let { action, rest } = parseGoalCommand(command);
            const before = await invoke(sessionID, "status");
            let replace = false;
            if (action === "edit" && !rest) {
              const objective = await context.ui.dialog.prompt({ title: "Edit goal", value: before.goal?.objective ?? "" });
              if (objective == null) return;
              command = `edit ${objective}`;
            }
            if (action === "set" && before.goal && before.goal.status !== "complete") {
              replace = await context.ui.dialog.confirm({ title: "Replace unfinished goal?", message: rest });
              if (!replace) return;
            }
            const state = await invoke(sessionID, command, replace);
            if (["status", "show", "help"].includes(action)) await context.ui.dialog.alert({ title: "Persistent goal", message: state.text });
            else context.ui.toast.show({ message: state.goal ? `Goal ${state.goal.status}` : "Goal cleared", variant: "success" });
          } catch (error) {
            context.ui.toast.show({ message: String(error.message ?? error), variant: "error" });
          }
        },
      }],
      bindings: [],
    }));
    context.ui.slot({ append: "prompt.footer.status", render: ({ sessionID }) => {
      if (sessionID && !loaded.has(sessionID)) {
        loaded.add(sessionID);
        void invoke(sessionID, "status").catch(() => loaded.delete(sessionID));
      }
      const goal = goals[sessionID]?.goal;
      return <text fg={context.theme.text.muted}>{goal ? `Goal ${goal.status}${goal.interrupted ? " (interrupted)" : ""} · ${goal.tokensUsed}/${goal.tokenBudget ?? "∞"} tokens` : ""}</text>;
    } });
    return () => stop();
  },
});
