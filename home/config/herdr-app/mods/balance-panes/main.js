import { View, div } from "gpui-kit";
import { spaces, panes, machines, exec, run, props, closeOverlay, notify } from "herdr";
import { activeTarget, sameTarget, balancePlan, applyPlan, decodeReply, qualify } from "./model.js";

export default class BalancePanes extends View {
  init(_props, cx) {
    this.closed = false;
    const overlay = props()?.overlay;
    // The mod checker and layout previews must never change the live document.
    if (!overlay) return;
    const target = activeTarget(spaces());
    cx.spawn(async () => {
      try {
        if (!target) return;
        const machine = machines().find(m => (m.name || null) === target.on);
        if (!machine || machine.layout !== "ready") throw new Error("That machine's layout is unavailable. Reconnect and try again.");
        const current = () => !this.closed && sameTarget(target, activeTarget(spaces()));
        const message = JSON.stringify({ jsonrpc: "2.0", id: "balance-panes", method: "doc.get", params: { name: "herdr-app", on: target.on } });
        // Read only. JSON is one positional argument, never interpolated into shell source.
        const result = await exec("sh", ["-c", 'printf "%s\\n" "$1" | hc connect', "balance-panes", message]);
        const snapshot = decodeReply(result);
        if (!current()) return;
        const plan = balancePlan(snapshot.value, target);
        const live = panes().filter(p => p.tab === qualify(target.tab, target.on)).map(p => p.pane);
        applyPlan(plan, live, update => run("layout.resize", update));
      } catch (error) {
        if (!this.closed) notify({ title: "Could not balance panes", body: String(error).replace(/^Error: /, ""), level: "error", timeout: 7000 });
      } finally {
        if (!this.closed) closeOverlay(overlay);
      }
    });
  }

  close() { this.closed = true; }
  render() { return div().id("balance-panes-worker").w(0).h(0); }
}
