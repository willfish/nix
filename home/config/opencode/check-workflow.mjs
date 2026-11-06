import assert from 'node:assert/strict';
import { mkdtemp, mkdir, writeFile, readFile, symlink, rm } from 'node:fs/promises';
import { createServer } from 'node:http';
import { createServer as createSocketServer } from 'node:net';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { join } from 'node:path';

const [binary, plugin] = process.argv.slice(2);
assert(binary && plugin, 'pass the V2 binary and built workflow plugin');
const root = await mkdtemp('/tmp/opencode/workflow-');
const project = join(root, 'project');
const home = join(root, 'home');
await mkdir(project, { recursive: true });
await writeFile(join(project, '.env'), 'FIXTURE_PERMISSION=synthetic\n');
await writeFile(join(root, 'outside.txt'), 'Synthetic external-directory fixture\n');
await mkdir(join(home, '.config/opencode'), { recursive: true });
for (const id of ['fixture-specialist', 'verification-before-completion', 'manual-only-fixture']) {
  const path = join(home, '.agents/skills', id);
  await mkdir(path, { recursive: true });
  await writeFile(join(path, 'SKILL.md'), `---\nname: ${id}\ndescription: Fixture skill discovery ${id}\n${id === 'manual-only-fixture' ? 'disable-model-invocation: true\n' : ''}---\nPRELOADED_${id}\n`);
}
const requests = [];
const fixture = createServer(async (request, response) => {
  const chunks = [];
  for await (const chunk of request) chunks.push(chunk);
  const body = JSON.parse(Buffer.concat(chunks).toString());
  requests.push(body);
  const text = JSON.stringify(body.messages);
  const last = body.messages.at(-1);
  let content = 'fixture complete';
  let call;
  if (text.includes('Reply with only the title.')) {
    content = 'Skill discovery check';
  } else if (text.includes('Stop acting as the assistant. You are now the reviewer for the session')) {
    content = JSON.stringify({ verdict: 'done', reason: 'The deterministic fixture produced its required result.' });
  } else if (body.tools?.length && text.includes('OFFLINE_NATIVE_GOAL') && last.role !== 'tool') {
    call = { name: 'todowrite', arguments: JSON.stringify({ todos: [{ content: 'Verify fixture objective', status: 'completed' }] }) };
  } else if (body.tools?.length && text.includes('PERMISSION_CHECK')) {
    const count = body.messages.flatMap(message => message.tool_calls ?? []).length;
    if (count === 0) call = { name: 'read', arguments: JSON.stringify({ path: '.env' }) };
    else if (count === 1) call = { name: 'read', arguments: JSON.stringify({ path: join(root, 'outside.txt') }) };
    else if (count === 2) call = { name: 'write', arguments: JSON.stringify({ path: 'permission-check.txt', content: 'Synthetic write fixture\n' }) };
    else if (count === 3) call = { name: 'shell', arguments: JSON.stringify({ command: 'test -f .env && test -f permission-check.txt', workdir: project }) };
  } else if (body.tools?.length && text.includes('SKILL_DISCOVERY') && last.role !== 'tool') {
    call = { name: 'skill_catalog', arguments: JSON.stringify({ query: 'fixture' }) };
  } else if (body.tools?.length && JSON.stringify(last).includes('NATIVE_CHILD') && last.role !== 'tool') {
    const existing = /followup:(ses_[A-Za-z0-9]+)/.exec(JSON.stringify(last));
    call = { name: 'subagent', arguments: JSON.stringify({ agent: 'builder', model: 'fixture/fixture', description: 'Offline native child', prompt: 'Return child complete.', ...(existing ? { sessionID: existing[1] } : {}), ...(JSON.stringify(last).includes('BACKGROUND') ? { background: true } : {}) }) };
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
if (process.env.OPENCODE_FIXTURE_COMMANDS) await symlink(process.env.OPENCODE_FIXTURE_COMMANDS, join(home, '.config/opencode/commands'));
const routingMarker = 'Keep delegation, shared tokens, file ownership and final integration coordinated here';
if (process.env.OPENCODE_FIXTURE_ORCHESTRATOR) {
  const text = await readFile(process.env.OPENCODE_FIXTURE_ORCHESTRATOR, 'utf8');
  assert(text.includes('## Design routing') && text.includes(routingMarker));
  assert(!text.includes('## Final voice summary') && !text.includes('## Dotfiles finish line'));
  await symlink(process.env.OPENCODE_FIXTURE_ORCHESTRATOR, join(home, '.config/opencode/ORCHESTRATOR.md'));
}
await writeFile(join(home, '.config/opencode/opencode.json'), JSON.stringify({
  ...deployed,
  $schema: 'https://opencode.ai/config.json',
  autoupdate: false,
  experimental: { ...deployed.experimental, todo: true },
  model: 'fixture/fixture',
  providers: { ...deployed.providers, fixture: { package: '@opencode/ai/providers/openai-compatible', settings: { baseURL: `http://127.0.0.1:${fixturePort}/v1`, apiKey: 'fixture' }, models: { fixture: { name: 'Fixture', capabilities: { tools: true }, limit: { context: 128000, output: 4096 } } } } },
  mcp: { ...deployed.mcp, servers: Object.fromEntries(Object.entries(deployed.mcp?.servers ?? {}).map(([name, server]) => [name, { ...server, disabled: true }])) },
  agents: { ...deployed.agents, builder: { ...deployed.agents?.builder, mode: 'subagent', description: 'Offline test builder', model: 'fixture/fixture' }, title: { model: 'fixture/fixture' } },
  plugins: [{ package: plugin, options: { busEnabled: process.env.OPENCODE_FIXTURE_BUS === '1' } }],
}));
const env = { ...process.env, HOME: home, XDG_CONFIG_HOME: join(home, '.config'), XDG_DATA_HOME: join(home, '.local/share'), XDG_STATE_HOME: join(home, '.local/state'), XDG_CACHE_HOME: join(home, '.cache') };
for (const key of Object.keys(env)) if (/TOKEN|KEY|SECRET|PASSWORD|CREDENTIAL|OPENCODE_CONFIG|PI_AGENT_BUS|HERDR_|^HC_/.test(key)) delete env[key];
const herdrSocket = join(root, 'herdr.sock');
let herdrTabLabel = '3';
const herdrCalls = [];
const herdrServer = createSocketServer(socket => {
  let input = '';
  socket.on('data', chunk => {
    input += chunk.toString('utf8');
    if (!input.includes('\n')) return;
    const req = JSON.parse(input.slice(0, input.indexOf('\n')));
    herdrCalls.push([req.method, req.params]);
    let result = {};
    if (req.method === 'pane.get') result = { pane: { pane_id: 'w1:p2', tab_id: 'w1:t1', workspace_id: 'w1' } };
    else if (req.method === 'pane.list') result = { panes: [{ pane_id: 'w1:p10', tab_id: 'w1:t1', workspace_id: 'w1' }, { pane_id: 'w1:p2', tab_id: 'w1:t1', workspace_id: 'w1' }] };
    else if (req.method === 'tab.get') result = { tab: { number: 26, label: herdrTabLabel } };
    else if (req.method === 'tab.rename') { herdrTabLabel = req.params.label; result = { tab: { label: herdrTabLabel } }; }
    socket.write(`${JSON.stringify({ id: req.id, result })}\n`);
  });
});
herdrServer.listen(herdrSocket);
await once(herdrServer, 'listening');
env.HERDR_ENV = '1';
env.HERDR_SOCKET_PATH = herdrSocket;
env.HERDR_PANE_ID = 'w1:p2';
env.OPENCODE_PASSWORD = 'fixture-local-only';
const headers = { 'content-type': 'application/json', authorization: `Basic ${Buffer.from('opencode:fixture-local-only').toString('base64')}` };
let logs = '';
function startServer() {
  const child = spawn(binary, ['serve', '--hostname', '127.0.0.1', '--port', String(port)], { cwd: project, env, stdio: ['ignore', 'pipe', 'pipe'] });
  child.stdout.on('data', chunk => { logs += chunk; });
  child.stderr.on('data', chunk => { logs += chunk; });
  return child;
}
const server = startServer();
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
const goal = async sessionID => (await api('/api/rpc/opencode.goal/get', { input: { sessionID } })).output;
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
  if (process.env.OPENCODE_FIXTURE_ORCHESTRATOR) {
    assert(systems.includes(routingMarker), 'primary design routing is present');
    assert(!systems.includes('Only this section goes to TTS'), 'Pi-only voice formatting is absent');
    console.log('PASS: lean primary context contains design routing without Pi voice formatting');
  }
  const matched = systems.indexOf('Fixture skill discovery fixture-specialist');
  if (matched !== -1) console.log('Unexpected skill advertisement:', systems.slice(Math.max(0, matched - 180), matched + 160));
  assert(!JSON.stringify(first.messages.filter(message => message.role === 'system' || message.role === 'developer')).includes('Fixture skill discovery fixture-specialist'), 'skill metadata is deferred');
  assert(first.tools.some(tool => tool.function.name === 'skill_catalog'), 'compact catalogue is directly callable');
  assert(first.tools.some(tool => tool.function.name === 'todowrite'), 'native todos are enabled');
  assert(first.tools.some(tool => tool.function.name === 'speak'), 'manual speech remains available');
  assert(!first.tools.some(tool => ['get_goal', 'create_goal', 'update_goal'].includes(tool.function.name)), 'the duplicate goal tools are absent');
  assert(requests.some(request => request.messages.some(message => message.role === 'tool' && JSON.stringify(message.content).includes('fixture-specialist') && JSON.stringify(message.content).includes('results'))), 'catalogue returns the requested trusted skill ID');
  const discovery = (await api(`/api/session/${session.id}/context`)).data.flatMap(message => message.content ?? []).find(part => part.type === 'tool' && part.name === 'skill_catalog');
  const catalogue = JSON.parse(discovery.state.content[0].text);
  assert(catalogue.results.find(skill => skill.id === 'manual-only-fixture')?.manualOnly, 'manual-only metadata survives compact discovery');
  await until(() => herdrTabLabel, value => value === '3 Skill discovery check', 'automatic session title names the numeric Herdr tab');
  await api(`/api/session/${session.id}`, { title: 'Updated session title' }, 'PATCH');
  await until(() => herdrTabLabel, value => value === '3 Updated session title', 'owned Herdr tab follows session title updates');
  herdrTabLabel = '3 Manual tab';
  await api(`/api/session/${session.id}`, { title: 'Ignored after manual rename' }, 'PATCH');
  await delay(200);
  assert.equal(herdrTabLabel, '3 Manual tab', 'manual Herdr tab rename ends automatic ownership');
  console.log('PASS: real V2 plugin loaded, compact skill discovery reached the local provider, and Herdr tab followed automatic session title');
  const voice = async (sessionID, action = 'status') => (await api('/api/rpc/dotfiles.voice/control', { input: { sessionID, action } })).output;
  assert.deepEqual(await voice(session.id), { enabled: false, error: '' });
  const voiceSession = await create();
  const beforeVoice = requests.length;
  assert.deepEqual(await voice(voiceSession.id, 'on'), { enabled: true, error: '' });
  assert.equal(requests.length, beforeVoice, 'voice control does not invoke a model');
  assert.equal((await voice(session.id)).enabled, false, 'opt-in does not change other sessions');
  await api(`/api/session/${voiceSession.id}/prompt`, { text: 'OFFLINE_SPEECH_COMPLETION' });
  await settled(voiceSession.id);
  // The isolated fixture has no key. Reaching this status proves the native completion
  // event reached TTS, while missing credentials stop it before any network request.
  const voiceFailure = await until(() => voice(voiceSession.id), value => value.error, 'completion callback attempted speech');
  assert.equal(voiceFailure.enabled, true, 'speech failure did not disable or fail the session');
  assert.deepEqual(await voice(voiceSession.id, 'off'), { enabled: false, error: '' });
  console.log('PASS: real V2 voice RPC defaults off, stays session-scoped, and completion callback isolates missing-key failure');
  if (process.env.OPENCODE_FIXTURE_COMMANDS) {
    const commands = (await until(() => api('/api/command'), value => value.data.some(command => command.name === 'design'), 'generated commands loaded')).data;
    assert.equal(commands.find(command => command.name === 'review')?.description, 'review changes [commit|branch|pr], defaults to uncommitted');
    console.log('PASS: native review is active alongside generated workflow commands');
  }
  if (process.env.OPENCODE_FIXTURE_CONFIG) {
    assert(deployed.permissions?.some(rule => rule.action === '*' && rule.resource === '*' && rule.effect === 'allow'), 'deployed configuration auto-allows tool operations');
    assert.equal(deployed.experimental?.todo, true, 'deployed configuration enables native todos');
    const permissionSession = await create();
    await api(`/api/session/${permissionSession.id}/prompt`, { text: 'PERMISSION_CHECK' });
    await settled(permissionSession.id);
    const context = (await api(`/api/session/${permissionSession.id}/context`)).data;
    const calls = context.flatMap(message => message.content ?? []).filter(part => part.type === 'tool');
    assert.equal(calls.length, 4);
    assert(calls.every(part => part.state.status === 'completed'), 'sensitive-path reads, external reads, writes and shell run without an approval response');
    assert.equal(await readFile(join(project, 'permission-check.txt'), 'utf8'), 'Synthetic write fixture\n');
    console.log('PASS: environment-file reads, external-directory reads, writes and shell run without permission prompts');
  }
  if (process.env.OPENCODE_FIXTURE_AGENTS) {
    const text = await readFile(join(process.env.OPENCODE_FIXTURE_AGENTS, 'builder.md'), 'utf8');
    assert(!text.includes('Load these skills when the task reaches their trigger:'), 'generated skill-loading note is absent');
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
  assert.equal(await goal(work.id), null);
  await api(`/api/session/${work.id}/command`, { name: 'goal', text: 'OFFLINE_NATIVE_GOAL' });
  const completed = await until(() => goal(work.id), value => value?.status === 'complete', 'native goal review and completion');
  assert.equal(completed.objective, 'OFFLINE_NATIVE_GOAL');
  const goalContext = (await api(`/api/session/${work.id}/context`)).data;
  assert(goalContext.flatMap(message => message.content ?? []).some(part => part.type === 'tool' && part.name === 'todowrite' && part.state.status === 'completed' && part.state.metadata.todos[0].status === 'completed'));
  await api(`/api/session/${work.id}/command`, { name: 'goal', text: 'clear' });
  assert.equal(await goal(work.id), null);
  console.log('PASS: OpenCode native goal command, reviewer, completion, clear and todo metadata');

  const childParent = await create();
  await api(`/api/session/${childParent.id}/prompt`, { text: 'NATIVE_CHILD' });
  await settled(childParent.id);
  const children = await api(`/api/session?parentID=${childParent.id}`);
  assert.equal(children.data.length, 1, 'native child retained');
  assert.equal(children.data[0].outcome, 'succeeded');
  assert(requests.some(request => JSON.stringify(request.messages).includes('PRELOADED_verification-before-completion')), 'role skill preloaded through native prompt admission');
  if (process.env.OPENCODE_FIXTURE_ORCHESTRATOR) {
    const child = requests.find(request => JSON.stringify(request.messages).includes('PRELOADED_verification-before-completion'));
    assert(!JSON.stringify(child.messages.filter(message => message.role === 'system' || message.role === 'developer')).includes(routingMarker), 'primary routing stays out of child sessions');
  }
  console.log('PASS: native retained child session and role skill preloading');
  await api(`/api/session/${childParent.id}/prompt`, { text: `NATIVE_CHILD followup:${children.data[0].id}` });
  await until(() => api(`/api/session/${children.data[0].id}/context`), value => value.data.filter(message => message.type === 'user').length === 2, 'native child follow-up');
  const retained = (await api(`/api/session?parentID=${childParent.id}`)).data;
  assert.equal(retained.length, 1, 'follow-up reuses the child');
  const background = await create();
  await api(`/api/session/${background.id}/prompt`, { text: 'NATIVE_CHILD BACKGROUND' });
  await until(() => api(`/api/session/${background.id}/context`), value => value.data.flatMap(message => message.content ?? []).some(part => part.type === 'tool' && part.name === 'subagent' && part.state.status === 'completed' && part.state.metadata.status === 'running'), 'native background dispatch');
  console.log('PASS: native subagent follow-up and background dispatch');

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
      assert.equal((await goal(added[0].id)).status, 'complete');
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
  await new Promise(resolve => herdrServer.close(resolve));
  await rm(root, { recursive: true, force: true });
}
