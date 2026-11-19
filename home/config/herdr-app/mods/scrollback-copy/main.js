import { View, div } from "gpui-kit";
import { h_flex } from "gpui-base";
import { focused, hc, home, props, openOverlay, closeOverlay, notify, run, terminals, theme } from "herdr";

// Run the pager on the desktop machine, even when the source terminal is remote.
// Pass its address separately and feed the snapshot as data, not as a command or file.
const pager = String.raw`
set -euo pipefail
snapshot=$(hc read --lines 5000 -- "$1")
printf '%s\n' "$snapshot" | nvim -u NONE -i NONE -n -M \
  --cmd 'set nomodeline' \
  -c 'let g:clipboard = "osc52"' \
  -c 'set nowrap laststatus=2 statusline=Scrollback\ \ \ /\ find\ \ \ v\ select\ \ \ y\ copy\ \ \ q\ close' \
  -c 'nnoremap <silent> q <Cmd>qa!<CR>' \
  -c 'nnoremap <silent> <Esc> <Cmd>qa!<CR>' \
  -c 'xnoremap <silent> y "+y<Cmd>qa!<CR>' \
  -c 'normal! G' -
`;

export default class ScrollbackCopy extends View {
  init(_props, cx) {
    this.closed = false;
    const { overlay, viewer } = props() ?? {};
    this.terminal = viewer;
    if (!overlay || viewer) return;
    const focus = focused();
    const source = focus?.shown ?? focus?.terminal;
    cx.spawn(async cx => {
      try {
        if (!source) throw new Error("Focus a terminal first.");
        const result = await hc([
          "run", "--bg", "--json", "--rm", "--no-parent", "--cwd", home(),
          "--", "bash", "-c", pager, "scrollback-copy", source
        ]);
        if (result.code !== 0) throw new Error("Could not start the local Neovim pager.");
        const terminal = JSON.parse(result.stdout).address;
        if (!terminal) throw new Error("The pager did not return a terminal address.");
        if (this.closed) {
          await hc(["kill", terminal]);
          return;
        }
        this.terminal = terminal;
        // Core acknowledges the launch before the app receives its terminal event.
        const live = () => terminals().some(t => t.address === terminal && t.state === "live");
        for (let i = 0; i < 40 && !live() && !this.closed; i++) await cx.sleep(50);
        if (this.closed) return;
        if (!live()) throw new Error("Neovim did not start. Check that hc and nvim are available.");
        openOverlay({
          kind: "dialog", modal: true, width: 0.8, height: 0.8,
          layout: { type: "column", children: [
            { type: "block", size: { px: 32 }, content: { mod: "scrollback-copy", props: { viewer: terminal } } },
            { type: "block", content: { terminal } }
          ] }
        });
        this.terminal = null; // The viewer now owns the pager.
      } catch (error) {
        if (!this.closed) notify({ title: "Could not open copy mode", body: error.message, level: "error" });
      } finally {
        if (!this.closed) closeOverlay(overlay);
      }
    });
  }

  close() {
    this.closed = true;
    const terminal = this.terminal;
    if (terminal && terminals().some(t => t.address === terminal && t.state === "live")) {
      run("terminal.kill", { terminal });
    }
  }

  render() {
    const { overlay, viewer } = props() ?? {};
    if (!viewer) return div().id("scrollback-copy-worker").w(0).h(0);
    const t = theme();
    return h_flex().size_full().items_center().justify_between().px(12)
      .bg(t.surface).text_color(t.foreground)
      .child("Copy scrollback")
      .child(div().id("close-scrollback").px(8).cursor_pointer().hover(s => s.bg(t.hover))
        .on_click(() => closeOverlay(overlay)).child("Close"));
  }
}
