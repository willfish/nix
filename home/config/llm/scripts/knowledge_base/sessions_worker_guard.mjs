import fs from "node:fs";
import path from "node:path";

// This is a tool policy, not an OS sandbox against concurrent filesystem edits.
export default function sessionWorkerGuard(pi) {
  const root = fs.realpathSync(process.env.SESSIONS_KB_ROOT);
  const jobsPath = path.resolve(process.env.SESSIONS_WORKER_JOBS);
  const jobs = JSON.parse(fs.readFileSync(jobsPath, "utf8"));
  const maxTurns = Number(process.env.SESSIONS_MAX_TURNS);
  if (!Number.isSafeInteger(maxTurns) || maxTurns < 1 || !Array.isArray(jobs)) {
    throw new Error("Invalid session worker policy");
  }
  const writes = new Set([
    process.env.SESSIONS_WORKER_RESULTS, process.env.SESSIONS_WORKER_LOG,
    ...jobs.map((job) => path.resolve(root, job.output_path)),
  ].map((file) => path.resolve(file)));
  const reads = new Set([
    ...writes, jobsPath, path.join(root, ".kb/sessions-phase-b-index.json"),
    ...jobs.map((job) => path.resolve(root, job.input_path)),
  ]);
  let turns = 0;
  pi.on("turn_start", () => { turns += 1; });
  pi.on("tool_call", (event) => {
    const block = (reason) => ({ block: true, terminate: true, reason });
    if (turns >= maxTurns) return block("Session worker turn limit reached");
    if (!["read", "write", "edit"].includes(event.toolName)) {
      return block("Session workers only have file tools");
    }
    const input = event.input?.path;
    if (typeof input !== "string" || !input) return block("Missing file path");
    const target = path.resolve(root, input);
    if (!target.startsWith(`${root}${path.sep}`)) return block("Path outside knowledge base");
    // Reject symlinks, including broken links and parent-directory links.
    let current = root;
    for (const part of path.relative(root, target).split(path.sep)) {
      current = path.join(current, part);
      try {
        if (fs.lstatSync(current).isSymbolicLink()) return block("Symlink access forbidden");
      } catch (error) {
        if (error.code !== "ENOENT") return block("Cannot validate file path");
      }
    }
    const relative = path.relative(root, target);
    const curated = /^(confluence|slack|jira|sessions|compressed)\//.test(relative);
    if (event.toolName === "read" ? !reads.has(target) && !curated : !writes.has(target)) {
      return block("File is not authorized for this worker");
    }
    event.input.path = target;
    return undefined;
  });
}
