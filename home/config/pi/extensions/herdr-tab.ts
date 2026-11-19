import type { ExtensionAPI, ExtensionContext } from '@earendil-works/pi-coding-agent';
import { execFile } from 'node:child_process';
import { basename } from 'node:path';
import { promisify } from 'node:util';

const execute = promisify(execFile);
const ownershipEntry = 'agent-bus-herdr-label'; // Reuse ownership recorded by earlier Pi sessions.
type Ownership = { socket: string; pane: string; tab: string; label: string; index?: string };
type Tab = { id: string; name: string; terminals: { terminal: string }[] };
type Layout = { spaces: { machine: string | null; tabs: Tab[] }[] };

function clean(value: unknown): string | undefined {
  if (typeof value !== 'string') return;
  const label = value.trim();
  if (label && !/[<>\u0000-\u001f\u007f-\u009f\u2028\u2029]/.test(label)) {
    return Array.from(label).slice(0, 48).join('');
  }
}

export default function herdrTab(pi: ExtensionAPI, options: {
  env?: NodeJS.ProcessEnv;
  app?: (...args: string[]) => Promise<string>;
} = {}) {
  const env = options.env ?? process.env;
  const terminal = env.HC_TERMINAL_ID;
  if (!terminal) return;
  const socket = `herdr-app:${env.HERDR_APP_HOME || 'default'}`;
  const app = options.app ?? (async (...args) => (await execute('herdr-app', args, {
    env, timeout: 2000, maxBuffer: 1024 * 1024,
  })).stdout);

  async function locate() {
    const layout: Layout = JSON.parse(await app('ls'));
    // Core terminal IDs are machine-local. Only the first terminal owns a split tab.
    return layout.spaces.filter(space => space.machine === null).flatMap(space => space.tabs)
      .find(tab => tab.terminals[0]?.terminal === terminal);
  }

  let ctx: ExtensionContext | undefined;
  let previous: Ownership | undefined;
  let desired: string | undefined;
  let busy = false;
  let dirty = false;

  async function drain() {
    if (busy || !ctx || !desired) return;
    busy = true;
    dirty = false;
    const label = desired;
    const session = ctx;
    const current = () => ctx === session && desired === label;
    try {
      const tab = await locate();
      if (!tab || !current()) return;
      const owned = previous?.tab === tab.id && previous.label === tab.name;
      // Herdr App's New menu starts Pi in a tab named after the program.
      const numbered = /^[0-9]+$/.test(tab.name);
      if (!owned && tab.name !== 'Pi' && !numbered) return;
      const index = owned ? previous.index : numbered ? tab.name : undefined;
      const next = `${index ? `${index} ` : ''}${label}`;
      if (next === tab.name && owned) return;
      if (next !== tab.name) {
        const latest = await locate();
        if (latest?.id !== tab.id || latest.name !== tab.name || !current()) return;
        const result = JSON.parse(await app('do', 'tab.rename', JSON.stringify({ id: tab.id, name: next })));
        if (result.ok !== true || ctx !== session) return;
      }
      previous = { socket, pane: terminal!, tab: tab.id, label: next, index };
      pi.appendEntry(ownershipEntry, previous);
    } catch { /* Optional UI integration; a closed app must not interrupt Pi. */ }
    finally {
      busy = false;
      if (dirty) void drain();
    }
  }

  function refresh() {
    if (!ctx) return;
    const entries = ctx.sessionManager.getBranch();
    function savedLabel(kind: string) {
      const entry = entries.findLast(e => e.type === 'custom' && e.customType === kind);
      return clean(entry?.type === 'custom' ? entry.data?.label : undefined);
    }
    const folder = clean(basename(ctx.cwd));
    const name = clean(pi.getSessionName());
    desired = savedLabel('agent-bus-label') ?? (name === folder ? undefined : name)
      ?? savedLabel('agent-bus-auto-label') ?? folder;
    if (busy) dirty = true;
    else void drain();
  }

  pi.on('session_start', (_event, session) => {
    if (session.mode !== 'tui') return;
    ctx = session;
    const entry = session.sessionManager.getEntries().findLast(e => {
      if (e.type !== 'custom' || e.customType !== ownershipEntry) return false;
      return e.data?.socket === socket && e.data?.pane === terminal;
    });
    previous = entry?.type === 'custom' ? entry.data : undefined;
    refresh();
  });
  pi.on('session_info_changed', refresh);
  pi.on('session_tree', refresh);
  // When installed, Switchboard can provide a short label without a successful presence update.
  const unsubscribe = pi.events.on('herdr:label_changed', refresh);
  pi.on('session_shutdown', () => {
    ctx = undefined;
    unsubscribe();
  });
}
