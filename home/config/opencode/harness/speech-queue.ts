import { spawn } from "node:child_process";
import { randomUUID } from "node:crypto";
import { lstat, mkdir, open, readdir, rm } from "node:fs/promises";
import { homedir } from "node:os";
import { join } from "node:path";
import { setTimeout as delay } from "node:timers/promises";

const FLOCK = "flock";
const MAX_PENDING = 3;
const MAX_WAIT_MS = 30_000;

function alive(pid: number) {
  try { process.kill(pid, 0); return true; }
  catch (error) { if (error.code === "ESRCH") return false; throw error; }
}

async function tryLock(fd: number) {
  // flock acts on the inherited open-file description, so the parent keeps the
  // lock after this short helper exits. Never unlink the shared lock file.
  return new Promise<boolean>((resolve, reject) => {
    const child = spawn(FLOCK, ["--exclusive", "--nonblock", "3"], { stdio: ["ignore", "ignore", "ignore", fd] });
    child.once("error", () => reject(new Error("Could not start the speech queue lock helper")));
    child.once("close", code => {
      if (code === 0 || code === 1) resolve(code === 0);
      else reject(new Error("Could not lock the speech queue"));
    });
  });
}

export async function withSpeechQueue<T>(work: (fd: number) => Promise<T>, {
  signal,
  maxWaitMs = MAX_WAIT_MS,
}: { signal?: AbortSignal; maxWaitMs?: number } = {}) {
  signal?.throwIfAborted();
  const base = process.env.XDG_RUNTIME_DIR || process.env.XDG_CACHE_HOME || join(homedir(), ".cache");
  const directory = join(base, "opencode-speech");
  await mkdir(directory, { recursive: true, mode: 0o700 });
  const stat = await lstat(directory);
  if (!stat.isDirectory() || stat.uid !== process.getuid() || (stat.mode & 0o077)) {
    throw new Error("Unsafe speech queue directory");
  }
  const started = Date.now();
  const deadline = started + maxWaitMs;
  // Tickets hold only ordering, expiry and owner metadata, never speech text.
  const ticket = join(directory, `${started}-${deadline}-${process.pid}-${randomUUID()}.wait`);
  const file = await open(ticket, "wx", 0o600);
  await file.close();
  let lock;
  try {
    while (true) {
      signal?.throwIfAborted();
      if (Date.now() >= deadline) return { ok: false, skipped: "expired" } as const;
      const pending: string[] = [];
      for (const name of (await readdir(directory)).sort()) {
        const match = /^(\d+)-(\d+)-(\d+)-[\da-f-]+\.wait$/.exec(name);
        if (!match) continue;
        if (Date.now() >= Number(match[2]) || !alive(Number(match[3]))) {
          await rm(join(directory, name), { force: true });
        } else pending.push(join(directory, name));
      }
      const position = pending.indexOf(ticket);
      if (position < 0) return { ok: false, skipped: "expired" } as const;
      if (position >= MAX_PENDING) return { ok: false, skipped: "queue-full" } as const;
      if (position === 0) {
        lock ??= await open(join(directory, "playback.lock"), "a", 0o600);
        if (await tryLock(lock.fd)) {
          signal?.throwIfAborted();
          if (Date.now() >= deadline) return { ok: false, skipped: "expired" } as const;
          await rm(ticket, { force: true });
          return await work(lock.fd);
        }
      }
      await delay(Math.min(100, Math.max(1, deadline - Date.now())), undefined, { signal });
    }
  } finally {
    await lock?.close();
    await rm(ticket, { force: true });
  }
}
