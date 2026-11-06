import { View, div } from "gpui-kit";
import { v_flex, h_flex, InputState, Input } from "gpui-base";
import { spaces, machines, home, exec, theme, run, props, closeOverlay } from "herdr";
import { spaceRows, folderKey, folderName, homeCandidate } from "./model.js";

const ROW = 56;
const HEIGHT = ROW * 6;

async function directory(path) {
  const resolved = await exec("realpath", ["-e", "--", path]);
  if (resolved.code !== 0) return null;
  const canonical = resolved.stdout.trimEnd();
  if (!canonical || /[\r\n\x00]/.test(canonical)) return null;
  const check = await exec("test", ["-d", canonical]);
  return check.code === 0 ? canonical : null;
}

export default class Workspacex extends View {
  init(_props, cx) {
    this.query = ""; this.selected = null; this.scroll = 0;
    this.folders = []; this.loading = false; this.busy = false;
    this.error = ""; this.lookupError = ""; this.generation = 0; this.closed = false;
    this.handle = cx.focus_handle();
    this.input = InputState.new({ placeholder: "Search spaces, or find a folder with zoxide…" });
    this.input.on("change", (_e, cx) => {
      this.query = this.input.value(); this.selected = null; this.scroll = 0;
      this.folders = []; this.error = ""; this.lookupError = "";
      const generation = ++this.generation;
      this.loading = !!this.query.trim();
      if (this.loading) cx.timer.after(180, cx => { if (generation === this.generation) this.lookup(cx, generation); });
      cx.notify();
    });
    this.input.on("submit", (_e, cx) => this.go(this.selection(this.rows()), cx));
    this.input.focus();
  }

  rows() { return spaceRows(spaces(), this.query, this.folders); }
  selection(rows) { return rows.find(r => r.key === this.selected) || rows[0]; }
  close() { this.closed = true; }
  dismiss() { this.closed = true; closeOverlay(props().overlay); }

  lookup(cx, generation = ++this.generation) {
    const query = this.query.trim();
    if (!query) return;
    this.loading = true; this.lookupError = ""; cx.notify();
    cx.spawn(async cx => {
      let folders = [];
      let issue = "";
      try {
        const result = await exec("zoxide", ["query", "--list", "--", ...query.split(/\s+/)]);
        if (result.code === 0) folders = result.stdout.split("\n").filter(p => p.startsWith("/") && !/[\r\x00]/.test(p));
        else if (result.code !== 1 || !/no match/i.test(result.stderr)) issue = "Folder search failed. Retry or change the query.";
      } catch { issue = "Zoxide is unavailable. Retry or use an existing folder under home."; }
      if (!folders.length) {
        try {
          const candidate = homeCandidate(home(), query);
          const folder = candidate ? await directory(candidate) : null;
          const root = folderKey(home());
          if (folder && folder.startsWith(`${root}/`)) folders = [folder];
        } catch { issue = issue || "Couldn't check the home folder. Retry the search."; }
      }
      if (this.closed || generation !== this.generation) return;
      this.folders = folders; this.lookupError = issue; this.loading = false; cx.notify();
    });
  }

  go(row, cx) {
    if (!row || this.busy || this.closed) return;
    this.error = "";
    if (row.kind === "space") {
      if (!spaces().some(s => s.id === row.id)) { this.error = "That space is no longer available. Choose another result."; cx.notify(); return; }
      try { run("space.select", { id: row.id }); this.dismiss(); }
      catch (error) { this.error = String(error); cx.notify(); }
      return;
    }
    this.busy = true; cx.notify();
    cx.spawn(async cx => {
      try {
        const folder = await directory(row.folder);
        if (this.closed) return;
        if (!folder) throw new Error("That folder is no longer available. Search again; no space was created.");
        // Resolve live local folders again at activation, including symlink aliases.
        const local = spaces().filter(s => !s.machine && s.folder);
        const canonical = await Promise.all(local.map(async s => ({ id: s.id, folder: await directory(s.folder) })));
        if (this.closed) return;
        const current = spaces();
        const open = current.find(s => !s.machine && (folderKey(s.folder) === folderKey(folder) || canonical.some(c => c.id === s.id && c.folder === folder)));
        if (open) run("space.select", { id: open.id });
        else {
          const machine = machines().find(m => m.name === null);
          if (machine && machine.layout !== "ready") throw new Error("Local spaces are unavailable. Reconnect, then try again.");
          run("space.new", { name: folderName(folder), folder, machine: "" });
        }
        this.dismiss();
      } catch (error) { this.error = String(error).replace(/^Error: /, ""); }
      this.busy = false; cx.notify();
    });
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
    const unavailable = machines().some(m => m.layout !== "ready");
    return v_flex().id("workspacex").role("dialog").accessibility_label("Go to space").track_focus(this.handle).w_full().min_h(0).p(18).gap(10).bg(t.popover).text_size(13)
      .on_key_down((e, cx) => {
        if (e.key === "escape") { cx.stop_propagation(); this.dismiss(); return; }
        if (e.key !== "up" && e.key !== "down") return;
        cx.stop_propagation();
        if (rows.length) this.selected = rows[(at + (e.key === "down" ? 1 : rows.length - 1)) % rows.length].key;
        cx.notify();
      })
      .child(h_flex().items_center().justify_between()
        .child(div().font_semibold().text_size(15).text_color(t.foreground).child("Go to space"))
        .child(div().text_size(11).text_color(t.muted).child(this.busy ? "Opening…" : "Esc")))
      .child(h_flex().h(36).flex_none().px(10).items_center().rounded(9).border_1().border_color(t.accent).bg(t.background)
        .child(Input.new(this.input).accessibility_label("Search spaces or folders").flex_1().h_full().text_size(13).text_color(t.foreground)))
      .when(!!this.error, el => el.child(div().text_size(12).text_color(t.blocked).child(this.error)))
      .when(!!this.lookupError, el => el.child(h_flex().gap(10).items_center()
        .child(div().flex_1().text_size(12).text_color(t.blocked).child(this.lookupError))
        .child(div().id("retry-search").px(8).py(5).rounded(5).text_color(t.foreground).bg(t.raised).cursor_pointer().on_click((_e, cx) => this.lookup(cx)).child("Retry"))))
      .when(unavailable, el => el.child(div().text_size(11).text_color(t.muted).child("Some machines are loading or unavailable; their spaces may be missing.")))
      .child(div().id("space-results").role("list_box").accessibility_label("Matching spaces and folders").relative().h(height).min_h(0).overflow_hidden()
        .on_scroll_wheel((e, cx) => {
          const next = Math.max(0, Math.min(this.scroll - e.delta.y, rows.length * ROW - height));
          this.scroll = next;
          if (rows.length) this.selected = rows[Math.max(Math.ceil(next / ROW), Math.min(at, Math.floor((next + height) / ROW) - 1))]?.key;
          cx.stop_propagation(); cx.notify();
        })
        .when(!rows.length, el => el.child(div().p(12).text_color(t.muted).child(this.loading ? "Finding existing folders…" : this.query.trim() ? "No other spaces or existing folders match. Change the query." : "No other spaces. Type to find a folder and create a local space.")))
        .children(rows.map((r, i) => {
          if ((i + 1) * ROW <= this.scroll || i * ROW >= this.scroll + height) return null;
          const on = r.key === selected?.key;
          return h_flex().id(r.key).role("list_box_option").aria_selected(on).when(on, el => el.aria_active_descendant())
            .accessibility_label(`${r.name}, ${r.detail}`).tooltip(`${r.name}\n${r.detail}`)
            .absolute().left(0).right(0).top(i * ROW - this.scroll).h(ROW).px(10).gap(10).items_center().rounded(7)
            .cursor_pointer().when(on, el => el.bg(t.selected)).hover(el => el.bg(t.hover))
            .on_click((_e, cx) => this.go(r, cx))
            .child(div().w(12).flex_none().text_color(on ? t.accent : t.muted).child(r.kind === "create" ? "+" : on ? "›" : "·"))
            .child(v_flex().flex_1().min_w(0).gap(3)
              .child(div().truncate().text_color(t.foreground).font_semibold().child(r.name))
              .child(div().truncate().text_size(11).text_color(t.muted).child(r.detail)))
            .child(div().flex_none().text_size(11).text_color(t[r.status] || t.muted).child(r.kind === "create" ? "Create" : r.status || "Switch"));
        }).filter(Boolean)))
      .child(div().text_size(11).text_color(t.muted).child(this.loading && rows.length ? "Finding folders…   ↑ ↓ Select   Enter Open" : "↑ ↓ Select   Enter Open   Creation uses existing local folders only"));
  }
}
