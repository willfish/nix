import assert from 'node:assert/strict';
import { chmod, mkdtemp, mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { createServer } from 'node:net';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import test from 'node:test';
import { questionModel } from './fixtures/pi-team-question-model.js';
import { HerdrPanes, socketCall } from '../home/config/pi/extensions/subagent/herdr.js';
import { shellQuote } from '../home/config/pi/extensions/subagent/team.js';

const index = fileURLToPath(new URL('../home/config/pi/extensions/subagent/index.ts', import.meta.url));
const piBin = process.env.PI_TEAM_TEST_BIN ?? 'pi';
const isolatedArgs = ['--offline', '--no-extensions', '--no-skills', '--no-prompt-templates',
  '--no-context-files', '--no-builtin-tools', '--model', 'team-question/question', '--thinking', 'off'];
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

async function fixture(t) {
  const dir = await mkdtemp(join(tmpdir(), 'pi-team-question-'));
  t.after(() => rm(dir, { recursive: true, force: true }));
  const home = join(dir, 'home'), agentDir = join(home, '.pi', 'agent'), cwd = join(dir, 'project');
  await mkdir(join(agentDir, 'agents'), { recursive: true });
  await mkdir(join(agentDir, 'extensions'));
  await mkdir(cwd);
  const server = await questionModel();
  t.after(() => server.close());
  await writeFile(join(agentDir, 'models.json'), JSON.stringify(server.models));
  await writeFile(join(agentDir, 'settings.json'), JSON.stringify({
    defaultProvider: 'team-question', defaultModel: 'question', defaultProjectTrust: 'no',
    retry: { enabled: false }, compaction: { enabled: false },
  }));
  await writeFile(join(agentDir, 'agents', 'runtime.md'),
    '---\nname: runtime\ndescription: Loopback question fixture\n---\nFollow the marked task.');
  const env = { PATH: process.env.PATH, HOME: home, XDG_CONFIG_HOME: join(home, '.config'),
    PI_CODING_AGENT_DIR: agentDir, TMPDIR: dir, PI_OFFLINE: '1', PI_TELEMETRY: '0',
    CAPTURE_PROMPTS: '0', TERM: 'xterm-256color', LANG: 'C.UTF-8' };
  return { dir, agentDir, cwd, env, server };
}

async function waitJson(path, timeoutMs = 90000, failurePath) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try { return JSON.parse(await readFile(path, 'utf8')); }
    catch (error) { if (error.code !== 'ENOENT') throw error; }
    if (failurePath) {
      try {
        const report = JSON.parse(await readFile(failurePath, 'utf8'));
        assert.fail(report.fatal ?? 'Probe finished without the expected checkpoint');
      } catch (error) { if (error.code !== 'ENOENT') throw error; }
    }
    await sleep(100);
  }
  assert.fail(`Timed out waiting for ${path}`);
}
const paneIds = node => node.type === 'pane' ? [node.pane_id] : [...paneIds(node.first), ...paneIds(node.second)];
const shape = node => node.type === 'pane' ? { pane: node.pane_id } : {
  direction: node.direction, ratio: node.ratio, first: shape(node.first), second: shape(node.second),
};
function guardedPanes() {
  assert.equal(process.env.HERDR_ENV, '1');
  assert.ok(process.env.HERDR_SOCKET_PATH && process.env.HERDR_PANE_ID, 'Run inside a real herdr pane');
  const realParent = process.env.HERDR_PANE_ID, owned = new Set();
  let siblingCreated = false;
  const raw = (method, params) => socketCall(process.env.HERDR_SOCKET_PATH, method, params);
  const call = async (method, params) => {
    if (method === 'pane.split') {
      assert.equal(params.focus, false);
      assert.ok(owned.has(params.target_pane_id) || (!siblingCreated && params.target_pane_id === realParent));
    } else if (['pane.close', 'pane.rename', 'pane.send_input'].includes(method)) {
      assert.ok(owned.has(params.pane_id), `Refusing ${method} on unowned pane`);
    } else if (method === 'layout.set_split_ratio') {
      const { layout } = await raw('layout.export', { pane_id: realParent });
      assert.equal(params.tab_id, layout.tab_id);
      let subtree = layout.root;
      for (const second of params.path) subtree = second ? subtree.second : subtree.first;
      assert.ok(paneIds(subtree).every(id => owned.has(id)), 'Refusing foreign layout edit');
    } else assert.ok(['layout.export', 'pane.get', 'pane.current'].includes(method), `Forbidden operation ${method}`);
    const result = await raw(method, params);
    if (method === 'pane.split') {
      siblingCreated = true;
      assert.ok(result.pane?.pane_id && result.pane.pane_id !== realParent);
      owned.add(result.pane.pane_id);
    }
    if (method === 'pane.close') owned.delete(params.pane_id);
    return result;
  };
  return { realParent, owned, call, layout: async () => (await call('layout.export', { pane_id: realParent })).layout };
}

// Default execution is entirely offline and never touches herdr.
test('question wire fixture only emits a tool call when requested and available', async t => {
  const server = await questionModel();
  t.after(() => server.close());
  const url = server.models.providers['team-question'].baseUrl + '/chat/completions';
  const ask = { type: 'function', function: { name: 'ask_coordinator', parameters: { type: 'object' } } };
  const request = async (content, tools) => {
    const response = await fetch(url, { method: 'POST', headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ model: 'question', stream: true, messages: [{ role: 'user', content }], tools }) });
    assert.equal(response.status, 200);
    return response.text();
  };
  assert.doesNotMatch(await request('Q_PAR', []), /tool_calls/);
  assert.doesNotMatch(await request('Q_ANSWER', [ask]), /tool_calls/);
  assert.match(await request('Q_PAR', [ask]), /tool_calls/);
  assert.equal(server.histories.length, 3);
  assert.equal(server.toolcalls.length, 1);
  assert.deepEqual(server.errors, []);
});

test('live Pi/herdr: real child question termination, resumable jobs, repeated questions and main-pane human gate', {
  skip: process.env.PI_TEAM_LIVE_TEST !== '1' && 'Set PI_TEAM_LIVE_TEST=1 inside herdr to opt in',
  timeout: 240000,
}, async t => {
  const { realParent, owned, call, layout } = guardedPanes();
  const f = await fixture(t);
  const before = await layout(), focused = (await call('pane.current', {})).pane.pane_id;
  const owner = new HerdrPanes({ parentPaneId: realParent, call });
  const output = join(f.dir, 'report.json'), checkpoint = join(f.dir, 'waiting.json');
  const proceed = join(f.dir, 'proceed.json'), stopped = join(f.dir, 'stopped.json');
  const trace = join(f.dir, 'trace.jsonl'), extension = join(f.dir, 'probe.ts');
  const socketPath = join(f.dir, 'guard.sock'), violations = [], connections = new Set(), children = [];
  let temporaryParent;
  const command = (paneId, text, child = false) => `exec env -i ${Object.entries({ ...f.env,
    HERDR_ENV: '1', HERDR_SOCKET_PATH: socketPath, HERDR_PANE_ID: paneId,
    ...(child ? { PI_TEAM_CHILD: '1' } : {}),
  }).map(([key, value]) => shellQuote(`${key}=${value}`)).join(' ')} ${text}`;
  // Keep the actual index, manager, bridge and Pi tool pipeline. Only mediate pane ownership
  // and strip inherited credentials from the real child launch command.
  const proxy = createServer(socket => {
    connections.add(socket);
    socket.on('close', () => connections.delete(socket));
    socket.on('error', () => {});
    let buffer = '';
    socket.on('data', async chunk => {
      buffer += chunk;
      if (!buffer.includes('\n')) return;
      socket.removeAllListeners('data');
      let request;
      try {
        request = JSON.parse(buffer.slice(0, buffer.indexOf('\n')));
        const { method } = request;
        let { params } = request;
        if (method === 'pane.send_input') {
          assert.ok(owned.has(params.pane_id) && params.pane_id !== temporaryParent);
          assert.ok(params.text.startsWith("'exec' "), 'Expected actual TeamManager launch');
          params = { ...params, text: command(params.pane_id, params.text.slice(7), true) };
        }
        const result = await call(method, params);
        if (method === 'pane.split') children.push(result.pane.pane_id);
        socket.end(JSON.stringify({ id: request.id, result }) + '\n');
      } catch (error) {
        violations.push(error.stack ?? String(error));
        socket.end(JSON.stringify({ id: request?.id, error: { code: 'probe_guard', message: error.message } }) + '\n');
      }
    });
  });
  const closeProxy = async () => {
    for (const socket of connections) socket.destroy();
    if (proxy.listening) await new Promise(resolve => proxy.close(resolve));
  };
  t.after(closeProxy);
  await new Promise((resolve, reject) => { proxy.once('error', reject); proxy.listen(socketPath, resolve); });
  await chmod(socketPath, 0o600);
  await writeFile(join(f.agentDir, 'extensions', 'observe.ts'), `
    import { appendFileSync } from 'node:fs';
    export default function(pi) {
      const record = (kind, event, ctx) => appendFileSync(${JSON.stringify(trace)}, JSON.stringify({
        kind, event, mode: ctx.mode, pane: process.env.HERDR_PANE_ID, pid: process.pid,
        sessionId: ctx.sessionManager.getSessionId(), active: pi.getActiveTools(), idle: ctx.isIdle(),
      }) + '\\n');
      for (const kind of ['session_start', 'message_end', 'agent_settled']) pi.on(kind, (event, ctx) => record(kind, event, ctx));
    }
  `);
  await writeFile(extension, `
    import install from ${JSON.stringify(index)};
    import assert from 'node:assert/strict';
    import { writeFileSync, existsSync } from 'node:fs';
    export default function(pi) {
      const tools = new Map();
      install(new Proxy(pi, { get(target, key) {
        if (key === 'registerTool') return tool => { tools.set(tool.name, tool); target.registerTool(tool); };
        return target[key];
      }}));
      pi.on('session_shutdown', () => writeFileSync(${JSON.stringify(stopped)}, JSON.stringify({ shutdown: true })));
      pi.registerCommand('probe', { handler: async (_args, ctx) => {
        const report = { mode: ctx.mode, pane: process.env.HERDR_PANE_ID, ui: [] };
        const invoke = (name, params, context = ctx) => tools.get(name).execute('question-probe', params, undefined, undefined, context);
        const team = async (params, context) => JSON.parse((await invoke('team', params, context)).content[0].text);
        const run = async params => (await invoke('subagent', params)).details.job;
        const wait = id => team({ action: 'wait', id });
        const reject = async (params, pattern) => assert.rejects(() => team(params), pattern);
        const close = () => team({ action: 'close', id: 'all' });
        const pending = job => {
          assert.equal(job.status, 'waiting_question');
          const result = job.tasks.find(task => task.state === 'waiting_question').result;
          assert.ok(result.question.id && result.question.commandId && result.sessionId && result.memberId);
          return result;
        };
        const answer = (id, text) => team({ action: 'answer', id, text });
        try {
          assert.equal(ctx.mode, 'tui');
          assert.notEqual(process.env.PI_TEAM_CHILD, '1');
          report.parallel = await run({ tasks: [{ agent: 'runtime', task: 'Q_PAR' }, { agent: 'runtime', task: 'Q_SLOW' }] });
          report.returnedAt = Date.now();
          const first = pending(report.parallel);
          assert.equal(report.parallel.tasks[1].state, 'running', 'Question must return before slow sibling settles');
          report.first = await team({ action: 'read', id: first.memberId });
          writeFileSync(${JSON.stringify(checkpoint)}, JSON.stringify(report));
          const deadline = Date.now() + 20000;
          while (!existsSync(${JSON.stringify(proceed)})) {
            assert.ok(Date.now() < deadline, 'Outer termination verification timed out');
            await new Promise(resolve => setTimeout(resolve, 50));
          }
          const ack = await answer(first.question.id, 'Q_ANSWER');
          assert.equal(ack.status, 'queued');
          assert.deepEqual(await answer(first.question.id, 'Q_ANSWER'), ack, 'Duplicate answer must be idempotent');
          await reject({ action: 'answer', id: first.question.id, text: 'different' }, /already answered|stale/i);
          await reject({ action: 'answer', id: 'missing-question', text: 'no' }, /stale/i);
          report.parallelFinal = await wait(report.parallel.jobId);
          assert.equal(report.parallelFinal.status, 'completed');
          const final = report.parallelFinal.tasks[0].result;
          assert.equal(final.text, 'final:Q_ANSWER; user-turns:2');
          for (const key of ['memberId', 'paneId', 'sessionId', 'sessionFile']) assert.equal(final[key], first[key]);
          report.parallelReadFinal = await team({ action: 'read', id: first.memberId });
          assert.notEqual(report.parallelReadFinal.commandId, first.question.commandId);
          assert.equal((await team({ action: 'questions' })).members.length, 0);
          await close();

          report.chain = await run({ chain: [{ agent: 'runtime', task: 'Q_CHAIN' },
            { agent: 'runtime', task: 'Q_NEXT previous=[{previous}]' }] });
          const chainQuestion = pending(report.chain);
          assert.equal(report.chain.tasks[1].state, 'queued');
          await answer(chainQuestion.question.id, 'Q_CHAIN_ANSWER');
          report.chainFinal = await wait(report.chain.jobId);
          assert.equal(report.chainFinal.status, 'completed');
          assert.equal(report.chainFinal.tasks[0].result.text, 'final:Q_CHAIN_ANSWER; user-turns:2');
          await close();

          report.repeat = await run({ agent: 'runtime', task: 'Q_REPEAT' });
          const repeatOne = pending(report.repeat);
          await answer(repeatOne.question.id, 'Q_AGAIN');
          report.repeatSecond = await wait(report.repeat.jobId);
          const repeatTwo = pending(report.repeatSecond);
          assert.notEqual(repeatTwo.question.id, repeatOne.question.id);
          assert.notEqual(repeatTwo.question.commandId, repeatOne.question.commandId);
          assert.equal(repeatTwo.sessionId, repeatOne.sessionId);
          await answer(repeatTwo.question.id, 'Q_REPEAT_ANSWER');
          report.repeatFinal = await wait(report.repeat.jobId);
          assert.equal(report.repeatFinal.status, 'completed');
          assert.equal(report.repeatFinal.tasks[0].result.text, 'final:Q_REPEAT_ANSWER; user-turns:3');
          await close();

          report.human = await run({ agent: 'runtime', task: 'Q_HUMAN' });
          const human = pending(report.human);
          assert.equal(human.question.requiresUser, true);
          await reject({ action: 'answer', id: human.question.id, text: 'Approve', source: 'human' }, /human answer/);
          const uiContext = (select, input) => ({ ...ctx, ui: { ...ctx.ui,
            select: async (title, choices) => { report.ui.push({ kind: 'select', title, choices, pane: process.env.HERDR_PANE_ID }); return select; },
            input: async title => { report.ui.push({ kind: 'input', title, pane: process.env.HERDR_PANE_ID }); return input; },
          } });
          report.dismissed = await team({ action: 'ask', id: human.question.id }, uiContext(undefined));
          assert.equal(report.dismissed.status, 'waiting_question');
          assert.equal((await team({ action: 'questions' })).members[0].id, human.question.id);
          await new Promise(resolve => setTimeout(resolve, 200));
          assert.equal(report.ui.length, 1, 'Dismissed dialog must not automatically reopen');
          report.humanAck = await team({ action: 'ask', id: human.question.id }, uiContext('Other answer (type text)', 'Q_HUMAN_ANSWER'));
          assert.equal(report.humanAck.status, 'queued');
          report.humanFinal = await wait(report.human.jobId);
          assert.equal(report.humanFinal.status, 'completed');
          assert.equal(report.humanFinal.tasks[0].result.text, 'final:Q_HUMAN_ANSWER; user-turns:2');
          await close();

          report.cancel = await run({ agent: 'runtime', task: 'Q_CANCEL' });
          const cancelled = pending(report.cancel);
          assert.equal((await team({ action: 'cancel', id: report.cancel.jobId })).status, 'aborted');
          await reject({ action: 'answer', id: cancelled.question.id, text: 'late' }, /stale/i);
        } catch (error) { report.fatal = error.stack ?? String(error); }
        finally {
          writeFileSync(${JSON.stringify(output)}, JSON.stringify(report));
          ctx.shutdown();
        }
      }});
    }
  `);
  try {
    temporaryParent = await owner.open(f.cwd, {}, 'pi-team questions (temporary)');
    const baseline = await layout();
    const args = [...isolatedArgs, '--session-dir', join(f.dir, 'parent-sessions'), '-e', extension, '/probe'];
    await call('pane.send_input', { pane_id: temporaryParent,
      text: command(temporaryParent, [piBin, ...args].map(shellQuote).join(' ')), keys: ['Enter'] });
    const waiting = await waitJson(checkpoint, 90000, output);
    assert.equal(waiting.parallel.status, 'waiting_question');
    // Hold both the sibling and coordinator answer. A spurious model turn is observable
    // here, rather than being hidden by a fast answer or by direct hook invocation.
    await sleep(800);
    const childHistories = () => f.server.histories.filter(history =>
      !(history.tools ?? []).some(tool => ['subagent', 'team'].includes(tool.function.name)));
    assert.equal(childHistories().length, 2, 'No extra provider turn after terminate:true');
    assert.deepEqual(f.server.toolcalls.map(call => call.marker), ['Q_PAR']);
    assert.ok(waiting.returnedAt - f.server.toolcalls[0].at < 5000,
      'Parent must yield promptly after the child asks, without waiting for the held sibling');
    assert.equal(waiting.first.commandId, waiting.parallel.tasks[0].result.question.commandId);
    const observation = (await readFile(trace, 'utf8')).trim().split('\n').map(JSON.parse);
    const settled = observation.find(row => row.kind === 'agent_settled' && row.sessionId === waiting.first.sessionId);
    assert.ok(settled?.idle, 'Real Pi must settle the terminating tool segment');
    assert.ok(!settled.active.includes('ask_coordinator'), 'Question tool is inactive while waiting');
    assert.ok(observation.some(row => row.kind === 'message_end' && row.event.message?.role === 'toolResult'),
      'Question must traverse the real tool pipeline');
    f.server.releaseSlow();
    await writeFile(proceed, '{}');
    const report = await waitJson(output, 120000);
    assert.equal(report.fatal, undefined);
    assert.equal(report.mode, 'tui');
    assert.equal(report.pane, temporaryParent);
    assert.deepEqual(report.ui.map(item => item.kind), ['select', 'select', 'input']);
    assert.ok(report.ui.every(item => item.pane === temporaryParent));
    assert.deepEqual(report.ui[0].choices, ['Approve', 'Decline', 'Other answer (type text)']);
    assert.match(report.ui[0].title, /runtime: Clarify Q_HUMAN/);
    assert.deepEqual(f.server.toolcalls.map(call => call.marker), ['Q_PAR', 'Q_CHAIN', 'Q_REPEAT', 'Q_AGAIN', 'Q_HUMAN', 'Q_CANCEL']);
    const histories = childHistories();
    assert.equal(histories.length, 12, 'One request per prompt/answer, none after a terminating question');
    assert.ok(histories.every(history => history.tools.some(tool => tool.function.name === 'ask_coordinator')),
      'Dynamic question tool must be active again for every new command and answer');
    assert.ok(histories.every(history => !history.tools.some(tool => ['subagent', 'team'].includes(tool.function.name))));
    const next = histories.find(history => JSON.stringify(history.messages.filter(message => message.role === 'user').at(-1)?.content).includes('Q_NEXT'));
    assert.ok(next, 'Chain successor actually reached the provider');
    const nextUser = JSON.stringify(next.messages.filter(message => message.role === 'user').at(-1).content);
    assert.match(nextUser, /previous=\[final:Q_CHAIN_ANSWER; user-turns:2\]/);
    assert.doesNotMatch(nextUser, /Clarify|Waiting for coordinator|\{previous\}/);
    const resumed = histories.find(history => history.messages.some(message => message.role === 'user' && JSON.stringify(message.content).includes('Q_REPEAT_ANSWER')));
    assert.equal(resumed.messages.filter(message => message.role === 'user').length, 3);
    assert.equal(resumed.messages.filter(message => message.role === 'tool').length, 2);
    assert.deepEqual(f.server.errors, []);
    assert.deepEqual(await waitJson(stopped, 15000), { shutdown: true });
    assert.deepEqual([...owned], [temporaryParent], 'Index shutdown closes all owned children');
    assert.equal(new Set(children).size, 7, 'Only the expected retained child sessions were launched');
    const current = await layout();
    // The temporary coordinator can disappear after its acknowledged shutdown.
    const expected = paneIds(current.root).includes(temporaryParent) ? baseline : before;
    assert.deepEqual(shape(current.root), shape(expected.root));
    assert.equal(current.focused_pane_id, before.focused_pane_id);
    assert.equal((await call('pane.current', {})).pane.pane_id, focused);
    assert.deepEqual(violations, []);
  } finally {
    f.server.releaseSlow();
    // If an outer assertion failed at the waiting checkpoint, let the probe leave its
    // barrier and run its own ctx.shutdown before resorting to pane cleanup.
    await writeFile(proceed, '{}');
    if (temporaryParent) {
      try { await waitJson(stopped, 15000); }
      catch (error) { t.diagnostic(`Cooperative shutdown did not finish: ${error.message}`); }
    }
    const errors = [];
    // Child panes first, then the temporary coordinator; never touch foreign panes.
    for (const id of [...owned].reverse()) {
      try { await call('pane.close', { pane_id: id }); }
      catch (error) { if (error.code !== 'pane_not_found') errors.push(error); }
    }
    await closeProxy();
    const after = await layout();
    assert.deepEqual(shape(after.root), shape(before.root));
    assert.equal(after.focused_pane_id, before.focused_pane_id);
    assert.equal((await call('pane.current', {})).pane.pane_id, focused);
    assert.equal(after.tab_id, before.tab_id);
    assert.equal(after.workspace_id, before.workspace_id);
    assert.deepEqual(errors, [], 'Owned cleanup must succeed');
  }
});
