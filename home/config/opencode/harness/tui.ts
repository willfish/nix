import { Plugin } from "@opencode/plugin/tui";
import {
  createTabSynchronizer,
  nameUnlabelledTab,
  sessionTabTitle,
  type TabOwnership,
} from "./herdr-tab.ts";

export function setupHerdrTab(
  context: any,
  options: {
    env?: Record<string, string | undefined>;
    nameTab?: typeof nameUnlabelledTab;
    intervalMs?: number;
  } = {},
) {
  const env = options.env ?? process.env;
  if (env.HERDR_ENV !== "1" || !env.HERDR_SOCKET_PATH || !env.HERDR_PANE_ID) return () => {};
  const paneKey = `${env.HERDR_SOCKET_PATH}:${env.HERDR_PANE_ID}`;
  const stored = context.storage?.store?.("herdr-tab", {
    initial: {} as Record<string, TabOwnership>,
  });
  const saved = stored?.[0]?.[paneKey];
  const initialOwnership =
    saved && ["socket", "pane", "tab", "label"].every(k => typeof saved[k] === "string")
      ? { socket: saved.socket, pane: saved.pane, tab: saved.tab, label: saved.label }
      : undefined;
  const tab = createTabSynchronizer({
    env,
    nameTab: options.nameTab,
    initialOwnership,
    onOwnership(next) {
      if (stored) void stored[1](draft => { draft[paneKey] = next; }).catch(() => {});
    },
  });
  const sessions = new Map<string, { id: string; parentID?: string; title?: string }>();
  let lastSession: string | undefined;
  let lastLabel: string | undefined;
  let lastSyncAt = -Infinity;

  function root(id: string | undefined): string | undefined {
    const seen = new Set<string>();
    while (typeof id === "string" && !seen.has(id)) {
      seen.add(id);
      const cached = context.data.session.get(id);
      const known = sessions.get(id);
      if (!cached && !known) return;
      const parentID = cached?.parentID ?? known?.parentID;
      if (!parentID) return id;
      id = parentID;
    }
  }

  function current(): string | undefined {
    const route = context.ui.router.current();
    return route?.type === "session" ? root(route.sessionID) : undefined;
  }

  function titleFor(id: string | undefined): string | undefined {
    if (!id) return;
    const cached = context.data.session.get(id);
    const known = sessions.get(id);
    if (known?.title !== undefined && cached?.title === known.title) {
      delete known.title;
    }
    return sessionTabTitle(known?.title) ?? sessionTabTitle(cached?.title);
  }

  function check(force = false) {
    const selected = current();
    const label = titleFor(selected);
    if (!selected || !label) return;
    const now = Date.now();
    if (!force && selected === lastSession && label === lastLabel && now - lastSyncAt < 5000) return;
    lastSession = selected;
    lastLabel = label;
    lastSyncAt = now;
    tab.sync(selected, label);
  }

  const unsubscribe = context.data.listen(({ details: event }: any) => {
    const data = event?.data;
    if (!data) return;
    if (event.type === "session.created") {
      sessions.set(data.sessionID, {
        id: data.sessionID,
        parentID: data.parentID,
        title: data.title ?? sessions.get(data.sessionID)?.title,
      });
      check(true);
    } else if (event.type === "session.renamed") {
      const existing = sessions.get(data.sessionID);
      sessions.set(data.sessionID, {
        id: data.sessionID,
        parentID: existing?.parentID,
        title: data.title,
      });
      check(true);
    } else if (event.type === "session.deleted") {
      sessions.delete(data.sessionID);
      check(true);
    }
  });
  check(true);
  const poll = setInterval(() => check(false), options.intervalMs ?? 250);
  poll.unref?.();
  return () => {
    clearInterval(poll);
    unsubscribe();
    tab.stop();
  };
}

export default Plugin.define({
  id: "dotfiles.workflow.tui",
  setup(context) {
    return setupHerdrTab(context);
  },
});
