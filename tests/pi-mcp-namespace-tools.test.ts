import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { stripTypeScriptTypes } from 'node:module';
import { dirname, join } from 'node:path';
import test from 'node:test';
import vm from 'node:vm';

// Run against the candidate package, not a second implementation of its gate:
// PI_MCP_TEST_EXTENSION=<package>/index.ts node --experimental-vm-modules --test tests/pi-mcp-namespace-tools.test.ts
const extension = process.env.PI_MCP_TEST_EXTENSION;

test('persona allowlists explicitly retain their permissions and permit skill catalog search', () => {
  const expected = {
    architect: 'read, grep, find, ls, bash, skill_catalog',
    builder: 'read, grep, find, ls, bash, edit, write, skill_catalog',
    'domain-specialist': 'read, grep, find, ls, bash, skill_catalog',
    planner: 'read, grep, find, ls, skill_catalog',
    reviewer: 'read, grep, find, ls, bash, skill_catalog',
    sceptic: 'read, grep, find, ls, bash, skill_catalog',
    scout: 'read, grep, find, ls, bash, skill_catalog',
    'security-reviewer': 'read, grep, find, ls, bash, skill_catalog',
    'test-engineer': 'read, grep, find, ls, bash, edit, write, skill_catalog',
  };
  for (const [role, tools] of Object.entries(expected)) {
    const source = readFileSync(new URL(`../home/config/pi/agents/${role}.md`, import.meta.url), 'utf8');
    assert.equal(source.split('---')[1].match(/^tools: (.+)$/m)?.[1], tools, role);
  }
  const worker = readFileSync(new URL('../home/config/pi/agents/worker.md', import.meta.url), 'utf8');
  assert.doesNotMatch(worker.split('---')[1], /^tools:/m);
});

test('packaged config preserves namespaceTools through normalization, merge and reload', {
  skip: !extension && 'Set PI_MCP_TEST_EXTENSION to the candidate packaged index.ts',
}, (t) => {
  const home = mkdtempSync(join(tmpdir(), 'pi-mcp-config-'));
  t.after(() => rmSync(home, { recursive: true, force: true }));
  execFileSync(process.execPath, ['--input-type=module', '-e', `
    import assert from 'node:assert/strict';
    import { mkdirSync, writeFileSync } from 'node:fs';
    import { join } from 'node:path';
    import { loadMcpConfig, cloneMcpConfig } from ${JSON.stringify(extension && join(dirname(extension), 'dist/config.js'))};
    const save = (path, settings) => writeFileSync(path, JSON.stringify({ mcpServers: {}, settings }));
    mkdirSync('.config/mcp', { recursive: true });
    mkdirSync('.pi', { recursive: true });
    const shared = join(process.cwd(), '.config/mcp/mcp.json');
    const project = join(process.cwd(), '.pi/mcp.json');
    save(shared, { namespaceTools: false, directTools: false, hostConfigDiscovery: 'off' });
    save(project, { idleTimeout: 10 });
    const config = loadMcpConfig();
    assert.equal(config.settings.namespaceTools, false);
    assert.equal(config.settings.idleTimeout, 10);
    assert.equal(cloneMcpConfig(config).settings.namespaceTools, false);
    save(project, { namespaceTools: true });
    assert.equal(loadMcpConfig().settings.namespaceTools, true);
    save(project, { namespaceTools: false });
    assert.equal(loadMcpConfig().settings.namespaceTools, false);
    save(shared, { directTools: false, hostConfigDiscovery: 'off' });
    save(project, {});
    assert.equal(loadMcpConfig().settings.namespaceTools, undefined);
  `], {
    cwd: home,
    env: { ...process.env, HOME: home, XDG_CONFIG_HOME: join(home, '.config'),
      PI_CODING_AGENT_DIR: join(home, '.pi/agent'), PI_PACKAGE_DIR: '',
      PI_MCP_CONFIG_MODE: '', PI_MCP_CONFIG: '' },
    stdio: 'pipe',
  });
});

test('packaged namespace registration lifecycle', {
  skip: !extension && 'Set PI_MCP_TEST_EXTENSION to the candidate packaged index.ts',
}, async (t) => {
  const context = vm.createContext({ console });
  const calls = [];
  const mocks = {
    typebox: { Type: { Object: (value) => value, String: (value) => value, Optional: (value) => value } },
    './types.ts': { isServerDisabled: (definition) => definition.enabled === false },
    './metadata-cache.ts': { isServerCacheValid: (entry) => entry.valid !== false },
    './proxy-modes.ts': { executeCall: (...args) => { calls.push(args); return { content: [], details: {} }; } },
    './mcp-references.ts': {
      namespaceProxyName: (name) => `mcp__${name}`,
      hasCallableCachedTargets: (entry) => entry.tools.length > 0,
      isMcpServerDirectlyRegistered: (definition, settings) => definition.directTools ?? settings?.directTools ?? false,
    },
    './tool-result-renderer.ts': {
      createMcpProxyToolCallRenderer: () => () => {},
      createMcpToolResultRenderer: () => () => {},
      resolveMcpToolRenderOptions: () => ({ resultRendering: 'compact' }),
    },
  };
  const source = readFileSync(join(dirname(extension), 'namespace-tools.ts'), 'utf8');
  const module = new vm.SourceTextModule(stripTypeScriptTypes(source), { context });
  await module.link((specifier) => {
    const exports = mocks[specifier];
    assert.ok(exports, `Unexpected dependency: ${specifier}`);
    return new vm.SyntheticModule(Object.keys(exports), function () {
      for (const [name, value] of Object.entries(exports)) this.setExport(name, value);
    }, { context });
  });
  await module.evaluate();
  const { syncNamespaceProxyTools: sync } = module.namespace;

  function fixture(settings, unregister = true) {
    const gateway = { name: 'mcp', execute: () => 'gateway' };
    const tools = new Map(['read', 'subagent', 'team', 'todo', 'skill'].map((name) => [name, { name }]));
    tools.set('mcp', gateway);
    let active = [...tools.keys()];
    const input = {
      config: { mcpServers: { fixture: { command: 'fixture' } }, ...(settings === undefined ? {} : { settings }) },
      cache: { servers: { fixture: { tools: [{ name: 'echo' }] } } },
      envOverride: null,
      existingDirectNames: new Set(),
      existingNamespaceNames: new Set(),
      pi: {
        registerTool(tool) { tools.set(tool.name, tool); if (!active.includes(tool.name)) active.push(tool.name); },
        getActiveTools: () => [...active],
        setActiveTools: (names) => { active = [...names]; },
        ...(unregister ? { unregisterTool: (name) => tools.delete(name) } : {}),
      },
      getState: () => ({ marker: 'real-state' }),
      getInitPromise: () => null,
      getPiTools: () => [...tools.values()],
    };
    function refresh() {
      const result = sync(input);
      for (const name of result.added) input.existingNamespaceNames.add(name);
      for (const name of result.deactivated) input.existingNamespaceNames.delete(name);
      return result;
    }
    return { input, refresh, tools, gateway, active: () => [...active] };
  }

  for (const [label, settings] of [
    ['settings absent', undefined],
    ['setting unspecified', { directTools: false }],
    ['explicitly enabled', { namespaceTools: true }],
  ]) {
    await t.test(label, () => {
      const f = fixture(settings);
      assert.deepEqual([...f.refresh().added], ['mcp__fixture']);
      assert.deepEqual([...f.refresh().updated], ['mcp__fixture']);
    });
  }

  await t.test('false generates no namespace tools and leaves the gateway and other tools untouched', () => {
    const f = fixture({ namespaceTools: false });
    const before = f.active();
    const result = f.refresh();
    assert.equal(result.specs.length, 0);
    assert.equal(result.added.length, 0);
    assert.deepEqual(f.active(), before);
    assert.equal(f.tools.get('mcp'), f.gateway);
    assert.equal(f.gateway.execute(), 'gateway');
  });

  for (const unregister of [true, false]) {
    await t.test(`refresh removes stale active namespaces, unregister API ${unregister ? 'present' : 'absent'}`, () => {
      const f = fixture({ namespaceTools: true }, unregister);
      const before = f.active();
      f.refresh();
      f.input.config.settings.namespaceTools = false;
      assert.deepEqual([...f.refresh().deactivated], ['mcp__fixture']);
      assert.deepEqual(f.active(), before);
      assert.equal(f.tools.get('mcp'), f.gateway);
      assert.equal(f.tools.has('mcp__fixture'), !unregister);
      assert.equal(f.input.existingNamespaceNames.size, 0);
      assert.equal(f.refresh().deactivated.length, 0);
      f.input.config.settings.namespaceTools = true;
      assert.deepEqual([...f.refresh().added], ['mcp__fixture']);
    });
  }

  await t.test('stale cleanup preserves a namespace name now owned by an active direct tool', () => {
    const f = fixture({ namespaceTools: true });
    f.refresh();
    f.input.config.settings.namespaceTools = false;
    f.input.activeDirectNames = new Set(['mcp__fixture']);
    assert.equal(f.refresh().deactivated.length, 0);
    assert.ok(f.active().includes('mcp__fixture'));
  });

  await t.test('enabled namespace calls keep using the existing proxy executor', async () => {
    const f = fixture();
    f.refresh();
    const args = { message: 'hello' };
    const signal = new AbortController().signal;
    await f.tools.get('mcp__fixture').execute('call', { tool: 'echo', args }, signal);
    const call = calls.at(-1);
    assert.equal(call[0].marker, 'real-state');
    assert.equal(call[1], 'echo');
    assert.equal(call[2], args);
    assert.equal(call[3], 'fixture');
    assert.equal(call[4], f.input.getPiTools);
    assert.equal(call[5], signal);
    assert.equal(call[6], 'proxy');
  });
});
