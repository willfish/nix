// /nuke deletes one Pi session's local file and the local indexes that copy it.
// /nuke [id|path] [!]  Add ! to skip the confirmation.
// The current session is deleted after this process exits, then Pi exits.
// Forks, provider logs, and shared channel posts are separate and are not removed.

import { spawn, spawnSync } from "node:child_process";
import {
  closeSync,
  lstatSync,
  openSync,
  readSync,
  readdirSync,
  realpathSync,
  unlinkSync,
} from "node:fs";
import { homedir } from "node:os";
import { basename, dirname, isAbsolute, join, relative, resolve, sep } from "node:path";

const SESSION_ID = /^[A-Za-z0-9_-]{8,128}$/;

export type SessionCandidate = {
  path: string;
  id?: string;
};

export type WipeSpec = {
  waitPid?: number;
  unlink: string[];
  sidecarDir?: string;
  sidecarPrefix?: string;
  memoryDir?: string;
  memoryRoot?: string;
  removeParent?: string;
  sessionsRoot?: string;
  db?: string;
  sessionPaths: string[];
};

type Header = {
  id?: string;
  cwd?: string;
  parentSession?: string;
};

type CommandCtx = {
  cwd: string;
  hasUI: boolean;
  mode?: string;
  ui: {
    confirm: (title: string, message?: string) => Promise<boolean>;
    notify: (message: string, level?: string) => void;
  };
  shutdown: () => void;
  sessionManager: {
    getSessionFile?: () => string | null | undefined;
    getSessionId?: () => string | null | undefined;
    getSessionsDir?: () => string | null | undefined;
  };
};

export function agentDir(env: NodeJS.ProcessEnv = process.env): string {
  const raw = env.PI_CODING_AGENT_DIR?.trim();
  return raw ? resolve(raw) : join(homedir(), ".pi", "agent");
}

export function sessionsRootFromEnv(env: NodeJS.ProcessEnv = process.env): string {
  const raw = env.PI_CODING_AGENT_SESSION_DIR?.trim();
  return raw ? resolve(raw) : join(agentDir(env), "sessions");
}

export function historyDbPath(env: NodeJS.ProcessEnv = process.env): string {
  return join(agentDir(env), "prompt-history", "history.db");
}

export function withinRoot(path: string, root: string): boolean {
  const rel = relative(resolve(root), resolve(path));
  return rel === "" || (rel !== ".." && !rel.startsWith(`..${sep}`) && !isAbsolute(rel));
}

export function idFromFilename(name: string): string | undefined {
  if (!name.endsWith(".jsonl")) return undefined;
  const stem = name.slice(0, -".jsonl".length);
  const split = stem.lastIndexOf("_");
  if (split < 0 || split === stem.length - 1) return undefined;
  const id = stem.slice(split + 1);
  return SESSION_ID.test(id) ? id : undefined;
}

export function parseNukeArgs(args: string): { selector: string; force: boolean } {
  let force = false;
  const kept: string[] = [];
  for (const part of args.trim().split(/\s+/).filter(Boolean)) {
    if (part === "!" || part === "--yes" || part === "-y") force = true;
    else kept.push(part);
  }
  return { selector: kept.join(" "), force };
}

export function sameFile(left: string, right: string): boolean {
  if (resolve(left) === resolve(right)) return true;
  const a = safeRealpath(left);
  const b = safeRealpath(right);
  return Boolean(a && b && a === b);
}

export function listSessionFiles(root: string): string[] {
  const out: string[] = [];
  const rootReal = safeRealpath(root) ?? resolve(root);
  const walk = (dir: string) => {
    let entries: ReturnType<typeof readdirSync>;
    try {
      entries = readdirSync(dir, { withFileTypes: true });
    } catch {
      return;
    }
    for (const entry of entries) {
      const path = join(dir, entry.name);
      if (entry.isSymbolicLink()) continue;
      if (entry.isDirectory()) {
        const real = safeRealpath(path);
        if (!real || !withinRoot(real, rootReal) || real === rootReal) continue;
        walk(path);
        continue;
      }
      if (entry.isFile() && entry.name.endsWith(".jsonl")) out.push(path);
    }
  };
  walk(root);
  return out;
}

export function matchCandidates(
  candidates: SessionCandidate[],
  selector: string,
  current?: string,
): SessionCandidate[] {
  const query = selector.trim();
  if (!query) {
    if (!current) return [];
    const found = candidates.find((candidate) => sameFile(candidate.path, current));
    return [found ?? { path: current, id: idFromFilename(basename(current)) }];
  }

  const lowered = query.toLowerCase();
  const exact = candidates.filter((candidate) => {
    if (candidate.path === query || sameFile(candidate.path, query)) return true;
    if (basename(candidate.path) === query) return true;
    return candidate.id?.toLowerCase() === lowered;
  });
  if (exact.length > 0) return uniquePaths(exact);

  if (query.length < 4 || query.includes("*") || query.includes("?")) return [];
  const prefix = candidates.filter((candidate) => candidate.id?.toLowerCase().startsWith(lowered));
  return uniquePaths(prefix);
}

export function readHeader(path: string): Header {
  let fd: number | undefined;
  try {
    fd = openSync(path, "r");
    const buf = Buffer.alloc(8192);
    const n = readSync(fd, buf, 0, buf.length, 0);
    const line = buf.subarray(0, n).toString("utf8").split("\n", 1)[0] ?? "";
    const header = JSON.parse(line) as { type?: string; id?: string; cwd?: string; parentSession?: string };
    if (header.type !== "session") return {};
    return {
      id: typeof header.id === "string" ? header.id : undefined,
      cwd: typeof header.cwd === "string" ? header.cwd : undefined,
      parentSession: typeof header.parentSession === "string" ? header.parentSession : undefined,
    };
  } catch {
    return {};
  } finally {
    if (fd !== undefined) closeSync(fd);
  }
}

export function memoryDirFor(cwd: string | undefined, sessionId: string | undefined): string | undefined {
  if (!cwd || !sessionId || !isAbsolute(cwd) || !SESSION_ID.test(sessionId)) return undefined;
  return join(cwd, ".memory", sessionId);
}

export function isAllowedSessionPath(path: string, root: string, current?: string): boolean {
  if (!path.endsWith(".jsonl")) return false;
  if (current && resolve(path) === resolve(current)) return true;
  if (!withinRoot(resolve(path), resolve(root))) return false;
  return isFileEntry(path);
}

export function buildWipeSpec(input: {
  sessionFile: string;
  sessionsRoot: string;
  currentFile?: string;
  cwd?: string;
  sessionId?: string;
  dbPath?: string;
}): WipeSpec {
  const sessionFile = resolve(input.sessionFile);
  if (!isAllowedSessionPath(sessionFile, input.sessionsRoot, input.currentFile)) {
    throw new Error("refusing to delete a path outside session storage");
  }
  const root = resolve(input.sessionsRoot);
  if (sessionFile === root || sessionFile === dirname(root)) {
    throw new Error("refusing to delete the sessions directory");
  }
  const unlink = unlinkTargets(sessionFile, root, input.currentFile);
  const parent = dirname(sessionFile);
  const removeParent = withinRoot(parent, root) && parent !== root ? parent : undefined;
  const memoryDir = memoryDirFor(input.cwd, input.sessionId);
  const paths = new Set<string>([sessionFile, ...unlink]);
  return {
    unlink,
    sidecarDir: parent,
    sidecarPrefix: basename(sessionFile),
    memoryDir,
    memoryRoot: memoryDir ? dirname(memoryDir) : undefined,
    removeParent,
    sessionsRoot: root,
    db: input.dbPath,
    sessionPaths: [...paths],
  };
}

export const WIPE_SCRIPT = String.raw`
import json, os, sqlite3, sys, time, shutil

spec = json.loads(sys.argv[1])

def gone(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return True
    except PermissionError:
        return False
    return False

def unlink_file(path):
    if not path or not os.path.lexists(path):
        return
    if os.path.isdir(path) and not os.path.islink(path):
        return
    os.unlink(path)

def contained(path, root):
    if not path or not root:
        return False
    real = os.path.realpath(path)
    root_real = os.path.realpath(root)
    return real == root_real or real.startswith(root_real + os.sep)

def remove_memory(path, root):
    if not path or not root or not os.path.lexists(path):
        return
    if os.path.islink(path):
        os.unlink(path)
        return
    if not os.path.isdir(path):
        return
    real = os.path.realpath(path)
    root_real = os.path.realpath(root)
    if real == root_real or not real.startswith(root_real + os.sep):
        return
    if os.path.basename(real) != os.path.basename(path):
        return
    shutil.rmtree(real)

def remove_empty(path, root):
    if not path or not root or not contained(path, root):
        return
    if os.path.realpath(path) == os.path.realpath(root):
        return
    try:
        os.rmdir(path)
    except OSError:
        pass

def scrub_db(db, paths, base):
    if not db or not os.path.exists(db):
        return
    for _ in range(40):
        try:
            con = sqlite3.connect(db, timeout=1)
            con.execute("PRAGMA foreign_keys=ON")
            rows = [row[0] for row in con.execute("SELECT session_file FROM sessions")]
            targets = []
            for stored in rows:
                if stored in paths or (base and (stored.endswith("/" + base) or stored.endswith("\\" + base))):
                    targets.append(stored)
            for stored in targets:
                con.execute("DELETE FROM prompts WHERE session_file = ?", (stored,))
                con.execute("DELETE FROM sessions WHERE session_file = ?", (stored,))
            con.commit()
            try:
                con.execute("VACUUM")
            except sqlite3.Error:
                pass
            con.close()
            return
        except sqlite3.OperationalError:
            time.sleep(0.1)
    raise SystemExit("prompt history stayed locked")

pid = spec.get("waitPid")
if pid:
    while not gone(int(pid)):
        time.sleep(0.05)
    time.sleep(0.2)

for path in spec.get("unlink") or []:
    unlink_file(path)
sidecar_dir = spec.get("sidecarDir")
prefix = spec.get("sidecarPrefix")
if sidecar_dir and prefix and os.path.isdir(sidecar_dir):
    for name in os.listdir(sidecar_dir):
        if name == prefix or name.startswith(prefix + "."):
            unlink_file(os.path.join(sidecar_dir, name))
remove_memory(spec.get("memoryDir"), spec.get("memoryRoot"))
scrub_db(spec.get("db"), set(spec.get("sessionPaths") or []), prefix or "")
remove_empty(spec.get("removeParent"), spec.get("sessionsRoot"))
`;

export function runWipe(spec: WipeSpec): { ok: boolean; error?: string } {
  const result = spawnSync("python3", ["-c", WIPE_SCRIPT, JSON.stringify(spec)], { encoding: "utf8" });
  if (result.error) return { ok: false, error: result.error.message };
  if (result.status === 0) return { ok: true };
  const detail = (result.stderr || result.stdout || "wipe failed").trim().split("\n").at(-1) ?? "wipe failed";
  return { ok: false, error: detail.slice(0, 180) };
}

export function armReaper(spec: WipeSpec): { ok: boolean; error?: string } {
  const ready = spawnSync("python3", ["-c", "import sqlite3"], { stdio: "ignore" });
  if (ready.status !== 0) return { ok: false, error: "python3 is not available for post-exit cleanup" };
  const child = spawn("python3", ["-c", WIPE_SCRIPT, JSON.stringify({ ...spec, waitPid: process.pid })], {
    detached: true,
    stdio: "ignore",
  });
  child.unref();
  return { ok: true };
}

function safeRealpath(path: string): string | undefined {
  try {
    return realpathSync(path);
  } catch {
    return undefined;
  }
}

function isFileEntry(path: string): boolean {
  try {
    const stat = lstatSync(path);
    return stat.isFile() || stat.isSymbolicLink();
  } catch {
    return false;
  }
}

function uniquePaths(candidates: SessionCandidate[]): SessionCandidate[] {
  const seen = new Set<string>();
  const out: SessionCandidate[] = [];
  for (const candidate of candidates) {
    const key = safeRealpath(candidate.path) ?? resolve(candidate.path);
    if (seen.has(key)) continue;
    seen.add(key);
    out.push(candidate);
  }
  return out;
}

function unlinkTargets(path: string, root: string, current?: string): string[] {
  const targets = [path];
  const real = safeRealpath(path);
  if (!real || real === path) return targets;
  const currentReal = current ? safeRealpath(current) : undefined;
  if (withinRoot(real, root) || (currentReal && real === currentReal)) targets.push(real);
  return targets.filter((target) => {
    try {
      return lstatSync(target).isFile() || lstatSync(target).isSymbolicLink();
    } catch {
      return target === path;
    }
  });
}

function sessionsRoot(ctx: CommandCtx): string {
  try {
    const fromApi = ctx.sessionManager.getSessionsDir?.();
    if (typeof fromApi === "string" && fromApi.trim()) return resolve(fromApi);
  } catch {
    // Fall back to the env/default directory.
  }
  return sessionsRootFromEnv();
}

function candidatesFor(root: string, current?: string): SessionCandidate[] {
  const files = listSessionFiles(root);
  if (current && !files.some((file) => sameFile(file, current))) files.push(current);
  return files.map((file) => ({ path: file, id: idFromFilename(basename(file)) ?? readHeader(file).id }));
}

function countForks(files: string[], parent: string): number {
  let count = 0;
  for (const file of files) {
    if (sameFile(file, parent)) continue;
    const header = readHeader(file);
    if (header.parentSession && sameFile(header.parentSession, parent)) count += 1;
  }
  return count;
}

let exitUnlinks: string[] = [];
process.on("exit", () => {
  for (const path of exitUnlinks) {
    try {
      unlinkSync(path);
    } catch {
      // The reaper removes a file recreated by shutdown.
    }
  }
});

export default function nukeSession(pi: {
  on: (name: string, handler: () => void) => void;
  registerCommand: (name: string, spec: Record<string, unknown>) => void;
}) {
  pi.on("session_shutdown", () => {
    for (const path of exitUnlinks) {
      try {
        unlinkSync(path);
      } catch {
        // Already removed, or shutdown recreated it and the reaper will remove it.
      }
    }
  });

  pi.registerCommand("nuke", {
    description: "Delete a session file and its local index. Exits when the target is current. /nuke [id|path] [!]",
    getArgumentCompletions: (prefix: string) => {
      const needle = prefix.trim().toLowerCase();
      const values: { value: string; label: string }[] = [];
      for (const file of listSessionFiles(sessionsRootFromEnv())) {
        const id = idFromFilename(basename(file));
        const value = id ?? basename(file);
        if (needle && !value.toLowerCase().startsWith(needle) && !basename(file).toLowerCase().includes(needle)) continue;
        values.push({ value, label: id ? `${id.slice(0, 8)} ${basename(file)}` : basename(file) });
        if (values.length >= 40) break;
      }
      return values.length > 0 ? values : null;
    },
    handler: async (args: string, ctx: CommandCtx) => {
      const { selector, force } = parseNukeArgs(args);
      const root = sessionsRoot(ctx);
      const current = ctx.sessionManager.getSessionFile?.() || undefined;
      let matches = matchCandidates(candidatesFor(root, current), selector, current)
        .filter((candidate) => isAllowedSessionPath(candidate.path, root, current));
      if (matches.length === 0 && selector) {
        const abs = isAbsolute(selector) ? selector : resolve(ctx.cwd, selector);
        if (isAllowedSessionPath(abs, root, current)) {
          matches = [{ path: abs, id: idFromFilename(basename(abs)) ?? readHeader(abs).id }];
        }
      }

      if (matches.length === 0) {
        if (!selector && !current) {
          if (!force) {
            if (!ctx.hasUI) {
              ctx.ui.notify("Refusing to exit without confirmation. Re-run with !", "error");
              return;
            }
            const ok = await ctx.ui.confirm("Nuke session", "This session has no file. Exit anyway?");
            if (!ok) return;
          }
          ctx.shutdown();
          return;
        }
        ctx.ui.notify(selector ? "No matching session." : "This session has no file to delete.", "error");
        return;
      }
      if (matches.length > 1) {
        const ids = matches.slice(0, 5).map((candidate) => candidate.id?.slice(0, 8) ?? basename(candidate.path));
        ctx.ui.notify(`That matches ${matches.length} sessions (${ids.join(", ")}). Pass a longer id.`, "error");
        return;
      }

      const target = matches[0].path;
      const currentTarget = Boolean(current && sameFile(target, current));
      const header = readHeader(target);
      const sessionId = header.id ?? matches[0].id ?? ctx.sessionManager.getSessionId?.() ?? undefined;
      const cwd = header.cwd ?? (currentTarget ? ctx.cwd : undefined);
      const forks = countForks(listSessionFiles(root), target);
      const forkNote = forks > 0 ? ` ${forks} forked session${forks === 1 ? "" : "s"} will still contain copied history.` : "";

      if (!force) {
        if (!ctx.hasUI) {
          ctx.ui.notify("Refusing to nuke without confirmation. Re-run with !", "error");
          return;
        }
        const message = currentTarget
          ? `Delete this session and exit? Its file, prompt history, and local observational memory will be removed.${forkNote}`
          : `Delete that session's local file and index? This Pi stays open.${forkNote}`;
        const ok = await ctx.ui.confirm("Nuke session", message);
        if (!ok) return;
      }

      let spec: WipeSpec;
      try {
        spec = buildWipeSpec({
          sessionFile: target,
          sessionsRoot: root,
          currentFile: current,
          cwd,
          sessionId,
          dbPath: historyDbPath(),
        });
      } catch (error) {
        ctx.ui.notify(error instanceof Error ? error.message : "Refusing to delete that path.", "error");
        return;
      }

      if (currentTarget) {
        exitUnlinks = spec.unlink;
        const armed = armReaper(spec);
        if (!armed.ok) ctx.ui.notify(armed.error ?? "Post-exit cleanup was not armed.", "warning");
        ctx.shutdown();
        return;
      }

      const wiped = runWipe(spec);
      ctx.ui.notify(wiped.ok ? "Deleted." : (wiped.error ?? "Could not delete that session."), wiped.ok ? "info" : "error");
    },
  });
}
