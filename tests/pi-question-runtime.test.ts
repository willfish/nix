// Real Pi loads the question and subagent extensions together, including from Nix store paths.
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { mkdtemp, mkdir, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { setTimeout as delay } from 'node:timers/promises';
import test from 'node:test';

const binary = process.env.PI_QUESTION_TEST_BIN ?? process.env.PI_SESSION_TEST_BIN;
test('real Pi loads sibling question and subagent extensions with required options', { skip: !binary, timeout: 30000 }, async () => {
  const dir = await mkdtemp(join(tmpdir(), 'pi-question-runtime-'));
  const histories = [], pending = new Map();
  let child, buffer = '', stderr = '', sequence = 0;
  const server = createServer(async (req, res) => {
    try {
      let body = ''; for await (const chunk of req) body += chunk;
      const payload = JSON.parse(body); histories.push(payload);
      res.writeHead(200, { 'content-type': 'text/event-stream' });
      const chunk = (delta, finish_reason = null) => res.write(`data: ${JSON.stringify({ id: 'fixture', object: 'chat.completion.chunk', created: 1, model: 'echo', choices: [{ index: 0, delta, finish_reason }] })}\n\n`);
      chunk({ role: 'assistant' }); chunk({ content: 'ok' }); chunk({}, 'stop'); res.end('data: [DONE]\n\n');
    } catch (error) {
      stderr += `fixture: ${error.stack}\n`;
      res.writeHead(500).end('Fixture error');
    }
  });
  try {
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    const agentDir = join(dir, 'agent'); await mkdir(agentDir);
    await writeFile(join(agentDir, 'models.json'), JSON.stringify({ providers: { 'question-smoke': {
      baseUrl: `http://127.0.0.1:${server.address().port}/v1`, api: 'openai-completions', apiKey: 'local-test-placeholder',
      compat: { supportsDeveloperRole: false, supportsUsageInStreaming: false },
      models: [{ id: 'echo', reasoning: false, contextWindow: 128000, maxTokens: 2048 }],
    } } }));
    await writeFile(join(agentDir, 'settings.json'), JSON.stringify({ defaultProvider: 'question-smoke', defaultModel: 'echo', enableInstallTelemetry: false, retry: { enabled: false } }));
    const extension = process.env.PI_QUESTION_TEST_EXTENSION ?? (process.env.PI_HARNESS_TEST_HOME_FILES
      ? join(process.env.PI_HARNESS_TEST_HOME_FILES, '.pi/agent/extensions/question.ts')
      : fileURLToPath(new URL('../home/config/pi/extensions/question.ts', import.meta.url)));
    child = spawn(binary, ['--mode', 'rpc', '--offline', '--no-extensions', '--no-skills', '--no-prompt-templates', '--no-context-files', '-e', extension, '-e', join(dirname(extension), 'subagent/index.ts')],
      { cwd: dir, env: { ...process.env, PATH: `${dirname(binary)}:${process.env.PATH}`, PI_CODING_AGENT_DIR: agentDir, PI_OFFLINE: '1', CAPTURE_PROMPTS: '0' }, stdio: ['pipe', 'pipe', 'pipe'] });
    child.on('error', error => { stderr += `Pi startup failed: ${error.code}\n`; });
    child.stderr.setEncoding('utf8').on('data', text => { stderr += text; });
    child.stdout.setEncoding('utf8').on('data', text => {
      buffer += text;
      let newline;
      while ((newline = buffer.indexOf('\n')) >= 0) {
        const line = buffer.slice(0, newline); buffer = buffer.slice(newline + 1);
        if (!line.trim()) continue;
        const event = JSON.parse(line);
        if (event.type === 'response') pending.get(event.id)?.(event);
      }
    });
    function request(type, args = {}) {
      return new Promise((resolve, reject) => {
        const id = String(++sequence);
        const timer = setTimeout(() => { pending.delete(id); reject(Error(`Timeout ${type}: ${stderr}`)); }, 15000);
        pending.set(id, event => { clearTimeout(timer); pending.delete(id); event.success ? resolve(event.data) : reject(Error(JSON.stringify(event))); });
        child.stdin.write(JSON.stringify({ id, type, ...args }) + '\n');
      });
    }
    await request('prompt', { message: 'hello' });
    for (let i = 0; i < 80 && histories.length === 0; i++) await delay(25);
    assert.equal(histories.length > 0, true, `no model request: ${stderr}`);
    const names = histories[0].tools.map(tool => tool.function.name);
    for (const name of ['question', 'subagent', 'team']) {
      assert.equal(names.filter(value => value === name).length, 1, `${name} must register once: ${names.join(',')}`);
    }
    assert.doesNotMatch(stderr, /Failed to load extension|Extension errors/i);
    const question = histories[0].tools.find(tool => tool.function.name === 'question');
    const required = question.function.parameters?.required ?? [];
    assert.ok(required.includes('options'), JSON.stringify(question.function.parameters));
    const system = histories[0].messages.filter(message => ['system', 'developer'].includes(message.role)).map(message => message.content).join('\n');
    assert.match(system, /question tool/);
  } finally {
    if (child && child.exitCode === null) { child.kill('SIGTERM'); await new Promise(resolve => child.once('exit', resolve)); }
    await new Promise(resolve => { server.close(resolve); server.closeAllConnections(); });
    await rm(dir, { recursive: true, force: true });
  }
});
