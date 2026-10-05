import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { copyFile, mkdir, mkdtemp, readFile, rm, symlink, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import test from 'node:test';

const repo = new URL('..', import.meta.url);
const homeFiles = process.env.PI_HARNESS_TEST_HOME_FILES;
const binary = process.env.PI_MCP_TEST_BIN;
const expectedTools = ['read', 'bash', 'edit', 'write', 'team', 'subagent', 'get_goal', 'create_goal',
  'update_goal', 'skill_catalog', 'mcp', 'todo', 'get_coordination_guidance', 'report_work', 'list_agents',
  'set_agent_label', 'list_channels', 'set_coordination_scope', 'read_channel', 'post_channel',
  'update_channel_status', 'send_agent_message', 'question'];

// Captures the actual request after all deployed extensions. Isolated HOME, synthetic
// credentials, loopback inference and an empty MCP configuration prevent live access.
test('candidate full-capability payload is compact and remains discoverable across turns and resume', {
  timeout: 60000,
  skip: !homeFiles || !binary ? 'Set PI_HARNESS_TEST_HOME_FILES and PI_MCP_TEST_BIN to the candidate generation' : false,
}, async t => {
  const root = await mkdtemp(join(tmpdir(), 'pi-harness-payload-'));
  t.after(() => rm(root, { recursive: true, force: true }));
  const profile = join(root, '.pi/agent');
  await mkdir(profile, { recursive: true });
  await mkdir(join(root, '.agents'), { recursive: true });
  await mkdir(join(root, '.config/mcp'), { recursive: true });
  await symlink(join(homeFiles!, '.agents/skills'), join(root, '.agents/skills'));
  await symlink(join(homeFiles!, '.pi/agent/extensions'), join(profile, 'extensions'));
  for (const name of ['AGENTS.md', 'ORCHESTRATOR.md']) {
    await copyFile(join(homeFiles!, '.pi/agent', name), join(profile, name));
  }
  await copyFile(new URL('AGENTS.md', repo), join(root, 'AGENTS.md'));
  await writeFile(join(root, '.config/mcp/mcp.json'), JSON.stringify({
    mcpServers: {}, settings: { hostConfigDiscovery: 'off', directTools: false, namespaceProxyTools: false, scriptMode: false },
  }));
  await writeFile(join(profile, 'settings.json'), JSON.stringify({
    defaultProjectTrust: 'no', extensions: ['-builtin:mcp'], retry: { enabled: false },
  }));
  const requests: any[] = [];
  const server = createServer(async (req, res) => {
    let body = '';
    for await (const chunk of req) body += chunk;
    const payload = JSON.parse(body);
    requests.push(payload);
    const last = payload.messages.at(-1);
    const delta = last.role === 'tool'
      ? { role: 'assistant', content: 'Fixture complete.' }
      : { role: 'assistant', tool_calls: [{ index: 0, id: `fixture-${requests.length}`, type: 'function',
        function: { name: 'skill_catalog', arguments: JSON.stringify({ query: JSON.stringify(last.content).includes('installation path') ? 'tmp' : 'local-dev-environment' }) } }] };
    res.writeHead(200, { 'content-type': 'text/event-stream' });
    for (const [content, finish] of [[delta, null], [{}, last.role === 'tool' ? 'stop' : 'tool_calls']]) {
      res.write(`data: ${JSON.stringify({ id: 'fixture', object: 'chat.completion.chunk', created: 1,
        model: 'fixture', choices: [{ index: 0, delta: content, finish_reason: finish }] })}\n\n`);
    }
    res.end('data: [DONE]\n\n');
  });
  await new Promise<void>(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(() => new Promise<void>(resolve => { server.close(() => resolve()); server.closeAllConnections(); }));
  const port = (server.address() as { port: number }).port;
  await writeFile(join(profile, 'models.json'), JSON.stringify({ providers: { fixture: {
    baseUrl: `http://127.0.0.1:${port}/v1`, api: 'openai-completions', apiKey: 'synthetic',
    models: [{ id: 'fixture', name: 'Fixture', reasoning: false, input: ['text'], contextWindow: 128000,
      maxTokens: 1024, cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0 } }],
  } } }));
  const session = join(root, 'session.jsonl');
  const run = async (prompt: string) => {
    const child = spawn(binary!, ['--offline', '--provider', 'fixture', '--model', 'fixture', '--thinking', 'off',
      '--no-prompt-templates', '--no-themes', '--session', session, '-p', prompt], {
      cwd: root,
      env: { PATH: process.env.PATH, HOME: root, XDG_CONFIG_HOME: join(root, '.config'),
        PI_CODING_AGENT_DIR: profile, PI_OFFLINE: '1', PI_TELEMETRY: '0', CAPTURE_PROMPTS: '0' },
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    let output = '';
    child.stdout.on('data', data => { output += data; });
    child.stderr.on('data', data => { output += data; });
    const timer = setTimeout(() => child.kill('SIGKILL'), 20000);
    try {
      const code = await new Promise((resolve, reject) => { child.on('error', reject); child.on('close', resolve); });
      assert.equal(code, 0, output);
      assert.doesNotMatch(output, /Extension error|Failed to load extension|registry unavailable/i);
    } finally { clearTimeout(timer); }
  };
  await run('Discover the local environment skill.');
  await run('Discover the same skill after resuming.');
  await run('Search only an installation path component.');
  assert.equal(requests.length, 6);
  const first = requests[0];
  const chars = first.messages.filter(m => ['system', 'developer'].includes(m.role)).reduce((n, m) => n + m.content.length, 0);
  t.diagnostic(`Static request: instructions=${chars}, schemas=${JSON.stringify(first.tools).length}, total=${chars + JSON.stringify(first.tools).length} characters; ${requests.length} requests including resume`);
  const shared = await readFile(new URL('home/config/llm/AGENTS.md', repo), 'utf8');
  const policy = '### Approval scope\n\n' + shared.split('### Approval scope\n\n')[1].split('\n\n## Verification')[0];
  for (const request of requests) {
    const instructions = request.messages.filter(m => ['system', 'developer'].includes(m.role)).map(m => m.content).join('');
    const schemas = JSON.stringify(request.tools);
    assert.deepEqual(request.tools.map(tool => tool.function.name).sort(), [...expectedTools].sort());
    assert.equal(/<available_skills>|Always read pi \.md files completely/.test(instructions), false,
      'The full catalogue and blanket documentation-reading policy must not reach the provider');
    assert.match(instructions, /Use skill_catalog to find task-relevant skills/);
    assert.match(instructions, /Read whole files only when needed for correctness/);
    assert.match(instructions, /You are the starting Pi agent for this session/);
    assert.match(instructions, /## Final voice summary/);
    assert.equal(instructions.split(policy).length - 1, 1);
    // Full current capability baseline: ~53.2k characters before the compatibility
    // fixes. Keep a tight 36k ceiling, rather than the obsolete 13-tool budget.
    assert.ok(instructions.length + schemas.length <= 36000,
      `Static payload ${instructions.length + schemas.length} exceeds 36000 characters`);
  }
  for (const request of [requests[1], requests[3]]) {
    const result = JSON.parse(request.messages.filter(m => m.role === 'tool').at(-1).content);
    assert.equal(result.total, 1);
    assert.equal(result.results[0].name, 'local-dev-environment');
    assert.equal(result.results[0].manualOnly, false);
    assert.match(result.results[0].path, /local-dev-environment\/SKILL\.md$/);
  }
  const pathSearch = JSON.parse(requests[5].messages.filter(m => m.role === 'tool').at(-1).content);
  assert.equal(pathSearch.total, 0);
  assert.deepEqual(pathSearch.results, []);
});
