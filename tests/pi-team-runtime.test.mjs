import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { chmod, mkdtemp, mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { createServer } from 'node:net';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import test from 'node:test';
import { smokeModel } from './fixtures/pi-team-smoke-model.js';
import { HerdrPanes, socketCall } from '../home/config/pi/extensions/subagent/herdr.js';
import { TeamManager, shellQuote } from '../home/config/pi/extensions/subagent/team.js';
import { withSkills } from '../home/config/pi/extensions/subagent/skills.js';

const index = fileURLToPath(new URL('../home/config/pi/extensions/subagent/index.ts', import.meta.url));
const piBin = process.env.PI_TEAM_TEST_BIN ?? 'pi';
const isolatedArgs = ['--offline', '--no-extensions', '--no-skills', '--no-prompt-templates',
  '--no-context-files', '--no-builtin-tools', '--model', 'team-smoke/echo', '--thinking', 'off'];
const skillBody = 'SMOKE_SKILL_SYSTEM_CONTENT\nResolve references/check.md before writing code.\nEND_SMOKE_SKILL';

async function fixture(t) {
  const dir = await mkdtemp(join(tmpdir(), 'pi-team-runtime-'));
  t.after(() => rm(dir, { recursive: true, force: true }));
  const home = join(dir, 'home');
  const agentDir = join(home, '.pi', 'agent');
  const cwd = join(dir, 'project');
  await mkdir(join(agentDir, 'agents'), { recursive: true });
  await mkdir(cwd);
  const server = await smokeModel();
  t.after(() => server.close());
  await writeFile(join(agentDir, 'models.json'), JSON.stringify(server.models));
  await writeFile(join(agentDir, 'settings.json'), JSON.stringify({
    defaultProvider: 'team-smoke', defaultModel: 'echo', enableSkillCommands: true,
    defaultProjectTrust: 'no', retry: { enabled: false }, compaction: { enabled: false },
  }));
  const skill = join(dir, 'SKILL.md');
  await writeFile(skill, `---\nname: runtime-skill\ndescription: Runtime fixture\n---\n${skillBody}\n`);
  await writeFile(join(agentDir, 'agents', 'runtime.md'),
    '---\nname: runtime\ndescription: Offline runtime persona\nskills: [runtime-skill]\n---\nSMOKE_PERSONA_SYSTEM_CONTENT');
  // Whitelist rather than inherit cloud credentials, proxy settings or the real user config.
  const env = { PATH: process.env.PATH, HOME: home, XDG_CONFIG_HOME: join(home, '.config'),
    PI_CODING_AGENT_DIR: agentDir, PI_OFFLINE: '1', PI_TELEMETRY: '0', CAPTURE_PROMPTS: '0',
    TERM: 'xterm-256color', LANG: 'C.UTF-8' };
  return { dir, home, agentDir, cwd, skill, env, server };
}

async function cli(f, args, extraEnv = {}) {
  return new Promise((resolve, reject) => {
    const child = spawn(piBin, args, { cwd: f.cwd, env: { ...f.env, ...extraEnv }, stdio: ['ignore', 'pipe', 'pipe'] });
    let stdout = '', stderr = '';
    const timer = setTimeout(() => child.kill('SIGKILL'), 45000);
    child.stdout.on('data', data => { stdout += data; });
    child.stderr.on('data', data => { stderr += data; });
    child.on('error', error => { clearTimeout(timer); reject(error); });
    child.on('close', (code, signal) => {
      clearTimeout(timer);
      try {
        assert.equal(code, 0, `Pi failed (${signal ?? 'exit'}): ${stderr}\n${stdout}`);
        assert.doesNotMatch(stderr + stdout, /Failed to load extension|Extension errors/i);
        resolve({ stdout, stderr });
      } catch (error) { reject(error); }
    });
  });
}

async function probe(f, child = false) {
  const output = join(f.dir, child ? 'child-result.json' : 'root-result.json');
  const extension = join(f.dir, 'probe.ts');
  const launchesFile = join(f.dir, child ? 'child-launches.jsonl' : 'root-launches.jsonl');
  await mkdir(join(f.agentDir, 'extensions'), { recursive: true });
  // The parent uses --no-extensions; only a real fallback child discovers this fixture.
  await writeFile(join(f.agentDir, 'extensions', 'record-start.ts'), `
    import { appendFileSync } from 'node:fs';
    export default function(pi) {
      pi.on('session_start', (_event, ctx) => {
        appendFileSync(${JSON.stringify(launchesFile)}, JSON.stringify({ pid: process.pid, mode: ctx.mode }) + '\\n');
      });
    }
  `);
  await writeFile(extension, `
    import install from ${JSON.stringify(index)};
    import { writeFileSync, readFileSync, existsSync } from 'node:fs';
    export default function(pi) {
      const tools = new Map();
      install(new Proxy(pi, { get(target, key) {
        if (key === 'registerTool') return tool => { tools.set(tool.name, tool); target.registerTool(tool); };
        return target[key];
      }}));
      pi.registerCommand('runtime-probe', { handler: async (_args, ctx) => {
        const report = { mode: ctx.mode, tools: pi.getAllTools().filter(t => tools.has(t.name)) };
        const invoke = (name, params) => tools.get(name).execute('runtime-check', params, undefined, undefined, ctx);
        const failure = async (name, params) => {
          try { return { unexpected: await invoke(name, params) }; }
          catch (error) { return { error: error.message }; }
        };
        const launches = () => existsSync(${JSON.stringify(launchesFile)})
          ? readFileSync(${JSON.stringify(launchesFile)}, 'utf8').trim().split('\\n').map(line => JSON.parse(line)) : [];
        try {
          report.team = await failure('team', { action: 'list' });
          const task = { agent: 'runtime', task: 'SMOKE_FALLBACK', skills: ['missing-runtime-skill'] };
          report.rejected = await failure('subagent', task);
          report.launchesBeforeFallback = launches();
          if (process.env.PI_TEAM_CHILD !== '1') {
            report.fallback = await invoke('subagent', { ...task, skills: [] });
          }
          report.launches = launches();
        } catch (error) { report.fatal = error.message; }
        writeFileSync(${JSON.stringify(output)}, JSON.stringify(report));
      }});
    }
  `);
  await cli(f, [...isolatedArgs, '--skill', f.skill, '--no-session', '-e', extension, '-p', '/runtime-probe'],
    child ? { PI_TEAM_CHILD: '1', HERDR_ENV: '1', HERDR_SOCKET_PATH: join(f.dir, 'must-not-connect.sock'), HERDR_PANE_ID: 'forbidden-parent' } : {});
  return JSON.parse(await readFile(output, 'utf8'));
}

function assertSchemas(report) {
  assert.equal(report.fatal, undefined);
  assert.deepEqual(report.tools.map(tool => tool.name).sort(), ['subagent', 'team']);
  const description = report.tools.find(tool => tool.name === 'subagent').description;
  assert.match(description, /Default to working solo/);
  assert.match(description, /at most one general reviewer/);
  assert.match(description, /Never launch every role or a fixed pipeline/);
  const schemas = Object.fromEntries(report.tools.map(tool => [tool.name, tool.parameters]));
  assert.equal(schemas.subagent.type, 'object');
  for (const schema of [schemas.subagent, schemas.subagent.properties.tasks.items, schemas.subagent.properties.chain.items]) {
    assert.equal(schema.properties.skills.type, 'array');
    assert.equal(schema.properties.skills.items.type, 'string');
  }
  assert.deepEqual(schemas.team.required, ['action']);
  assert.deepEqual(schemas.team.properties.action.enum, ['list', 'read', 'send', 'steer', 'close']);
}

function assertSkillHistory(history, skill) {
  const system = history.messages.filter(message => ['system', 'developer'].includes(message.role))
    .map(message => message.content).join('\n');
  assert.ok(system.includes(skillBody), 'Full skill body must reach the provider system prompt');
  assert.ok(system.includes(`Skill file: ${skill}`));
  assert.ok(system.includes('SMOKE_PERSONA_SYSTEM_CONTENT'));
}

test('real Pi full index registers schemas, rejects missing skills before spawn, and uses JSON fallback outside herdr', { timeout: 60000 }, async t => {
  const f = await fixture(t);
  const report = await probe(f);
  assertSchemas(report);
  assert.equal(report.mode, 'print');
  assert.match(report.team.error, /root Pi session inside herdr/);
  assert.match(report.rejected.error, /missing-runtime-skill.*active Pi skills/);
  assert.deepEqual(report.launchesBeforeFallback, []);
  assert.equal(report.launches.length, 1, 'The fallback must really launch a child Pi CLI');
  assert.equal(report.launches[0].mode, 'json');
  assert.ok(report.launches[0].pid > 0);
  assert.equal(report.fallback.details.mode, 'single');
  const [result] = report.fallback.details.results;
  assert.equal(result.exitCode, 0);
  assert.equal(result.model, 'team-smoke/echo');
  assert.ok(result.messages.some(message => message.role === 'assistant' && message.stopReason === 'stop'));
  assert.match(report.fallback.content[0].text, /echo:SMOKE_FALLBACK; user-turns:1/);
  assert.equal(f.server.histories.length, 1);
  assertSkillHistory(f.server.histories[0], f.skill);
});

test('real Pi full index refuses recursive delegation in a team child before discovery or launch', { timeout: 60000 }, async t => {
  const f = await fixture(t);
  const report = await probe(f, true);
  assertSchemas(report);
  assert.match(report.rejected.error, /recursive delegation is disabled/);
  assert.match(report.team.error, /root Pi session inside herdr/);
  assert.deepEqual(report.launches, []);
  assert.equal(f.server.histories.length, 0);
});

test('normal Pi CLI loads the actual index and serializes both tools to a loopback model', { timeout: 60000 }, async t => {
  const f = await fixture(t);
  const result = await cli(f, [...isolatedArgs, '--no-session', '-e', index, '--mode', 'json', '-p', 'SMOKE_NORMAL_CLI']);
  const events = result.stdout.split('\n').filter(line => line.startsWith('{')).map(line => JSON.parse(line));
  const assistant = events.find(event => event.type === 'message_end' && event.message.role === 'assistant')?.message;
  assert.equal(assistant?.stopReason, 'stop');
  assert.equal(assistant.content[0].text, 'echo:SMOKE_NORMAL_CLI; user-turns:1');
  assert.equal(f.server.histories.length, 1);
  assert.deepEqual(f.server.histories[0].tools.map(tool => tool.function.name).sort(), ['subagent', 'team']);
});

function paneIds(node) {
  return node.type === 'pane' ? [node.pane_id] : [...paneIds(node.first), ...paneIds(node.second)];
}
function layoutShape(node) {
  return node.type === 'pane' ? { pane: node.pane_id } : {
    direction: node.direction, ratio: node.ratio,
    first: layoutShape(node.first), second: layoutShape(node.second),
  };
}
function parentOf(node, paneId) {
  if (node.type !== 'split') return undefined;
  if ([node.first, node.second].some(child => child.type === 'pane' && child.pane_id === paneId)) return node;
  return parentOf(node.first, paneId) ?? parentOf(node.second, paneId);
}
function assertBalanced(node, owned) {
  if (node.type === 'pane') { assert.ok(owned.has(node.pane_id)); return 1; }
  assert.equal(node.direction, 'down');
  const first = assertBalanced(node.first, owned), second = assertBalanced(node.second, owned);
  assert.ok(Math.abs(node.ratio - first / (first + second)) < 1e-5, 'Equal child heights');
  return first + second;
}

async function waitForJson(path, timeoutMs = 90000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try { return JSON.parse(await readFile(path, 'utf8')); }
    catch (error) { if (error.code !== 'ENOENT') throw error; }
    await new Promise(resolve => setTimeout(resolve, 100));
  }
  assert.fail(`Timed out waiting for ${path}`);
}

function guardedLivePanes() {
  assert.equal(process.env.HERDR_ENV, '1');
  assert.ok(process.env.HERDR_SOCKET_PATH && process.env.HERDR_PANE_ID, 'A real herdr parent is required');
  const realParent = process.env.HERDR_PANE_ID;
  const owned = new Set();
  let siblingCreated = false;
  const rawCall = (method, params) => socketCall(process.env.HERDR_SOCKET_PATH, method, params);
  // Fail before a buggy implementation can mutate any pre-existing pane or layout branch.
  const call = async (method, params) => {
    if (method === 'pane.split') {
      assert.equal(params.focus, false);
      assert.ok(owned.has(params.target_pane_id) || (!siblingCreated && params.target_pane_id === realParent));
    } else if (['pane.close', 'pane.rename', 'pane.send_input'].includes(method)) {
      assert.ok(owned.has(params.pane_id), `Refusing ${method} on an unowned pane`);
    } else if (method === 'layout.set_split_ratio') {
      const { layout } = await rawCall('layout.export', { pane_id: realParent });
      assert.equal(params.tab_id, layout.tab_id);
      let subtree = layout.root;
      for (const second of params.path) subtree = second ? subtree.second : subtree.first;
      assert.ok(paneIds(subtree).every(id => owned.has(id)), 'Refusing a ratio edit containing foreign panes');
    } else {
      assert.ok(['layout.export', 'pane.get', 'pane.current'].includes(method), `Forbidden live operation: ${method}`);
    }
    const result = await rawCall(method, params);
    if (method === 'pane.split') {
      siblingCreated = true;
      assert.ok(result.pane?.pane_id && result.pane.pane_id !== realParent);
      owned.add(result.pane.pane_id);
    }
    if (method === 'pane.close') owned.delete(params.pane_id);
    return result;
  };
  const layout = async () => (await call('layout.export', { pane_id: realParent })).layout;
  return { realParent, owned, call, layout };
}

// Run manually from the existing herdr pane. Never creates tabs/workspaces or focuses a pane.
test('live herdr: retained interactive children preserve context, focus and parent ratios', {
  skip: process.env.PI_TEAM_LIVE_TEST !== '1' && 'Set PI_TEAM_LIVE_TEST=1 inside herdr to opt in', timeout: 240000,
}, async t => {
  const { realParent, call, layout } = guardedLivePanes();
  const f = await fixture(t);
  const before = await layout();
  const focused = (await call('pane.current', {})).pane.pane_id;
  assert.ok(paneIds(before.root).includes(realParent));
  const owner = new HerdrPanes({ parentPaneId: realParent, call });
  let manager;
  let temporaryParent;
  try {
    temporaryParent = await owner.open(f.cwd, {}, 'pi-team smoke (temporary)');
    const baseline = await layout();
    const mainRatio = parentOf(baseline.root, realParent).ratio;
    assert.equal(baseline.focused_pane_id, before.focused_pane_id);
    manager = new TeamManager({ panes: new HerdrPanes({ parentPaneId: temporaryParent, call }),
      root: f.dir, startupMs: 45000, taskMs: 45000, pollMs: 100 });
    const prompt = await withSkills({ name: 'runtime', systemPrompt: 'SMOKE_PERSONA_SYSTEM_CONTENT', skills: ['runtime-skill'] },
      [], [{ name: 'runtime-skill', filePath: f.skill }]);
    const promptFile = join(f.dir, 'system.txt');
    await writeFile(promptFile, prompt);
    // Strip shell-inherited credentials while keeping only the newly-created pane identity.
    const launcher = join(f.dir, 'pi-isolated');
    await writeFile(launcher, `#!/bin/sh\nexec env -i ${Object.entries(f.env).map(([key, value]) => shellQuote(`${key}=${value}`)).join(' ')} HERDR_ENV=1 HERDR_SOCKET_PATH=${shellQuote(process.env.HERDR_SOCKET_PATH)} HERDR_PANE_ID="$HERDR_PANE_ID" PI_TEAM_CHILD=1 ${shellQuote(piBin)} "$@"\n`);
    await chmod(launcher, 0o700);
    const results = [];
    let teamRatio;
    for (let i = 0; i < 3; i++) {
      const result = await manager.run({ agent: `runtime-${i}`, task: `SMOKE_LIVE_${i}`, cwd: f.cwd,
        invocation: bridge => ({ command: launcher, args: [...isolatedArgs, '--session-dir', join(f.dir, 'sessions'),
          '--append-system-prompt', promptFile, '-e', index, ...bridge] }) });
      assert.equal(result.status, 'completed');
      assert.equal(result.text, `echo:SMOKE_LIVE_${i}; user-turns:1`);
      assert.ok(result.commandId && result.sessionId && result.sessionFile && result.memberId && result.paneId);
      assert.ok(result.messages.some(message => message.role === 'assistant'));
      results.push(result);
      const current = await layout();
      assert.equal(current.focused_pane_id, before.focused_pane_id);
      assert.equal((await call('pane.current', {})).pane.pane_id, focused);
      assert.equal(parentOf(current.root, realParent).ratio, mainRatio);
      const split = parentOf(current.root, temporaryParent);
      teamRatio ??= split.ratio;
      assert.equal(split.ratio, teamRatio, 'Stable coordinator/children width ratio');
      assert.equal(split.direction, 'right');
      assert.equal(assertBalanced(split.second, manager.panes.owned), i + 1);
    }
    assert.equal(new Set(results.map(result => result.sessionId)).size, 3);
    const follow = await manager.send(results[0].memberId, 'SMOKE_FOLLOWUP');
    assert.equal(follow.status, 'completed');
    assert.equal(follow.text, 'echo:SMOKE_FOLLOWUP; user-turns:2');
    for (const key of ['memberId', 'paneId', 'sessionId', 'sessionFile']) assert.equal(follow[key], results[0][key]);
    assert.notEqual(follow.commandId, results[0].commandId);
    assert.equal((await manager.read(results[0].memberId)).text, follow.text);
    assert.equal(f.server.histories.length, 4);
    for (const history of f.server.histories) {
      assertSkillHistory(history, f.skill);
      assert.ok(!(history.tools ?? []).some(tool => ['subagent', 'team'].includes(tool.function.name)),
        'Interactive child bridge must disable recursive delegation tools');
    }
    const history = f.server.histories.at(-1).messages;
    assert.ok(history.some(message => message.role === 'user' && JSON.stringify(message.content).includes('SMOKE_LIVE_0')));
    assert.ok(history.some(message => message.role === 'assistant' && JSON.stringify(message.content).includes('echo:SMOKE_LIVE_0')));
    assert.ok(history.some(message => message.role === 'user' && JSON.stringify(message.content).includes('SMOKE_FOLLOWUP')));
  } finally {
    // Finish child cleanup before removing their owned coordinator pane, even on assertion failures.
    try { if (manager) await manager.shutdown(); }
    finally { for (const id of [...owner.owned]) await owner.close(id); }
  }
  const after = await layout();
  assert.deepEqual(layoutShape(after.root), layoutShape(before.root));
  assert.equal(after.focused_pane_id, before.focused_pane_id);
  assert.equal((await call('pane.current', {})).pane.pane_id, focused);
  assert.equal(after.tab_id, before.tab_id);
  assert.equal(after.workspace_id, before.workspace_id);
});

test('live herdr: full index interactive parent delegates skills and sends to the returned retained member', {
  skip: process.env.PI_TEAM_LIVE_TEST !== '1' && 'Set PI_TEAM_LIVE_TEST=1 inside herdr to opt in', timeout: 180000,
}, async t => {
  const { realParent, owned, call, layout } = guardedLivePanes();
  const f = await fixture(t);
  const before = await layout();
  const focused = (await call('pane.current', {})).pane.pane_id;
  const owner = new HerdrPanes({ parentPaneId: realParent, call });
  const output = join(f.dir, 'interactive-report.json');
  const stopped = join(f.dir, 'interactive-stopped.json');
  const launches = join(f.dir, 'interactive-launches.jsonl');
  const extension = join(f.dir, 'interactive-probe.ts');
  const socketPath = join(f.dir, 'guard.sock');
  const violations = [];
  const childPanes = [];
  const connections = new Set();
  let temporaryParent;
  // The actual index constructs its own TeamManager. Gate its socket traffic rather
  // than replace the manager, discovery, skill compilation, or Pi invocation.
  const isolatedCommand = (paneId, command, child = false) => {
    const env = { ...f.env, TMPDIR: f.dir, HERDR_ENV: '1', HERDR_SOCKET_PATH: socketPath,
      HERDR_PANE_ID: paneId, ...(child ? { PI_TEAM_CHILD: '1' } : {}) };
    return `exec env -i ${Object.entries(env).map(([key, value]) => shellQuote(`${key}=${value}`)).join(' ')} ${command}`;
  };
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
          assert.ok(params.text.startsWith("'exec' "), 'Expected real TeamManager launch command');
          params = { ...params, text: isolatedCommand(params.pane_id, params.text.slice(7), true) };
        }
        const result = await call(method, params);
        if (method === 'pane.split') childPanes.push(result.pane.pane_id);
        socket.end(JSON.stringify({ id: request.id, result }) + '\n');
      } catch (error) {
        violations.push(error.stack ?? String(error));
        socket.end(JSON.stringify({ id: request?.id, error: { code: 'probe_guard', message: error.message } }) + '\n');
      }
    });
  });
  // Also release the socket if fixture setup fails before any pane is opened.
  t.after(async () => {
    for (const socket of connections) socket.destroy();
    if (proxy.listening) await new Promise(resolve => proxy.close(resolve));
  });
  await new Promise((resolve, reject) => { proxy.once('error', reject); proxy.listen(socketPath, resolve); });
  await chmod(socketPath, 0o600);
  await mkdir(join(f.agentDir, 'extensions'), { recursive: true });
  await writeFile(join(f.agentDir, 'extensions', 'identity.ts'), `
    import { appendFileSync } from 'node:fs';
    export default function(pi) {
      pi.on('session_start', (_event, ctx) => {
        appendFileSync(${JSON.stringify(launches)}, JSON.stringify({ mode: ctx.mode,
          pane: process.env.HERDR_PANE_ID, child: process.env.PI_TEAM_CHILD, pid: process.pid }) + '\\n');
      });
    }
  `);
  await writeFile(extension, `
    import install from ${JSON.stringify(index)};
    import assert from 'node:assert/strict';
    import { writeFileSync } from 'node:fs';
    export default function(pi) {
      const tools = new Map();
      install(new Proxy(pi, { get(target, key) {
        if (key === 'registerTool') return tool => { tools.set(tool.name, tool); target.registerTool(tool); };
        return target[key];
      }}));
      pi.on('session_shutdown', () => writeFileSync(${JSON.stringify(stopped)}, JSON.stringify({ shutdown: true })));
      pi.registerCommand('probe', { handler: async (_args, ctx) => {
        const report = { mode: ctx.mode, pane: process.env.HERDR_PANE_ID,
          child: process.env.PI_TEAM_CHILD, pid: process.pid };
        const invoke = (name, params) => tools.get(name).execute('interactive-probe', params, undefined, undefined, ctx);
        const team = async params => JSON.parse((await invoke('team', params)).content[0].text);
        try {
          assert.equal(ctx.mode, 'tui');
          assert.notEqual(process.env.PI_TEAM_CHILD, '1');
          report.initial = await invoke('subagent', { agent: 'runtime', task: 'SMOKE_INDEX_LIVE' });
          const handle = report.initial.content[0].text.match(/Team member: (\\S+) \\(runtime\\)/);
          assert.ok(handle, 'Interactive subagent must return a retained member handle, not fallback JSON');
          report.memberId = handle[1];
          report.members = (await team({ action: 'list' })).members;
          report.first = await team({ action: 'read', id: report.memberId });
          report.follow = await team({ action: 'send', id: report.memberId, text: 'SMOKE_INDEX_FOLLOWUP' });
          report.last = await team({ action: 'read', id: report.memberId });
        } catch (error) { report.fatal = error.stack ?? String(error); }
        finally {
          writeFileSync(${JSON.stringify(output)}, JSON.stringify(report));
          ctx.shutdown();
        }
      }});
    }
  `);
  try {
    temporaryParent = await owner.open(f.cwd, {}, 'pi-team full index (temporary)');
    const baseline = await layout();
    const args = [...isolatedArgs, '--skill', f.skill, '--session-dir', join(f.dir, 'parent-sessions'), '-e', extension, '/probe'];
    await call('pane.send_input', { pane_id: temporaryParent,
      text: isolatedCommand(temporaryParent, [piBin, ...args].map(shellQuote).join(' ')), keys: ['Enter'] });
    const report = await waitForJson(output);
    assert.equal(report.fatal, undefined);
    assert.equal(report.mode, 'tui');
    assert.equal(report.pane, temporaryParent);
    assert.equal(report.child, undefined);
    assert.equal(report.initial.details.mode, 'single');
    const [result] = report.initial.details.results;
    assert.equal(result.agent, 'runtime');
    assert.equal(result.agentSource, 'user');
    assert.equal(result.exitCode, 0);
    assert.equal(result.model, 'team-smoke/echo');
    assert.match(report.initial.content[0].text, /echo:SMOKE_INDEX_LIVE; user-turns:1/);
    assert.equal(report.members.length, 1);
    assert.equal(report.members[0].id, report.memberId);
    assert.equal(report.follow.memberId, report.memberId);
    assert.equal(report.follow.paneId, report.members[0].paneId);
    assert.deepEqual(childPanes, [report.follow.paneId]);
    assert.notEqual(report.follow.paneId, temporaryParent);
    assert.notEqual(report.follow.paneId, realParent);
    assert.equal(report.first.status, 'completed');
    assert.equal(report.follow.status, 'completed');
    assert.equal(report.follow.text, 'echo:SMOKE_INDEX_FOLLOWUP; user-turns:2');
    for (const key of ['sessionId', 'sessionFile']) {
      assert.ok(report.first[key]);
      assert.equal(report.follow[key], report.first[key]);
    }
    assert.notEqual(report.follow.commandId, report.first.commandId);
    assert.equal(report.last.commandId, report.follow.commandId);
    assert.equal(report.last.text, report.follow.text);
    const identities = (await readFile(launches, 'utf8')).trim().split('\n').map(line => JSON.parse(line));
    assert.equal(identities.length, 1, 'One real interactive child, no fallback or recursive spawn');
    assert.equal(identities[0].mode, 'tui');
    assert.equal(identities[0].child, '1');
    assert.equal(identities[0].pane, report.follow.paneId);
    assert.ok(identities[0].pid > 0 && identities[0].pid !== report.pid);
    assert.equal(f.server.histories.length, 2, 'Only the loopback child model is called');
    for (const history of f.server.histories) {
      assertSkillHistory(history, f.skill);
      assert.ok(!(history.tools ?? []).some(tool => ['subagent', 'team'].includes(tool.function.name)),
        'The real interactive child must not expose recursive delegation tools');
    }
    const history = f.server.histories[1].messages;
    assert.ok(history.some(message => message.role === 'user' && JSON.stringify(message.content).includes('SMOKE_INDEX_LIVE')));
    assert.ok(history.some(message => message.role === 'assistant' && JSON.stringify(message.content).includes('echo:SMOKE_INDEX_LIVE')));
    assert.ok(history.some(message => message.role === 'user' && JSON.stringify(message.content).includes('SMOKE_INDEX_FOLLOWUP')));
    assert.deepEqual(await waitForJson(stopped, 15000), { shutdown: true });
    assert.deepEqual([...owned], [temporaryParent], 'Parent shutdown must close its retained child');
    const current = await layout();
    assert.equal(parentOf(current.root, realParent).ratio, parentOf(baseline.root, realParent).ratio);
    assert.equal(current.focused_pane_id, before.focused_pane_id);
    assert.equal((await call('pane.current', {})).pane.pane_id, focused);
    assert.deepEqual(violations, []);
  } finally {
    // Remove children before the parent, including when the probe times out or fails.
    const errors = [];
    for (const id of [...owned].reverse()) {
      try { await call('pane.close', { pane_id: id }); }
      catch (error) { if (error.code !== 'pane_not_found') errors.push(error); }
    }
    for (const socket of connections) socket.destroy();
    await new Promise(resolve => proxy.close(resolve));
    const after = await layout();
    assert.deepEqual(layoutShape(after.root), layoutShape(before.root));
    assert.equal(after.focused_pane_id, before.focused_pane_id);
    assert.equal((await call('pane.current', {})).pane.pane_id, focused);
    assert.equal(after.tab_id, before.tab_id);
    assert.equal(after.workspace_id, before.workspace_id);
    assert.deepEqual(errors, [], 'Owned pane cleanup must succeed');
  }
});
