import { randomUUID } from 'node:crypto';
import { readFile, rename, writeFile, mkdir, stat } from 'node:fs/promises';
import { join } from 'node:path';

export const VERSION = 1;
export const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
export const envelope = (runId, value) => ({ ...value, version: VERSION, runId });
export async function atomicJson(file, value) {
  const tmp = `${file}.${randomUUID()}.tmp`;
  await writeFile(tmp, JSON.stringify(value), { mode: 0o600, flag: 'wx' });
  await rename(tmp, file);
}
export async function readJson(file, runId) {
  try {
    if ((await stat(file)).size > 8 * 1024 * 1024) throw new Error('Team IPC file exceeds 8 MiB');
    const data = JSON.parse(await readFile(file, 'utf8'));
    if (data.version !== VERSION || data.runId !== runId) throw new Error('Team IPC identity mismatch');
    return data;
  } catch (error) {
    if (error.code === 'ENOENT') return null;
    throw error;
  }
}
export async function prepareRun(dir, request) {
  await mkdir(join(dir, 'commands'), { mode: 0o700 });
  await mkdir(join(dir, 'results'), { mode: 0o700 });
  await atomicJson(join(dir, 'request.json'), envelope(request.runId, { parentPid: process.pid, ...request }));
  await atomicJson(join(dir, 'lease.json'), envelope(request.runId, { updated: Date.now() }));
}
export async function command(dir, runId, kind, text) {
  const id = randomUUID();
  await atomicJson(join(dir, 'commands', `${id}.json`), envelope(runId, { id, kind, text, created: Date.now() }));
  return id;
}
export function assistantResult(messages) {
  const assistants = messages.filter((message) => message.role === 'assistant');
  const last = assistants.at(-1);
  const text = last?.content?.filter((part) => part.type === 'text').map((part) => part.text).join('\n') ?? '';
  const stopReason = last?.stopReason;
  const status = stopReason === 'aborted' ? 'aborted' :
    stopReason === 'error' ? 'error' : !text || !['stop', 'end'].includes(stopReason) ? 'incomplete' : 'completed';
  return { status, text, stopReason, errorMessage: last?.errorMessage };
}
