import assert from 'node:assert/strict';
import { mkdtemp, mkdir, writeFile, readFile, symlink, rm } from 'node:fs/promises';
import { createServer } from 'node:http';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { join } from 'node:path';

const [binary, plugin] = process.argv.slice(2);
assert(binary && plugin, 'pass the V2 binary and built workflow plugin');
const root = await mkdtemp('/tmp/opencode/workflow-');
const project = join(root, 'project');
const home = join(root, 'home');
await mkdir(project, { recursive: true });
await mkdir(join(home, '.config/opencode'), { recursive: true });
for (const id of ['fixture-specialist', 'verification-before-completion', 'manual-only-fixture']) {
  const path = join(home, '.agents/skills', id);
  await mkdir(path, { recursive: true });
  await writeFile(join(path, 'SKILL.md'), `---\nname: ${id}\ndescription: Fixture skill discovery ${id}\n${id === 'manual-only-fixture' ? 'disable-model-invocation: true\n' : ''}---\nPRELOADED_${id}\n`);
}
const requests = [];
let goalTurns = 0;
const fixture = createServer(async (request, response) => {
  const chunks = [];
  for await (const chunk of request) chunks.push(chunk);
  const body = JSON.parse(Buffer.concat(chunks).toString());
  requests.push(body);
  const text = JSON.stringify(body.messages);
  const last = body.messages.at(-1);
  let content = 'fixture complete';
  let call;
  if (text.includes('OFFLINE_WAITING_GOAL')) await new Promise(resolve => setTimeout(resolve, 300));
  if (body.tools?.length && text.includes('OFFLINE_STALE_GOAL')) {
    await new Promise(resolve => setTimeout(resolve, 300));
    if (!text.includes('replacement')) call = { name: 'update_goal', arguments: JSON.stringify({ status: 'complete' }) };
  } else if (body.tools?.length && text.includes('OFFLINE_GOAL_OBJECTIVE')) {
    goalTurns++;
    if (goalTurns === 1) content = 'Completed the first part; further work remains.';
    else if (!text.includes('"name":"update_goal"') && !body.messages.some(m => m.role === 'tool')) {
      call = { name: 'update_goal', arguments: JSON.stringify({ status: 'complete' }) };
    }
  } else if (body.tools?.length && text.includes('SKILL_DISCOVERY') && last.role !== 'tool') {
    call = { name: 'skill_catalog', arguments: JSON.stringify({ query: 'fixture' }) };
  } else if (body.tools?.length && text.includes('NATIVE_CHILD') && !text.includes('You are a subagent spawned')) {
    if (last.role !== 'tool') call = { name: 'subagent', arguments: JSON.stringify({ agent: 'builder', model: 'fixture/fixture', description: 'Offline native child', prompt: 'Return child complete.' }) };
  } else if (body.tools?.length && text.includes('BUS_GUIDANCE') && last.role !== 'tool') {
    call = { name: 'execute', arguments: JSON.stringify({ code: 'return await tools.coordination.get_coordination_guidance({ role: "builder" });' }) };
  } else if (body.tools?.length && text.includes('NATIVE_QUESTION') && last.role !== 'tool') {
    call = { name: 'question', arguments: JSON.stringify({ questions: [{ header: 'Fixture', question: 'Choose a fixture option', options: [{ label: 'First', description: 'First option' }, { label: 'Second', description: 'Second option' }] }] }) };
  }
  const message = call ? { role: 'assistant', content: null, tool_calls: [{ id: `call_${requests.length}`, type: 'function', function: call }] } : { role: 'assistant', content };
  const finish_reason = call ? 'tool_calls' : 'stop';
  if (body.stream) {
    response.writeHead(200, { 'content-type': 'text/event-stream' });
    const delta = call ? { role: 'assistant', tool_calls: [{ index: 0, ...message.tool_calls[0] }] } : { role: 'assistant', content };
    response.write(`data: ${JSON.stringify({ id: `fixture_${requests.length}`, object: 'chat.completion.chunk', model: 'fixture', choices: [{ index: 0, delta, finish_reason: null }] })}\n\n`);
    response.write(`data: ${JSON.stringify({ id: `fixture_${requests.length}`, object: 'chat.completion.chunk', model: 'fixture', choices: [{ index: 0, delta: {}, finish_reason }], usage: { prompt_tokens: 100, completion_tokens: 20, total_tokens: 120 } })}\n\n`);
    response.end('data: [DONE]\n\n');
  } else {
    response.writeHead(200, { 'content-type': 'application/json' });
    response.end(JSON.stringify({ id: `fixture_${requests.length}`, object: 'chat.completion', model: 'fixture', choices: [{ index: 0, message, finish_reason }], usage: { prompt_tokens: 100, completion_tokens: 20, total_tokens: 120 } }));
  }
});
fixture.listen(0, '127.0.0.1');
await once(fixture, 'listening');
const fixturePort = fixture.address().port;
const reservation = createServer();
reservation.listen(0, '127.0.0.1');
await once(reservation, 'listening');
const port = reservation.address().port;
await new Promise(resolve => reservation.close(resolve));
const deployed = process.env.OPENCODE_FIXTURE_CONFIG ? JSON.parse(await readFile(process.env.OPENCODE_FIXTURE_CONFIG, 'utf8')) : {};
if (process.env.OPENCODE_FIXTURE_AGENTS) await symlink(process.env.OPENCODE_FIXTURE_AGENTS, join(home, '.config/opencode/agents'));
await writeFile(join(home, '.config/opencode/opencode.json'), JSON.stringify({
  ...deployed,
  $schema: 'https://opencode.ai/config.json',
  autoupdate: false,
  model: 'fixture/fixture',
  providers: { ...deployed.providers, fixture: { package: '@opencode/ai/providers/openai-compatible', settings: { baseURL: `http://127.0.0.1:${fixturePort}/v1`, apiKey: 'fixture' }, models: { fixture: { name: 'Fixture', capabilities: { tools: true }, limit: { context: 128000, output: 4096 } } } } },
  mcp: { ...deployed.mcp, servers: Object.fromEntries(Object.entries(deployed.mcp?.servers ?? {}).map(([name, server]) => [name, { ...server, disabled: true }])) },
  agents: { ...deployed.agents, builder: { ...deployed.agents?.builder, mode: 'subagent', description: 'Offline test builder', model: 'fixture/fixture' }, title: { model: 'fixture/fixture' } },
  plugins: [{ package: plugin, options: { busEnabled: process.env.OPENCODE_FIXTURE_BUS === '1' } }],
}));
const env = { ...process.env, HOME: home, XDG_CONFIG_HOME: join(home, '.config'), XDG_DATA_HOME: join(home, '.local/share'), XDG_STATE_HOME: join(home, '.local/state'), XDG_CACHE_HOME: join(home, '.cache') };
for (const key of Object.keys(env)) if (/TOKEN|KEY|SECRET|PASSWORD|CREDENTIAL|OPENCODE_CONFIG|PI_AGENT_BUS/.test(key)) delete env[key];
env.OPENCODE_PASSWORD = 'fixture-local-only';
const headers = { 'content-type': 'application/json', authorization: `Basic ${Buffer.from('opencode:fixture-local-only').toString('base64')}` };
let logs = '';
function startServer() {
  const child = spawn(binary, ['serve', '--hostname', '127.0.0.1', '--port', String(port)], { cwd: project, env, stdio: ['ignore', 'pipe', 'pipe'] });
  child.stdout.on('data', chunk => { logs += chunk; });
  child.stderr.on('data', chunk => { logs += chunk; });
  return child;
}
let server = startServer();
const base = `http://127.0.0.1:${port}`;
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
async function api(path, body, method = body === undefined ? 'GET' : 'POST') {
  const response = await fetch(base + path, { method, headers, ...(body === undefined ? {} : { body: JSON.stringify(body) }), signal: AbortSignal.timeout(20000) });
  const text = await response.text();
  assert(response.ok, `${path}: ${response.status} ${text}`);
  return text ? JSON.parse(text) : undefined;
}
async function until(read, predicate, title) {
  let lastValue;
  for (let i = 0; i < 100; i++) {
    const value = await read();
    lastValue = value;
    if (predicate(value)) return value;
    if (server.exitCode !== null) throw new Error(`Server exited: ${logs}`);
    await delay(100);
  }
  throw new Error(`Timed out: ${title}; last value: ${JSON.stringify(lastValue)}`);
}
const goal = async (sessionID, command) => (await api('/api/rpc/dotfiles.workflow/goal', { input: { sessionID, command } })).output;
const create = async () => (await api('/api/session', { location: { directory: project }, model: { providerID: 'fixture', id: 'fixture' } })).data;
async function settled(id) {
  return until(() => api(`/api/session/${id}`), value => value.data?.outcome === 'succeeded', 'session settlement');
}
try {
  await until(async () => { try { return (await fetch(base + '/api/session', { headers })).ok; } catch { return false; } }, Boolean, 'server startup');
  const session = await create();
  assert(session?.id, JSON.stringify(session));
  await api(`/api/session/${session.id}/prompt`, { text: 'SKILL_DISCOVERY' });
  await settled(session.id);
  const first = requests.find(request => request.tools?.length && JSON.stringify(request.messages).includes('SKILL_DISCOVERY'));
  assert(first, `provider received tool request: ${JSON.stringify(requests.map(request => ({ keys: Object.keys(request), tools: request.tools?.map(tool => tool.function.name) })))}`);
  const systems = JSON.stringify(first.messages.filter(message => message.role === 'system' || message.role === 'developer'));
  const matched = systems.indexOf('Fixture skill discovery fixture-specialist');
  if (matched !== -1) console.log('Unexpected skill advertisement:', systems.slice(Math.max(0, matched - 180), matched + 160));
  assert(!JSON.stringify(first.messages.filter(message => message.role === 'system' || message.role === 'developer')).includes('Fixture skill discovery fixture-specialist'), 'skill metadata is deferred');
  assert(first.tools.some(tool => tool.function.name === 'skill_catalog'), 'compact catalogue is directly callable');
  assert(requests.some(request => request.messages.some(message => message.role === 'tool' && JSON.stringify(message.content).includes('fixture-specialist') && JSON.stringify(message.content).includes('results'))), 'catalogue returns the requested trusted skill ID');
  const discovery = (await api(`/api/session/${session.id}/context`)).data.flatMap(message => message.content ?? []).find(part => part.type === 'tool' && part.name === 'skill_catalog');
  const catalogue = JSON.parse(discovery.state.content[0].text);
  assert(catalogue.results.find(skill => skill.id === 'manual-only-fixture')?.manualOnly, 'manual-only metadata survives compact discovery');
  console.log('PASS: real V2 plugin loaded, compact skill discovery reached the local provider');
  if (process.env.OPENCODE_FIXTURE_AGENTS) {
    const agents = (await api('/api/agent')).data;
    const architect = agents.find(agent => agent.id === 'architect');
    assert.equal(architect.model.providerID, 'openai');
    assert.equal(architect.model.variant, 'high');
    assert(architect.permissions.some(rule => rule.action === 'edit' && rule.effect === 'deny'));
    const worker = agents.find(agent => agent.id === 'worker');
    assert(!worker.permissions.some(rule => rule.action === 'edit' && rule.effect === 'deny'));
    console.log('PASS: deployed role model/effort mapping and read-only permissions');
  }
  if (process.env.OPENCODE_FIXTURE_BUS === '1') {
    const busSession = await create();
    await api(`/api/session/${busSession.id}/prompt`, { text: 'BUS_GUIDANCE' });
    await settled(busSession.id);
    const context = (await api(`/api/session/${busSession.id}/context`)).data;
    const calls = context.flatMap(message => message.content ?? []).filter(part => part.type === 'tool');
    assert(calls.some(part => part.name === 'execute' && part.state.status === 'completed' && !part.state.metadata?.error));
    console.log('PASS: Switchboard guidance through native Code Mode without bus credentials');
  }

  const work = await create();
  await goal(work.id, 'set OFFLINE_GOAL_OBJECTIVE');
  await until(() => goal(work.id, 'status'), value => value.goal?.status === 'complete', 'automatic goal continuation and completion');
  assert(goalTurns >= 2, 'goal continued after an incomplete final response');
  const complete = await goal(work.id, 'status');
  assert(complete.goal.tokensUsed > 0, 'goal accounts reported usage');
  console.log(`PASS: persistent goal continued and completed, reported ${complete.goal.tokensUsed} tokens`);

  const childParent = await create();
  await api(`/api/session/${childParent.id}/prompt`, { text: 'NATIVE_CHILD' });
  await settled(childParent.id);
  const children = await api(`/api/session?parentID=${childParent.id}`);
  assert.equal(children.data.length, 1, 'native child retained');
  assert.equal(children.data[0].outcome, 'succeeded');
  assert(requests.some(request => JSON.stringify(request.messages).includes('PRELOADED_verification-before-completion')), 'role skill preloaded through native prompt admission');
  console.log('PASS: native retained child session and role skill preloading');

  const waiting = await create();
  await goal(waiting.id, 'set OFFLINE_WAITING_GOAL');
  assert.equal((await goal(waiting.id, 'pause')).goal.status, 'paused');
  await goal(waiting.id, 'edit OFFLINE_WAITING_GOAL edited');
  assert.equal((await goal(waiting.id, 'status')).goal.objective, 'OFFLINE_WAITING_GOAL edited');
  await goal(waiting.id, 'budget 1');
  await goal(waiting.id, 'resume');
  await until(() => goal(waiting.id, 'status'), value => value.goal.status === 'budget_limited', 'budget boundary');
  assert.equal((await goal(waiting.id, 'budget 10000')).goal.status, 'budget_limited', 'raising a stopped budget does not resume');
  await goal(waiting.id, 'clear');
  assert.equal((await goal(waiting.id, 'status')).goal, null);
  console.log('PASS: explicit pause, edit, budget stop, no implicit resume, and clear');

  const persistent = await create();
  await goal(persistent.id, 'set OFFLINE_WAITING_GOAL persistent');
  const saved = await goal(persistent.id, 'pause');
  server.kill('SIGTERM');
  await once(server, 'exit');
  server = startServer();
  await until(async () => { try { return (await fetch(base + '/api/session', { headers })).ok; } catch { return false; } }, Boolean, 'restarted server');
  const restored = await goal(persistent.id, 'status');
  assert.equal(restored.goal.id, saved.goal.id);
  assert.equal(restored.goal.status, 'paused');
  console.log('PASS: goal survives a real server restart without resuming a paused objective');

  const stale = await create();
  await goal(stale.id, 'set OFFLINE_STALE_GOAL');
  await until(async () => requests.some(request => request.tools?.length && JSON.stringify(request.messages).includes('OFFLINE_STALE_GOAL')), Boolean, 'stale request started');
  await goal(stale.id, 'edit OFFLINE_STALE_GOAL replacement');
  await until(() => api(`/api/session/${stale.id}/context`), value => value.data.flatMap(message => message.content ?? []).some(part => part.type === 'tool' && part.state.status === 'error' && part.state.error.message.includes('Goal changed during this request')), 'stale completion rejected');
  const paused = await goal(stale.id, 'pause');
  assert.equal(paused.goal.status, 'paused');
  assert.equal(paused.goal.objective, 'OFFLINE_STALE_GOAL replacement');
  console.log('PASS: an in-flight response cannot complete an edited objective');

  const questionSession = await create();
  await api(`/api/session/${questionSession.id}/prompt`, { text: 'NATIVE_QUESTION' });
  const forms = await until(() => api(`/api/session/${questionSession.id}/form`), value => value.data?.length, 'native question form');
  assert.equal(forms.data.length, 1);
  if (!process.env.OPENCODE_FIXTURE_TUI) await api(`/api/session/${questionSession.id}/form/${forms.data[0].id}`, undefined, 'DELETE');
  console.log('PASS: native question tool creates its own interactive form');
  if (process.env.OPENCODE_FIXTURE_TUI) {
    await writeFile(join(home, '.config/opencode/cli.json'), JSON.stringify({ $schema: 'https://opencode.ai/v2/cli.json', keybinds: { 'app.clear': 'none', 'model.list': 'ctrl+l,<leader>m' } }));
    console.log('READY_FOR_TUI');
    const before = new Set((await api('/api/session')).data.map(session => session.id));
    const tui = spawn(binary, ['--server', base, ...(process.env.OPENCODE_FIXTURE_HOME ? [] : ['--session', questionSession.id])], { cwd: project, env, stdio: 'inherit' });
    await once(tui, 'exit');
    assert.equal(tui.exitCode, 0, 'TUI exits cleanly');
    if (process.env.OPENCODE_FIXTURE_HOME) {
      const added = (await api('/api/session')).data.filter(session => !before.has(session.id));
      assert.equal(added.length, 1, 'goal command on the home screen creates one session');
      assert.equal((await goal(added[0].id, 'status')).goal.status, 'complete');
      console.log('PASS: /goal from the home screen creates a session and completes the goal');
    }
  }
  console.log('All representative checks passed. No live model or credentials used.');
} catch (error) {
  console.error(logs.replace(/server password \S+/g, 'server password [redacted]').slice(-10000));
  throw error;
} finally {
  server.kill('SIGTERM');
  await Promise.race([once(server, 'exit'), delay(3000)]);
  if (server.exitCode === null) server.kill('SIGKILL');
  await new Promise(resolve => fixture.close(resolve));
  await rm(root, { recursive: true, force: true });
}
