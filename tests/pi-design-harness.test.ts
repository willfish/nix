import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { mkdtemp, mkdir, readFile, readdir, rm, symlink, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import test from 'node:test';
import { searchCatalog } from '../home/config/pi/extensions/skill-catalog/catalog.ts';

const repo = fileURLToPath(new URL('..', import.meta.url));
const index = fileURLToPath(new URL('../home/config/pi/extensions/subagent/index.ts', import.meta.url));
const agentsPath = fileURLToPath(new URL('../home/config/pi/extensions/subagent/agents.ts', import.meta.url));
const piBin = process.env.PI_TEAM_TEST_BIN ?? 'pi';
const homeFiles = process.env.PI_HARNESS_TEST_HOME_FILES;
const ROLES = ['interface-designer', 'dashboard-designer', 'communication-designer'];
const ALLOWED = ['read', 'grep', 'find', 'ls', 'bash', 'edit', 'write', 'skill_catalog', 'mcp'];
const RECURSIVE = ['subagent', 'team', 'ask_coordinator'];
const PNG = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==', 'base64');
const IMAGE_URL = `data:image/png;base64,${PNG.toString('base64')}`;

function roots() {
  if (!homeFiles) {
    return {
      agents: join(repo, 'home/config/pi/agents'),
      skillDir: join(repo, 'home/config/llm/skills/design-workflow'),
      verifyDir: join(repo, 'home/config/llm/process-skills/verification-before-completion'),
      prompt: join(repo, 'home/config/pi/prompts/design.md'),
      skillCatalog: join(repo, 'home/config/pi/extensions/skill-catalog'),
      tui: join(repo, 'home/config/llm/skills/tui-design/SKILL.md'),
      dashboards: join(repo, 'home/config/llm/guides/dashboards.md'),
    };
  }
  return {
    agents: join(homeFiles, '.pi/agent/agents'),
    skillDir: join(homeFiles, '.agents/skills/design-workflow'),
    verifyDir: join(homeFiles, '.agents/skills/verification-before-completion'),
    prompt: join(homeFiles, '.pi/agent/prompts/design.md'),
    skillCatalog: join(homeFiles, '.pi/agent/extensions/skill-catalog'),
    tui: join(homeFiles, '.agents/skills/tui-design/SKILL.md'),
    dashboards: join(homeFiles, '.agents/guides/dashboards.md'),
  };
}

function stripFrontmatter(text) {
  return text.replace(/^---\n[\s\S]*?\n---\n/, '');
}

function expandDesign(template, args) {
  return stripFrontmatter(template).replaceAll('$@', args).replaceAll('$ARGUMENTS', args);
}

function userText(payload) {
  const message = payload.messages.find(item => item.role === 'user');
  if (!message) return '';
  return typeof message.content === 'string'
    ? message.content
    : message.content.map(part => part.text ?? '').join('');
}

function systemText(payload) {
  return payload.messages
    .filter(message => message.role === 'system' || message.role === 'developer')
    .map(message => typeof message.content === 'string' ? message.content : JSON.stringify(message.content))
    .join('\n');
}

function toolNames(payload) {
  return (payload.tools ?? []).map(tool => tool.function.name).sort();
}

function imageUrls(payload) {
  const found = [];
  const walk = value => {
    if (!value || typeof value !== 'object') return;
    if (value.type === 'image_url' && typeof value.image_url?.url === 'string') found.push(value.image_url.url);
    for (const child of Object.values(value)) walk(child);
  };
  walk(payload.messages);
  return found;
}

const RELATIVE_REFERENCES = ['references/interfaces.md', 'references/native-mobile.md',
  'references/communication.md', 'references/tools.md', 'references/quality.md'];

function assertSkillReferencePaths(skillText) {
  const bare = [...skillText.matchAll(/(?<!references\/)\b(?:interfaces|native-mobile|communication|tools|quality)\.md\b/g)];
  assert.deepEqual(bare.map(match => match[0]), []);
  for (const relative of RELATIVE_REFERENCES) assert.ok(skillText.includes(relative), relative);
}

async function loopback(t) {
  const histories = [];
  const peers = [];
  const state = { image: '' };
  const server = createServer(async (request, response) => {
    peers.push(request.socket.remoteAddress);
    if (request.method !== 'POST' || request.url !== '/v1/chat/completions') {
      response.writeHead(404).end();
      return;
    }
    let body = '';
    for await (const chunk of request) {
      body += chunk;
      if (body.length > 8 * 1024 * 1024) throw new Error('Oversized design fixture request');
    }
    const payload = JSON.parse(body);
    histories.push(structuredClone(payload));
    const encoded = JSON.stringify(payload.messages);
    const role = encoded.match(/DESIGN_ROLE_([a-z-]+)/)?.[1];
    const image = imageUrls(payload).includes(IMAGE_URL);
    const mcp = role && encoded.includes(`DESIGN_MCP_SENTINEL:${role}`);
    let delta;
    let finish;
    if (role && !image) {
      delta = { role: 'assistant', tool_calls: [{ index: 0, id: `call_read_${histories.length}`, type: 'function', function: { name: 'read', arguments: JSON.stringify({ path: state.image }) } }] };
      finish = 'tool_calls';
    } else if (role && !mcp) {
      delta = { role: 'assistant', tool_calls: [{ index: 0, id: `call_mcp_${histories.length}`, type: 'function', function: { name: 'mcp', arguments: JSON.stringify({ tool: 'fixture_echo', args: { role, text: 'synthetic-mcp' } }) } }] };
      finish = 'tool_calls';
    } else {
      delta = { role: 'assistant', content: role ? `DESIGN_STOP:${role}` : 'design-expand-ack' };
      finish = 'stop';
    }
    response.writeHead(200, { 'content-type': 'text/event-stream', 'cache-control': 'no-cache' });
    const chunk = (next, reason = null) => response.write(`data: ${JSON.stringify({
      id: `design-${histories.length}`, object: 'chat.completion.chunk', created: 1, model: 'echo',
      choices: [{ index: 0, delta: next, finish_reason: reason }],
    })}\n\n`);
    chunk(delta);
    chunk({}, finish);
    response.end('data: [DONE]\n\n');
  });
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  t.after(() => new Promise((resolve, reject) => {
    server.close(error => error ? reject(error) : resolve());
    server.closeAllConnections();
  }));
  return { histories, peers, port: server.address().port, state };
}

async function fixture(t, server, skills) {
  const source = roots();
  const dir = await mkdtemp(join(tmpdir(), 'pi-design-harness-'));
  t.after(() => rm(dir, { recursive: true, force: true }));
  const home = join(dir, 'home');
  const agentDir = join(home, '.pi', 'agent');
  const cwd = join(dir, 'project');
  const sourceProject = join(dir, 'source-project');
  const image = join(cwd, 'pixel.png');
  const mcpLog = join(dir, 'mcp-calls.jsonl');
  const launches = join(dir, 'launches.jsonl');
  await mkdir(join(agentDir, 'agents'), { recursive: true });
  await mkdir(join(agentDir, 'extensions'), { recursive: true });
  await mkdir(join(sourceProject, '.pi/agents'), { recursive: true });
  await mkdir(cwd);
  await writeFile(image, PNG);
  await symlink(source.skillCatalog, join(agentDir, 'extensions', 'skill-catalog'));
  await writeFile(join(agentDir, 'extensions', 'record-start.ts'), `
    import { appendFileSync } from 'node:fs';
    export default function(pi) {
      pi.on('session_start', (_event, ctx) => {
        appendFileSync(${JSON.stringify(launches)}, JSON.stringify({ pid: process.pid, mode: ctx.mode }) + '\\n');
      });
    }
  `);
  await writeFile(join(agentDir, 'extensions', 'mock-mcp.ts'), `
    import { appendFileSync } from 'node:fs';
    export default function(pi) {
      pi.registerTool({
        name: 'mcp',
        label: 'MCP',
        description: 'Local synthetic MCP gateway. No network and no real server.',
        parameters: {
          type: 'object', additionalProperties: true,
          properties: {
            tool: { type: 'string' },
            args: { type: 'object', additionalProperties: true },
          },
        },
        async execute(_id, params) {
          if (params?.tool !== 'fixture_echo' || params.args?.text !== 'synthetic-mcp' || !params.args?.role) {
            throw new Error('Unexpected synthetic MCP call');
          }
          appendFileSync(${JSON.stringify(mcpLog)}, JSON.stringify({ tool: params.tool, role: params.args.role }) + '\\n');
          return { content: [{ type: 'text', text: 'DESIGN_MCP_SENTINEL:' + params.args.role }], details: { synthetic: true } };
        },
      });
    }
  `);
  for (const role of ROLES) {
    const original = await readFile(join(source.agents, `${role}.md`), 'utf8');
    assert.match(original, /^model: openai-codex\/gpt-6-astra$/m, role);
    await writeFile(join(sourceProject, '.pi/agents', `${role}.md`), original);
    const copied = original.replace(/^model: openai-codex\/gpt-6-astra$/m, 'model: team-smoke/echo');
    assert.equal(copied.replace('model: team-smoke/echo', 'model: openai-codex/gpt-6-astra'), original);
    await writeFile(join(agentDir, 'agents', `${role}.md`), copied);
  }
  await writeFile(join(agentDir, 'models.json'), JSON.stringify({ providers: { 'team-smoke': {
    baseUrl: `http://127.0.0.1:${server.port}/v1`, api: 'openai-completions', apiKey: 'local-test-placeholder',
    models: [{ id: 'echo', reasoning: true, input: ['text', 'image'], contextWindow: 128000, maxTokens: 512,
      thinkingLevelMap: { off: null, minimal: null, low: 'low', medium: 'medium', high: 'high', xhigh: null, max: null },
      cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0 } }],
  } } }));
  await writeFile(join(agentDir, 'settings.json'), JSON.stringify({
    defaultProvider: 'team-smoke', defaultModel: 'echo', enableSkillCommands: true,
    defaultProjectTrust: 'never', retry: { enabled: false }, compaction: { enabled: false },
  }));
  const env = {
    PATH: process.env.PATH, HOME: home, XDG_CONFIG_HOME: join(home, '.config'),
    PI_CODING_AGENT_DIR: agentDir, PI_OFFLINE: '1', PI_TELEMETRY: '0', CAPTURE_PROMPTS: '0',
    TERM: 'xterm-256color', LANG: 'C.UTF-8',
  };
  return { dir, cwd, image, mcpLog, launches, sourceProject, env, skills, source };
}

async function cli(f, args) {
  return new Promise((resolve, reject) => {
    const child = spawn(piBin, args, { cwd: f.cwd, env: f.env, stdio: ['ignore', 'pipe', 'pipe'] });
    let stdout = '', stderr = '';
    const timer = setTimeout(() => child.kill('SIGKILL'), 150000);
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

function skillArgs(paths) {
  return paths.flatMap(path => ['--skill', path]);
}

test('every role can use conditional design guidance without gaining defaults or delegation authority', async () => {
  const source = roots();
  const names = (await readdir(source.agents)).filter(name => name.endsWith('.md'));
  assert.equal(names.length, 13);
  for (const name of names) {
    const text = await readFile(join(source.agents, name), 'utf8');
    assert.equal(text, await readFile(join(repo, 'home/config/pi/agents', name), 'utf8'), name);
    const header = text.split('\n---\n')[0];
    const body = stripFrontmatter(text);
    assert.match(body, /design-workflow/, name);
    if (!ROLES.includes(name.replace(/\.md$/, ''))) {
      assert.match(body, /For user-facing interface, dashboard, deck or marketing work/, name);
      assert.match(body, /within this role's scope and permissions/, name);
      assert.match(body, /through the coordinator, without recursive delegation or broader file ownership/, name);
      assert.doesNotMatch(header, /^skills:.*design-workflow/m, name);
    }
    if (name === 'scout.md') assert.match(body, /read selected SKILL\.md instructions in full/);
  }
  const path = homeFiles ? join(homeFiles, '.pi/agent/ORCHESTRATOR.md') : join(repo, 'home/config/llm/ORCHESTRATOR.md');
  const guidance = await readFile(path, 'utf8');
  assert.equal(guidance, await readFile(join(repo, 'home/config/llm/ORCHESTRATOR.md'), 'utf8'));
  assert.match(guidance, /read `design-workflow` before choosing roles/);
  assert.match(guidance, /when specialist design work adds value; otherwise apply the guidance directly/);
  assert.match(guidance, /not in a fixed pipeline or recursive child teams/);
});

test('design metadata covers all seven surfaces without backend-only keyword matches', async () => {
  const filePath = join(roots().skillDir, 'SKILL.md');
  const source = await readFile(filePath, 'utf8');
  const description = source.match(/^description: (.+)$/m)[1];
  const registry = [{ name: 'design-workflow', description, filePath }];
  const commands = [{ name: 'skill:design-workflow', source: 'skill', sourceInfo: { path: filePath } }];
  assert.ok(description.length <= 180, 'Keep the entire trigger visible in catalogue results');
  for (const query of ['TUI', 'dashboards', 'native', 'web', 'mobile', 'decks', 'marketing']) {
    const result = searchCatalog(registry, commands, { query }).results[0];
    assert.equal(result?.name, 'design-workflow', query);
    assert.equal(result.command, '/skill:design-workflow');
    assert.equal(result.manualOnly, false);
  }
  assert.equal(searchCatalog(registry, commands, { query: 'backend checksum refactor' }).total, 0);
});

test('design roles dispatch through real Pi with one full shared skill, declared tools, synthetic MCP and image bytes', { timeout: 180000 }, async t => {
  const source = roots();
  const server = await loopback(t);
  const f = await fixture(t, server, [source.skillDir, source.verifyDir]);
  const output = join(f.dir, 'report.json');
  const extension = join(f.dir, 'probe.ts');
  const designSkill = await readFile(join(source.skillDir, 'SKILL.md'), 'utf8');
  assertSkillReferencePaths(designSkill);
  for (const relative of [...RELATIVE_REFERENCES, 'scripts/contrast.py']) {
    const body = await readFile(join(source.skillDir, relative), 'utf8');
    assert.ok(body.length > 0, relative);
    if (relative.endsWith('.md')) {
      assert.doesNotMatch(body, /(?<!references\/)\b(?:interfaces|native-mobile|communication|tools|quality)\.md\b/);
      for (const match of body.matchAll(/references\/[a-z-]+\.md/g)) {
        assert.ok((await readFile(join(source.skillDir, match[0]), 'utf8')).length > 0, match[0]);
      }
    }
  }
  const verifySkill = await readFile(join(source.verifyDir, 'SKILL.md'), 'utf8');
  const absent = [
    (await readFile(join(source.skillDir, 'references/interfaces.md'), 'utf8')).match(/^Map the main journey.+$/m)[0],
    (await readFile(join(source.skillDir, 'references/native-mobile.md'), 'utf8')).match(/^A phone-shaped browser preview.+$/m)[0],
    (await readFile(join(source.skillDir, 'references/communication.md'), 'utf8')).match(/Distinguish a live talk from/)[0],
    (await readFile(join(source.skillDir, 'references/tools.md'), 'utf8')).match(/Missing tools use ephemeral Nix.+$/m)[0],
    (await readFile(join(source.skillDir, 'references/quality.md'), 'utf8')).match(/not an aesthetic score/)[0],
    (await readFile(join(source.skillDir, 'scripts/contrast.py'), 'utf8')).match(/WCAG relative luminance/)[0],
    'quiet, fast, keyboard-first control surface',
    'Shared guidance for CloudWatch and Grafana dashboard design',
  ];
  await writeFile(extension, `
    import install from ${JSON.stringify(index)};
    import { discoverAgents } from ${JSON.stringify(agentsPath)};
    import { writeFileSync } from 'node:fs';
    export default function(pi) {
      const tools = new Map();
      install(new Proxy(pi, { get(target, key) {
        if (key === 'registerTool') return tool => { tools.set(tool.name, tool); target.registerTool(tool); };
        return target[key];
      }}));
      pi.registerCommand('design-probe', { handler: async (_args, ctx) => {
        const summarize = agent => ({ name: agent.name, description: agent.description, tools: agent.tools,
          skills: agent.skills, model: agent.model, thinking: agent.thinking, systemPrompt: agent.systemPrompt });
        const report = {
          fixtureAgents: discoverAgents(${JSON.stringify(f.cwd)}, 'user').agents.map(summarize),
          sourceAgents: discoverAgents(${JSON.stringify(f.sourceProject)}, 'project').agents.map(summarize),
          results: [],
        };
        const invoke = params => tools.get('subagent').execute('design-check', params, undefined, undefined, ctx);
        try {
          for (const agent of ${JSON.stringify(ROLES)}) {
            report.results.push(await invoke({ agent, task: 'DESIGN_ROLE_' + agent + ' critique only. Do not edit.' }));
          }
        } catch (error) { report.fatal = error.stack ?? String(error); }
        writeFileSync(${JSON.stringify(output)}, JSON.stringify(report));
      }});
    }
  `);
  const before = await readdir(f.cwd);
  server.state.image = f.image;
  await cli(f, ['--offline', '--no-extensions', '--no-skills', '--no-prompt-templates', '--no-context-files',
    '--no-builtin-tools', '--model', 'team-smoke/echo', '--thinking', 'off', '--no-session',
    ...skillArgs(f.skills), '-e', extension, '-p', '/design-probe']);
  const report = JSON.parse(await readFile(output, 'utf8'));
  assert.equal(report.fatal, undefined, report.fatal);
  assert.deepEqual(report.sourceAgents.map(agent => agent.name).sort(), [...ROLES].sort());
  for (const agent of report.sourceAgents) {
    assert.deepEqual(agent.tools, ALLOWED);
    assert.deepEqual(agent.skills, ['design-workflow', 'verification-before-completion']);
    assert.equal(agent.model, 'openai-codex/gpt-6-astra');
    assert.equal(agent.thinking, 'high');
    assert.match(agent.systemPrompt, /Advice and critique are read-only/);
    assert.match(agent.systemPrompt, /Do not delegate recursively/);
  }
  for (const agent of report.fixtureAgents) {
    assert.equal(agent.model, 'team-smoke/echo');
    assert.equal(agent.thinking, 'high');
    assert.deepEqual(agent.tools, ALLOWED);
  }
  assert.equal(report.results.length, ROLES.length);
  for (const result of report.results) {
    assert.equal(result.details.mode, 'single');
    const [child] = result.details.results;
    assert.equal(child.exitCode, 0, child.stderr);
    assert.equal(child.model, 'team-smoke/echo');
    assert.doesNotMatch(child.stderr, /Failed to load extension|Extension errors/i);
    assert.match(result.content[0].text, new RegExp(`DESIGN_STOP:${child.agent}`));
  }
  const launches = (await readFile(f.launches, 'utf8')).trim().split('\n').map(line => JSON.parse(line));
  assert.equal(launches.length, ROLES.length);
  assert.deepEqual(launches.map(launch => launch.mode), ROLES.map(() => 'json'));
  assert.equal(new Set(launches.map(launch => launch.pid)).size, ROLES.length);
  const calls = (await readFile(f.mcpLog, 'utf8')).trim().split('\n').map(line => JSON.parse(line));
  assert.deepEqual(calls.map(call => call.role).sort(), [...ROLES].sort());
  assert.deepEqual(calls.map(call => call.tool), ROLES.map(() => 'fixture_echo'));
  assert.deepEqual(await readdir(f.cwd), before);
  assert.deepEqual(await readFile(f.image), PNG);
  assert.deepEqual([...new Set(server.peers)], ['127.0.0.1']);
  const byRole = new Map();
  for (const payload of server.histories) {
    assert.equal(payload.model, 'echo');
    assert.equal(payload.reasoning_effort, 'high');
    const role = JSON.stringify(payload.messages).match(/DESIGN_ROLE_([a-z-]+)/)[1];
    byRole.set(role, [...(byRole.get(role) ?? []), payload]);
  }
  assert.deepEqual([...byRole.keys()].sort(), [...ROLES].sort());
  for (const [role, payloads] of byRole) {
    assert.equal(payloads.length, 3, role);
    for (const payload of payloads) {
      assert.deepEqual(toolNames(payload), [...ALLOWED].sort());
      for (const name of RECURSIVE) assert.equal(toolNames(payload).includes(name), false, name);
      const catalog = payload.tools.find(tool => tool.function.name === 'skill_catalog');
      const mcp = payload.tools.find(tool => tool.function.name === 'mcp');
      assert.match(catalog.function.description, /Search trusted loaded skill metadata/);
      assert.match(mcp.function.description, /Local synthetic MCP gateway/);
      const system = systemText(payload);
      assert.equal(system.split(designSkill).length, 2, `${role} shared skill must appear once`);
      assert.equal(system.split(verifySkill).length, 2);
      assert.match(system, new RegExp(`You are the ${role.replaceAll('-', ' ')}`));
      assert.match(system, /Advice and critique are read-only/);
      assert.match(system, /Blocked: <action>; requires:/);
      assert.match(system, /Creation, repairs, source edits, round-trip edits and persisted handoffs/);
      assert.match(system, /apply only to implementation/);
      assert.match(system, /An empty brief always warrants a proposed brief without product changes/);
      assert.match(system, /even when context identifies an artifact/);
      const bases = [...system.matchAll(/^Reference base directory: (.+)$/gm)].map(match => match[1]);
      const designBase = bases.find(base => base === source.skillDir || base.endsWith('/design-workflow'));
      assert.equal(designBase, source.skillDir);
      for (const relative of RELATIVE_REFERENCES) {
        assert.ok(system.includes(relative), relative);
        assert.equal(system.includes(await readFile(join(designBase, relative), 'utf8')), false, relative);
      }
      for (const other of ROLES.filter(name => name !== role)) {
        assert.equal(system.includes(`You are the ${other.replaceAll('-', ' ')}`), false, other);
      }
      for (const line of absent) assert.equal(system.includes(line), false, line);
      assert.match(userText(payload), /critique only\. Do not edit\./);
    }
    assert.deepEqual(imageUrls(payloads[0]), []);
    assert.deepEqual(imageUrls(payloads[1]), [IMAGE_URL]);
    assert.deepEqual(imageUrls(payloads[2]), [IMAGE_URL]);
    assert.equal(JSON.stringify(payloads[1].messages).includes(`DESIGN_MCP_SENTINEL:${role}`), false);
    assert.equal(JSON.stringify(payloads[2].messages).includes(`DESIGN_MCP_SENTINEL:${role}`), true);
    const readCall = JSON.stringify(payloads[1].messages);
    assert.match(readCall, new RegExp(f.image.replaceAll('/', '\\/')));
    assert.equal(imageUrls(payloads[1]).some(url => url.includes(f.image) || url.startsWith('file:')), false);
  }
});

test('missing design skill rejects the real role before a child or provider call', { timeout: 60000 }, async t => {
  const source = roots();
  const server = await loopback(t);
  const f = await fixture(t, server, [source.verifyDir]);
  const output = join(f.dir, 'missing.json');
  const extension = join(f.dir, 'missing.ts');
  await writeFile(extension, `
    import install from ${JSON.stringify(index)};
    import { writeFileSync, readFileSync, existsSync } from 'node:fs';
    export default function(pi) {
      const tools = new Map();
      install(new Proxy(pi, { get(target, key) {
        if (key === 'registerTool') return tool => { tools.set(tool.name, tool); target.registerTool(tool); };
        return target[key];
      }}));
      pi.registerCommand('missing-probe', { handler: async (_args, ctx) => {
        const report = {};
        try {
          report.unexpected = await tools.get('subagent').execute('missing-check',
            { agent: 'interface-designer', task: 'DESIGN_ROLE_interface-designer critique only' }, undefined, undefined, ctx);
        } catch (error) { report.error = error.message; }
        report.launches = existsSync(${JSON.stringify(f.launches)})
          ? readFileSync(${JSON.stringify(f.launches)}, 'utf8') : '';
        writeFileSync(${JSON.stringify(output)}, JSON.stringify(report));
      }});
    }
  `);
  await cli(f, ['--offline', '--no-extensions', '--no-skills', '--no-prompt-templates', '--no-context-files',
    '--no-builtin-tools', '--model', 'team-smoke/echo', '--thinking', 'off', '--no-session',
    ...skillArgs(f.skills), '-e', extension, '-p', '/missing-probe']);
  const report = JSON.parse(await readFile(output, 'utf8'));
  assert.equal(report.unexpected, undefined);
  assert.match(report.error, /interface-designer/);
  assert.match(report.error, /design-workflow/);
  assert.match(report.error, /active Pi skills/);
  assert.equal(report.launches, '');
  assert.equal(server.histories.length, 0);
});

test('expands /design as a prompt template without treating the loopback reply as routing', { timeout: 60000 }, async t => {
  const source = roots();
  const server = await loopback(t);
  const f = await fixture(t, server, []);
  const template = await readFile(source.prompt, 'utf8');
  const skill = await readFile(join(source.skillDir, 'SKILL.md'), 'utf8');
  const brief = 'TUI: keyboard search with a long empty state';
  const args = ['--offline', '--no-extensions', '--no-skills', '--no-context-files', '--no-builtin-tools',
    '--model', 'team-smoke/echo', '--thinking', 'off', '--no-session', '--mode', 'json',
    '--prompt-template', source.prompt, '-p'];
  const runs = [];
  for (const prompt of [`/design ${brief}`, '/design', 'backend-only refactor, not a slash command']) {
    runs.push(await cli(f, [...args, prompt]));
  }
  assert.equal(server.histories.length, 3);
  const briefText = userText(server.histories[0]);
  const emptyText = userText(server.histories[1]);
  assert.equal(briefText.replace(/\s+$/, ''), expandDesign(template, brief).replace(/\s+$/, ''));
  assert.equal(emptyText.replace(/\s+$/, ''), expandDesign(template, '').replace(/\s+$/, ''));
  assert.equal(briefText.split('Use the design-workflow skill for this request:\n\n')[1].split('\n\nRead the skill')[0], brief);
  assert.equal(emptyText.split('Use the design-workflow skill for this request:\n\n')[1].split('\n\nRead the skill')[0], '');
  assert.match(emptyText, /If the arguments are empty, always inspect only enough to propose a concrete brief and safe direction, even when current context identifies an artifact/);
  assert.match(emptyText, /Do not dispatch arbitrary implementation or modify product files/);
  assert.equal(userText(server.histories[2]), 'backend-only refactor, not a slash command');
  for (const [index, payload] of server.histories.entries()) {
    const events = runs[index].stdout.split('\n').filter(line => line.startsWith('{')).map(line => JSON.parse(line));
    const assistant = events.find(event => event.type === 'message_end' && event.message?.role === 'assistant')?.message;
    assert.equal(assistant?.content?.[0]?.text, 'design-expand-ack');
    assert.equal(assistant.stopReason, 'stop');
    assert.equal(systemText(payload).includes(skill), false);
    assert.equal((payload.tools ?? []).some(tool => tool.function.name === 'subagent'), false);
    assert.equal(JSON.stringify(payload.messages).includes('tool_calls'), false);
  }
});

test('candidate generation design files match the repo contracts when a home-files path is provided', {
  skip: !homeFiles && 'Set PI_HARNESS_TEST_HOME_FILES to generation/home-files',
}, async () => {
  const deployed = roots();
  const repoRoots = {
    agents: join(repo, 'home/config/pi/agents'),
    skillDir: join(repo, 'home/config/llm/skills/design-workflow'),
    prompt: join(repo, 'home/config/pi/prompts/design.md'),
  };
  for (const role of ROLES) {
    assert.equal(await readFile(join(deployed.agents, `${role}.md`), 'utf8'),
      await readFile(join(repoRoots.agents, `${role}.md`), 'utf8'), role);
  }
  assert.equal(await readFile(deployed.prompt, 'utf8'), await readFile(repoRoots.prompt, 'utf8'));
  for (const relative of ['SKILL.md', 'references/interfaces.md', 'references/native-mobile.md',
    'references/communication.md', 'references/tools.md', 'references/quality.md', 'scripts/contrast.py']) {
    assert.equal(await readFile(join(deployed.skillDir, relative), 'utf8'),
      await readFile(join(repoRoots.skillDir, relative), 'utf8'), relative);
  }
});
