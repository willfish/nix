import test from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter, getEventListeners, once } from 'node:events';
import { spawn, execFileSync } from 'node:child_process';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';
import { waitForProcess } from '../home/config/pi/extensions/subagent/jobs.ts';

function fake() {
  const proc = new EventEmitter();
  proc.signals = [];
  proc.kill = signal => { proc.killed = true; proc.signals.push(signal); return true; };
  return proc;
}
for (const preAborted of [false, true]) test(`escalates despite killed=true, pre-aborted=${preAborted}`, async () => {
  const proc = fake(), controller = new AbortController();
  if (preAborted) controller.abort();
  const result = waitForProcess(proc, { signal: controller.signal, graceMs: 10 });
  controller.abort();
  await delay(30);
  assert.deepEqual(proc.signals, ['SIGTERM', 'SIGKILL']);
  proc.emit('exit', null, 'SIGKILL');
  proc.emit('close', null, 'SIGKILL');
  assert.equal(await result, 1);
  assert.equal(getEventListeners(controller.signal, 'abort').length, 0);
  for (const name of ['exit', 'close', 'error']) assert.equal(proc.listenerCount(name), 0);
});
for (const event of ['close', 'error']) test(`${event} clears cancellation listener and escalation timer`, async () => {
  const proc = fake(), controller = new AbortController();
  const result = waitForProcess(proc, { signal: controller.signal, graceMs: 10 });
  controller.abort();
  proc.emit(event, event === 'error' ? new Error('spawn failed') : 0);
  await result;
  await delay(30);
  assert.deepEqual(proc.signals, ['SIGTERM']);
  assert.equal(getEventListeners(controller.signal, 'abort').length, 0);
});
test('synchronous signal error cannot leave an escalation timer behind', async () => {
  const proc = fake(), controller = new AbortController();
  proc.kill = signal => { proc.signals.push(signal); proc.emit('error', new Error('signal denied')); return false; };
  const result = waitForProcess(proc, { signal: controller.signal, graceMs: 10 });
  controller.abort();
  assert.equal(await result, 1);
  await delay(30);
  assert.deepEqual(proc.signals, ['SIGTERM']);
  assert.equal(getEventListeners(controller.signal, 'abort').length, 0);
});

test('normal exit removes abort listener and preserves exit code', async () => {
  const proc = fake(), controller = new AbortController();
  const result = waitForProcess(proc, { signal: controller.signal });
  proc.emit('exit', 7); proc.emit('close', 7);
  assert.equal(await result, 7);
  controller.abort();
  assert.deepEqual(proc.signals, []);
});
test('exited direct child is not signalled while pipes drain', async () => {
  const proc = fake(), controller = new AbortController();
  const result = waitForProcess(proc, { signal: controller.signal, graceMs: 10 });
  proc.emit('exit', 0); controller.abort();
  await delay(30); proc.emit('close', 0);
  await result;
  assert.deepEqual(proc.signals, []);
});
for (const event of ['close', 'error']) test(`group escalation survives launcher ${event}`, async t => {
  const proc = fake(), controller = new AbortController(), signals = [];
  proc.pid = 12345;
  t.mock.method(process, 'kill', (pid, signal) => { assert.equal(pid, -proc.pid); signals.push(signal); return true; });
  let settled = false;
  const result = waitForProcess(proc, { signal: controller.signal, processGroup: true, graceMs: 20 });
  result.then(() => { settled = true; });
  controller.abort();
  proc.emit('exit', 0);
  proc.emit(event, event === 'error' ? new Error('pipe failed') : 0);
  await Promise.resolve();
  assert.equal(settled, false, 'closure cannot release cancellation before escalation');
  assert.equal(await result, event === 'error' ? 1 : 0);
  assert.deepEqual(signals.filter(signal => signal !== 0), ['SIGTERM', 'SIGKILL']);
  assert.equal(getEventListeners(controller.signal, 'abort').length, 0);
});

for (const disappearsAt of ['term', 'close', 'poll']) test(`confirmed group disappearance at ${disappearsAt} retires the target`, async t => {
  const proc = fake(), controller = new AbortController(), signals = [];
  proc.pid = 12345;
  let gone = disappearsAt === 'term';
  t.mock.method(process, 'kill', (pid, signal) => {
    assert.equal(pid, -proc.pid);
    signals.push(signal);
    if (gone) { gone = false; throw Object.assign(new Error('gone'), { code: 'ESRCH' }); }
    return true; // Simulate reuse after the first ESRCH. It must never be targeted.
  });
  const result = waitForProcess(proc, { signal: controller.signal, processGroup: true, graceMs: 100 });
  controller.abort();
  proc.emit('exit', 0);
  if (disappearsAt === 'close') gone = true;
  proc.emit('close', 0);
  if (disappearsAt === 'poll') gone = true;
  assert.equal(await result, 0);
  const retiredSignals = [...signals];
  await delay(130);
  assert.deepEqual(signals, retiredSignals);
  assert.ok(!signals.includes('SIGKILL'));
  assert.equal(getEventListeners(controller.signal, 'abort').length, 0);
});

test('normal grouped exit settles immediately without scheduling timers or probing the group', async t => {
  const proc = fake(), controller = new AbortController();
  proc.pid = 12345;
  const kill = t.mock.method(process, 'kill', () => { throw new Error('unexpected signal'); });
  const timers = t.mock.method(globalThis, 'setTimeout');
  const result = waitForProcess(proc, { signal: controller.signal, processGroup: true });
  proc.emit('exit', 7); proc.emit('close', 7);
  assert.equal(await result, 7);
  controller.abort();
  assert.equal(kill.mock.callCount(), 0);
  assert.equal(timers.mock.callCount(), 0);
});

test('cancellation kills a shell descendant even after launcher and output close', {
  skip: !['linux', 'darwin'].includes(process.platform), timeout: 5000,
}, async t => {
  const dir = await mkdtemp(join(tmpdir(), 'pi-process-'));
  const pidFile = join(dir, 'child.pid');
  // The child ignores TERM before publishing readiness and never holds our pipes.
  const proc = spawn('sh', ['-c', `sh -c 'trap "" TERM; echo $$ > "$1"; while :; do :; done' sh "$1" </dev/null >/dev/null 2>&1 & wait`, 'sh', pidFile],
    { detached: true, stdio: ['ignore', 'pipe', 'pipe'] });
  t.after(async () => {
    try { process.kill(-proc.pid, 'SIGKILL'); } catch {}
    await rm(dir, { recursive: true, force: true });
  });
  const controller = new AbortController();
  const result = waitForProcess(proc, { signal: controller.signal, processGroup: true, graceMs: 150 });
  let pid;
  for (let i = 0; i < 100 && !pid; i++) {
    pid = Number(await readFile(pidFile, 'utf8').catch(() => ''));
    if (!pid) await delay(10);
  }
  assert.ok(pid, 'descendant published readiness');
  const alive = () => {
    try { return !/^\s*Z/.test(execFileSync('ps', ['-o', 'stat=', '-p', String(pid)], { encoding: 'utf8' })); }
    catch (error) { if (error.status === 1) return false; throw error; }
  };
  assert.equal(alive(), true);
  const closed = once(proc, 'close');
  controller.abort();
  await closed;
  await result;
  for (let i = 0; i < 50 && alive(); i++) await delay(10);
  // This assertion runs BEFORE the unconditional safety cleanup above.
  assert.equal(alive(), false, 'TERM-resistant descendant must be dead before fixture cleanup');
});

test('isolated process group kills pipe-holding descendant after launcher exits', {
  skip: !['linux', 'darwin'].includes(process.platform), timeout: 5000,
}, async t => {
  const code = `const {spawn}=require('node:child_process');
    const child=spawn(process.execPath,['-e', "process.on('SIGTERM',()=>{}); console.log('ready'); setInterval(()=>{},1000)"],{stdio:['ignore','inherit','inherit']});
    process.on('SIGTERM',()=>process.exit(0));`;
  const proc = spawn(process.execPath, ['-e', code], { detached: true, stdio: ['ignore', 'pipe', 'pipe'] });
  t.after(() => { try { process.kill(-proc.pid, 'SIGKILL'); } catch {} });
  const controller = new AbortController();
  const result = waitForProcess(proc, { signal: controller.signal, processGroup: true, graceMs: 50 });
  await once(proc.stdout, 'data');
  controller.abort();
  assert.equal(await result, 0);
  assert.equal(getEventListeners(controller.signal, 'abort').length, 0);
});
