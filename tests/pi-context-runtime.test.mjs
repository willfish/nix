// Opt-in integration against the installed Pi, with isolated config and fake auth.
// PI_CONTEXT_TEST_BIN=pi node --test tests/pi-context-runtime.test.mjs
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { mkdtemp, mkdir, readFile, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import test from 'node:test';

const binary = process.env.PI_CONTEXT_TEST_BIN;
test('real Pi changes context, keeps registry defaults, restores sessions and opens the picker', {
  skip: !binary, timeout: 60000,
}, async () => {
  const dir = await mkdtemp(join(tmpdir(), 'pi-context-test-'));
  let child;
  try {
    const agentDir = join(dir, 'agent');
    await mkdir(agentDir);
    const models = JSON.parse(await readFile(new URL('../home/config/pi/models.json', import.meta.url), 'utf8'));
    // No production credentials or network requests are needed to select a model.
    models.providers = { 'openai-codex': { ...models.providers['openai-codex'], apiKey: 'offline-test-only' } };
    await writeFile(join(agentDir, 'models.json'), JSON.stringify(models));
    await writeFile(join(agentDir, 'settings.json'), JSON.stringify({
      defaultProvider: 'openai-codex', defaultModel: 'gpt-6-astra',
      enableInstallTelemetry: false,
    }));
    // Persist a harmless assistant entry so even an otherwise empty session is
    // written by Pi. Also expose reload using the documented command API.
    const fixture = join(dir, 'fixture.js');
    await writeFile(fixture, `export default function(pi) {
      pi.registerCommand('fixture-persist', { handler: async (_args, ctx) => {
        await ctx.newSession({ setup: sm => sm.appendMessage({
          role: 'assistant', content: [{type: 'text', text: 'offline fixture'}],
          api: 'openai-codex-responses', provider: 'openai-codex', model: 'gpt-6-astra',
          usage: {input: 1, output: 1, cacheRead: 0, cacheWrite: 0, totalTokens: 2,
            cost: {input: 0, output: 0, cacheRead: 0, cacheWrite: 0, total: 0}},
          stopReason: 'stop', timestamp: Date.now()
        }) });
      }});
      pi.registerCommand('fixture-reload', { handler: async (_args, ctx) => { await ctx.reload(); } });
      // Exercise the input lifecycle without making a model request.
      pi.on('input', () => ({ action: 'handled' }));
    }`);
    child = spawn(binary, [
      '--mode', 'rpc', '--offline', '--no-extensions', '--no-skills',
      '--no-prompt-templates', '--no-context-files',
      '-e', process.env.PI_CONTEXT_TEST_EXTENSION ?? fileURLToPath(new URL('../home/config/pi/extensions/context-window.js', import.meta.url)),
      '-e', fixture,
    ], { cwd: dir, env: { ...process.env, PI_CODING_AGENT_DIR: agentDir, CAPTURE_PROMPTS: '0' }, stdio: ['pipe', 'pipe', 'pipe'] });
    let buffer = '', stderr = '', counter = 0;
    const pending = new Map(), events = [];
    let choice = 1;
    const send = message => child.stdin.write(`${JSON.stringify(message)}\n`);
    child.stderr.setEncoding('utf8').on('data', text => { stderr += text; });
    child.stdout.setEncoding('utf8').on('data', text => {
      buffer += text;
      let newline;
      while ((newline = buffer.indexOf('\n')) >= 0) {
        const line = buffer.slice(0, newline);
        buffer = buffer.slice(newline + 1);
        if (!line.trim()) continue;
        const event = JSON.parse(line);
        events.push(event);
        if (event.type === 'extension_ui_request' && event.method === 'select') {
          send({ type: 'extension_ui_response', id: event.id, value: event.options[choice] });
        }
        if (event.type === 'extension_ui_request' && event.method === 'confirm') {
          send({ type: 'extension_ui_response', id: event.id, confirmed: true });
        }
        if (event.type === 'response') pending.get(event.id)?.(event);
      }
    });
    function request(type, data = {}) {
      return new Promise((resolve, reject) => {
        const id = String(++counter);
        const timer = setTimeout(() => { pending.delete(id); reject(new Error(`Timed out: ${type}; ${stderr}`)); }, 10000);
        pending.set(id, response => {
          clearTimeout(timer);
          pending.delete(id);
          if (!response.success) reject(new Error(JSON.stringify(response)));
          else resolve(response.data);
        });
        send({ id, type, ...data });
      });
    }
    const commands = await request('get_commands');
    assert.ok(commands.commands.some(command => command.name === 'context'));
    await request('prompt', { message: '/fixture-persist' });
    assert.equal((await request('get_state')).model.contextWindow, 272000);
    await request('set_thinking_level', { level: 'high' });
    await request('prompt', { message: '/context' });
    assert.equal((await request('get_state')).model.contextWindow, 500000);
    assert.equal((await request('get_state')).thinkingLevel, 'high');
    assert.ok(events.some(event => event.method === 'select' && event.options.length === 3));
    const catalogue = await request('get_available_models');
    assert.equal(catalogue.models.find(model => model.id === 'gpt-6-astra').contextWindow, 272000);
    assert.equal((await request('get_session_stats')).contextUsage.contextWindow, 500000);
    await request('prompt', { message: '/context maximum' });
    const state = await request('get_state');
    assert.equal(state.model.contextWindow, 872000);
    assert.equal(state.model.maxTokens, 128000);
    const sessionFile = state.sessionFile;
    await request('prompt', { message: '/fixture-reload' });
    assert.equal((await request('get_state')).model.contextWindow, 872000);
    await request('set_model', { provider: 'openai-codex', modelId: 'gpt-6-astra' });
    await request('prompt', { message: 'offline input fixture' });
    assert.equal((await request('get_state')).model.contextWindow, 872000);
    await request('new_session');
    assert.equal((await request('get_state')).model.contextWindow, 272000);
    await request('switch_session', { sessionPath: sessionFile });
    assert.equal((await request('get_state')).model.contextWindow, 872000);
    choice = 0;
    await request('prompt', { message: '/context' });
    assert.equal((await request('get_state')).model.contextWindow, 272000);
    assert.equal(events.filter(event => event.type === 'extension_error').length, 0);
    assert.equal(events.filter(event => event.type === 'agent_start').length, 0, 'must not call an LLM');
  } finally {
    if (child && child.exitCode === null) {
      child.kill('SIGTERM');
      await new Promise(resolve => child.once('exit', resolve));
    }
    await rm(dir, { recursive: true, force: true });
  }
});
