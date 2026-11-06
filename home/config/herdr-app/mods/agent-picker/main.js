import { View, div } from "gpui-kit";
import { v_flex, h_flex, InputState, Input } from "gpui-base";
import { agents, terminals, spaces, theme, run, props, closeOverlay } from "herdr";
import { agentRows } from "./model.js";

const ROW = 56;
const HEIGHT = ROW * 6;

export default class AgentPicker extends View {
  init(_props, cx) {
    this.query = "";
    this.selected = null;
    this.scroll = 0;
    this.error = "";
    this.handle = cx.focus_handle();
    this.input = InputState.new({ placeholder: "Search agent, model, title, space or path…" });
    this.input.on("change", (_e, cx) => {
      this.query = this.input.value(); this.selected = null; this.scroll = 0; this.error = ""; cx.notify();
    });
    this.input.on("submit", (_e, cx) => this.go(this.selection(this.rows()), cx));
    this.input.focus();
  }

  rows() { return agentRows(agents(), terminals(), spaces(), this.query); }

  selection(rows) {
    return rows.find(r => r.key === this.selected) || rows[0];
  }

  dismiss() { closeOverlay(props().overlay); }

  go(row, cx) {
    if (!row) return;
    if (!agents().some(a => a.address === row.key)) {
      this.error = "That agent is no longer available. Choose another result."; cx.notify(); return;
    }
    try {
      run("agent.focus", { terminal: row.key });
      this.dismiss();
    } catch (error) { this.error = String(error); cx.notify(); }
  }

  render() {
    const t = theme();
    const rows = this.rows();
    const selected = this.selection(rows);
    this.selected = selected?.key ?? null;
    const at = Math.max(0, rows.indexOf(selected));
    const height = Math.min(HEIGHT, Math.max(ROW, rows.length * ROW));
    this.scroll = Math.max(0, Math.min(this.scroll, rows.length * ROW - height));
    if (at * ROW < this.scroll) this.scroll = at * ROW;
    if ((at + 1) * ROW > this.scroll + height) this.scroll = (at + 1) * ROW - height;

    return v_flex().id("agent-picker").role("dialog").accessibility_label("Go to agent").track_focus(this.handle).w_full().min_h(0).p(18).gap(10).bg(t.popover).text_size(13)
      .on_key_down((e, cx) => {
        if (e.key === "escape") { cx.stop_propagation(); this.dismiss(); return; }
        if (e.key !== "up" && e.key !== "down") return;
        cx.stop_propagation();
        if (rows.length) this.selected = rows[(at + (e.key === "down" ? 1 : rows.length - 1)) % rows.length].key;
        cx.notify();
      })
      .child(h_flex().items_center().justify_between()
        .child(div().font_semibold().text_size(15).text_color(t.foreground).child("Go to agent"))
        .child(div().text_size(11).text_color(t.muted).child(`${rows.length} agents · Esc`)))
      .child(h_flex().h(36).flex_none().px(10).items_center().rounded(9).border_1().border_color(t.accent).bg(t.background)
        .child(Input.new(this.input).accessibility_label("Search agents").flex_1().h_full().text_size(13).text_color(t.foreground)))
      .when(!!this.error, el => el.child(div().text_size(12).text_color(t.blocked).child(this.error)))
      .child(div().id("agent-results").role("list_box").accessibility_label("Matching agents").relative().h(height).min_h(0).overflow_hidden()
        .on_scroll_wheel((e, cx) => {
          const next = Math.max(0, Math.min(this.scroll - e.delta.y, rows.length * ROW - height));
          this.scroll = next;
          if (rows.length) this.selected = rows[Math.max(Math.ceil(next / ROW), Math.min(at, Math.floor((next + height) / ROW) - 1))]?.key;
          cx.stop_propagation(); cx.notify();
        })
        .when(!rows.length, el => el.child(div().p(12).text_color(t.muted).child(this.query.trim() ? "No matching agents. Try a model, title or folder." : "No live agents are available.")))
        .children(rows.map((r, i) => {
          if ((i + 1) * ROW <= this.scroll || i * ROW >= this.scroll + height) return null;
          const on = r.key === selected?.key;
          return h_flex().id(`agent-${r.key}`).role("list_box_option").aria_selected(on).when(on, el => el.aria_active_descendant())
            .accessibility_label(`${r.display}, ${r.status}, ${r.context}`).tooltip(`${r.display}\n${r.context}`)
            .absolute().left(0).right(0).top(i * ROW - this.scroll).h(ROW).px(10).gap(10).items_center().rounded(7)
            .cursor_pointer().when(on, el => el.bg(t.selected)).hover(el => el.bg(t.hover))
            .on_click((_e, cx) => this.go(r, cx))
            .child(div().w(12).flex_none().text_color(on ? t.accent : t.muted).child(on ? "›" : "·"))
            .child(v_flex().flex_1().min_w(0).gap(3)
              .child(div().truncate().text_color(t.foreground).font_semibold().child(r.display))
              .child(div().truncate().text_size(11).text_color(t.muted).child(r.context)))
            .child(div().flex_none().text_size(11).text_color(t[r.status] || t.muted).child(r.status));
        }).filter(Boolean)))
      .child(div().text_size(11).text_color(t.muted).child("↑ ↓ Select   Enter Focus exact terminal"));
  }
}
