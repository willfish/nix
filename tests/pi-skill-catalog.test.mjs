import assert from 'node:assert/strict';
import test from 'node:test';
import { mkdtemp, mkdir, writeFile, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import { searchCatalog, replaceSkillAdvertisement, BOOTSTRAP } from '../home/config/pi/extensions/skill-catalog/catalog.js';

const skills = Array.from({ length: 17 }, (_, i) => ({
  name: `skill-${String(i).padStart(2, '0')}`, description: `Testing workflow ${i}. ${'detail '.repeat(70)}`,
  filePath: `/trusted/skill-${i}/SKILL.md`, disableModelInvocation: i === 3,
}));
const commands = skills.map(s => ({ name: `skill:${s.name}`, source: 'skill', sourceInfo: { path: s.filePath } }));
const format = (ss, tool) => ss.some(s => !s.disableModelInvocation)
  ? `\n\nUse ${tool}\n<available_skills>${JSON.stringify(ss.filter(s => !s.disableModelInvocation))}</available_skills>` : '';
const options = { skills, selectedTools: ['read', 'skill_catalog'], cwd: '/project' };
const generated = format(skills, 'read');
const prompt = `core\nteam preload\ncontext${generated}\nCurrent working directory: /project\nother extension`;

test('default five, maximum ten, bounded triggers and complete pagination', () => {
  assert.equal(searchCatalog(skills, commands).results.length, 5);
  assert.equal(searchCatalog(skills, commands, { limit: 999 }).results.length, 10);
  const names = [];
  for (let offset = 0; offset < skills.length; offset += 5) {
    const page = searchCatalog(skills, commands, { offset });
    assert.equal(page.total, 17);
    names.push(...page.results.map(s => s.name));
    assert.ok(page.results.every(s => s.trigger.length <= 180));
  }
  assert.deepEqual(names, skills.map(s => s.name));
  assert.equal(searchCatalog(skills, commands, { offset: 99 }).nextOffset, null);
  assert.deepEqual(searchCatalog(skills, commands, { offset: 99 }).results, []);
});

test('invalid direct-call bounds normalize safely', () => {
  for (const limit of [NaN, Infinity, '9', null]) assert.equal(searchCatalog(skills, commands, { limit }).results.length, 5);
  assert.equal(searchCatalog(skills, commands, { limit: -2 }).results.length, 1);
  assert.equal(searchCatalog(skills, commands, { limit: 2.8 }).results.length, 2);
  assert.equal(searchCatalog(skills, commands, { offset: -2 }).offset, 0);
});

test('exact names, commands, aliases and full descriptions remain searchable', () => {
  const registry = [{ name: 'outlook-login', description: 'Microsoft work authentication. alias microsoft-login', filePath: '/trusted/microsoft-login/SKILL.md', disableModelInvocation: true }];
  const registered = [{ name: 'skill:outlook-login', source: 'skill', sourceInfo: { path: registry[0].filePath } },
    { name: 'skill:microsoft-login', source: 'skill', sourceInfo: { path: registry[0].filePath } }];
  for (const query of ['outlook-login', '/skill:outlook-login', 'microsoft-login', 'Microsoft authentication']) {
    const [found] = searchCatalog(registry, registered, { query }).results;
    assert.equal(found.name, 'outlook-login');
    assert.equal(found.manualOnly, true);
    assert.equal(found.path, registry[0].filePath);
    assert.equal(found.command, '/skill:outlook-login');
  }
  assert.equal(searchCatalog(registry, [], {}).results[0].command, null);
  assert.equal(searchCatalog(registry, [{ ...registered[0], source: 'extension' }]).results[0].command, null);
  assert.equal(searchCatalog(skills, commands, { query: 'absent' }).total, 0);
});

test('exact name outranks description matches, without mutating registry', () => {
  const registry = structuredClone(skills);
  registry[0].description += ' skill-16';
  const original = structuredClone(registry);
  const exact = searchCatalog(registry, commands, { query: 'skill-16' });
  assert.deepEqual(exact.results.map(result => result.name), ['skill-16']);
  assert.equal(exact.total, 1);
  assert.equal(exact.nextOffset, null);
  assert.deepEqual(searchCatalog(registry, commands, { query: 'skill-16', offset: 1 }).results, []);
  assert.deepEqual(registry, original);
});

test('real skill descriptions support paraphrases, ranked partial matches and manual-only metadata', async () => {
  const sources = [
    ['process-skills', 'systematic-debugging'], ['skills', 'rspec-testing'],
    ['skills', 'outlook-login'], ['skills', 'jira-workflow'], ['skills', 'skill-router'],
  ];
  const registry = await Promise.all(sources.map(async ([group, name]) => {
    const file = new URL(`../home/config/llm/${group}/${name}/SKILL.md`, import.meta.url);
    const text = await readFile(file, 'utf8');
    const header = text.split('---')[1];
    const description = header.match(/^description: *(.*)((?:\n[ \t]+.*)*)/m);
    return { name, filePath: fileURLToPath(file),
      description: (description[1].replace(/^[>|]-?$/, '') + description[2]).replace(/\s+/g, ' ').trim(),
      disableModelInvocation: /^disable-model-invocation: true$/m.test(header) };
  }));
  const registered = registry.map(s => ({ name: `skill:${s.name}`, source: 'skill', sourceInfo: { path: s.filePath } }));
  const original = structuredClone(registry);
  for (const [query, expected] of [
    ['fix failing tests', 'systematic-debugging'],
    ['please help me fix the failing tests', 'systematic-debugging'],
    ['debug unexpected regressions', 'systematic-debugging'],
    ['writing Ruby specs', 'rspec-testing'],
    ['Microsoft mailbox sign in', 'outlook-login'],
    ['microsoft-login', 'outlook-login'],
    ['update Jira stories', 'jira-workflow'],
  ]) {
    const page = searchCatalog(registry, registered, { query });
    assert.equal(page.results[0]?.name, expected, query);
    assert.ok(page.results.length <= 5);
    assert.deepEqual(searchCatalog([...registry].reverse(), registered, { query }), page);
  }
  const result = searchCatalog(registry, registered, { query: 'skill-router' }).results[0];
  assert.equal(result.manualOnly, true);
  assert.equal(result.command, '/skill:skill-router');
  assert.deepEqual(registry, original);
  for (const query of ['zyxnotaword', 'please the and', '???']) {
    assert.deepEqual(searchCatalog(registry, registered, { query }).results, []);
  }
  assert.equal(searchCatalog(registry, registered).total, sources.length);
});

test('canonical exact names outrank aliases, partial queries page stably and repeat words do not inflate rank', () => {
  const registry = [
    { name: 'alpha', description: 'Debug tests and failures', filePath: '/a', disableModelInvocation: true },
    { name: 'debug-tests', description: 'Debug tests', filePath: '/b' },
    { name: 'zeta', description: 'Tests only; repair reference', filePath: '/c' },
  ];
  const registered = [{ name: 'skill:debug-tests', source: 'skill', sourceInfo: { path: '/a' } }];
  assert.deepEqual(searchCatalog(registry, registered, { query: '/skill:debug-tests' }).results.map(result => result.name), ['debug-tests']);
  const aliases = [
    { name: 'skill:repair', source: 'skill', sourceInfo: { path: '/a' } },
    { name: 'skill:shared', source: 'skill', sourceInfo: { path: '/a' } },
    { name: 'skill:shared', source: 'skill', sourceInfo: { path: '/b' } },
  ];
  const alias = searchCatalog(registry, aliases, { query: 'repair' });
  assert.equal(alias.total, 1);
  assert.equal(alias.results[0].name, 'alpha');
  assert.equal(alias.results[0].manualOnly, true);
  assert.equal(alias.results[0].command, '/skill:repair');
  assert.deepEqual(searchCatalog(registry, aliases, { query: 'shared' }).results.map(result => result.name), ['alpha', 'debug-tests']);
  const query = 'debug failing tests';
  const all = searchCatalog(registry, registered, { query, limit: 10 });
  assert.equal(all.results[0].name, 'alpha');
  assert.equal(all.results[0].manualOnly, true);
  const paged = [0, 1, 2].flatMap(offset => searchCatalog(registry, registered, { query, limit: 1, offset }).results);
  assert.deepEqual(paged, all.results);
  assert.deepEqual(searchCatalog(registry, registered, { query: `${query} tests tests`, limit: 10 }).results, all.results);
});

test('only generated advertisement changes; team preload, context and chained additions survive', () => {
  assert.equal(replaceSkillAdvertisement(prompt, options, true, format), prompt.replace(generated, BOOTSTRAP));
  assert.match(BOOTSTRAP, /manual-only/i);
  assert.match(BOOTSTRAP, /offset/);
});

test('disabled tool, no registry, drift, ambiguity and contributed copies leave prompt unchanged', () => {
  assert.equal(replaceSkillAdvertisement(prompt, options, false, format), prompt);
  for (const opts of [undefined, {}, { ...options, skills: [] }, { ...options, selectedTools: ['read'] },
    { ...options, selectedTools: ['skill_catalog'] }, { ...options, cwd: '/wrong' },
    { ...options, appendSystemPrompt: generated }, { ...options, customPrompt: generated },
    { ...options, contextFiles: [{ content: generated }] }]) {
    assert.equal(replaceSkillAdvertisement(prompt, opts, true, format), prompt);
  }
  for (const original of [prompt.replace('Use read', 'changed'), prompt + generated, prompt + '<available_skills>other</available_skills>']) {
    assert.equal(replaceSkillAdvertisement(original, options, true, format), original);
  }
  assert.equal(replaceSkillAdvertisement(prompt, options, true, () => { throw Error('drift'); }), prompt);
});

test('installed Pi exports formatter, loads extension and supplies trusted registry without command changes', { timeout: 30000 }, async t => {
  const dir = await mkdtemp(join(tmpdir(), 'pi-catalog-'));
  t.after(() => rm(dir, { recursive: true, force: true }));
  const agent = join(dir, 'agent');
  await mkdir(agent);
  const visible = join(dir, 'microsoft-login.md');
  const manual = join(dir, 'manual.md');
  await writeFile(visible, '---\nname: outlook-login\ndescription: Microsoft work authentication, alias microsoft-login\n---\nSKILL BODY MUST NOT BE IN CATALOGUE');
  await writeFile(manual, '---\nname: manual\ndescription: Explicit user request only\ndisable-model-invocation: true\n---\nMANUAL BODY');
  const output = join(dir, 'result.json');
  const fixture = join(dir, 'fixture.ts');
  const extension = fileURLToPath(new URL('../home/config/pi/extensions/skill-catalog/index.ts', import.meta.url));
  const policy = fileURLToPath(new URL('../home/config/pi/extensions/reading-policy.js', import.meta.url));
  await writeFile(fixture, `
    import catalog from ${JSON.stringify(extension)};
    import { replaceReadingPolicy, CORE_READING_RULE } from ${JSON.stringify(policy)};
    import { formatSkillsForPrompt } from '@earendil-works/pi-coding-agent';
    import { writeFileSync } from 'node:fs';
    export default function(pi) {
      const hooks = new Map(); let tool;
      catalog({ on: (name, fn) => hooks.set(name, fn), registerTool: value => { tool = value; pi.registerTool(value); },
        getCommands: () => pi.getCommands(), getActiveTools: () => pi.getActiveTools() });
      pi.registerCommand('check-catalog', { handler: async (_args, ctx) => {
        const options = ctx.getSystemPromptOptions();
        const original = ctx.getSystemPrompt() + '\\nteam preload and prior extension';
        const before = pi.getCommands();
        const patch = hooks.get('before_agent_start')({ systemPrompt: original, systemPromptOptions: options });
        const result = await tool.execute('test', { query: 'microsoft-login' });
        const manual = await tool.execute('test', { query: 'manual' });
        const all = await tool.execute('test', {});
        const reading = replaceReadingPolicy(original, options);
        const advertisement = formatSkillsForPrompt(options.skills, 'read');
        hooks.get('session_shutdown')();
        let cleared = false;
        try { await tool.execute('test', {}); } catch { cleared = true; }
        writeFileSync(${JSON.stringify(output)}, JSON.stringify({ original, patched: patch?.systemPrompt,
          advertisement, reading, hadCoreRule: original.includes(CORE_READING_RULE), result, manual, all,
          commandsUnchanged: JSON.stringify(before) === JSON.stringify(pi.getCommands()), cleared,
          active: options.selectedTools.includes('skill_catalog') }));
      }});
    }
  `);
  const run = spawnSync(process.env.PI_SKILL_CATALOG_TEST_BIN ?? 'pi', [
    '--offline', '--no-extensions', '--no-skills', '--no-prompt-templates', '--no-context-files', '--no-session',
    '--skill', visible, '--skill', manual, '-e', fixture, '-p', '/check-catalog',
  ], { cwd: dir, env: { ...process.env, PI_CODING_AGENT_DIR: agent, CAPTURE_PROMPTS: '0' }, encoding: 'utf8', timeout: 25000 });
  assert.equal(run.status, 0, `${run.error ?? ''}\n${run.stderr}\n${run.stdout}`);
  const evidence = JSON.parse(await readFile(output, 'utf8'));
  assert.equal(evidence.patched, evidence.original.replace(evidence.advertisement, BOOTSTRAP));
  assert.equal(JSON.parse(evidence.result.content[0].text).results[0].command, '/skill:outlook-login');
  assert.equal(JSON.parse(evidence.manual.content[0].text).results[0].manualOnly, true);
  assert.equal(JSON.parse(evidence.all.content[0].text).total, 2);
  assert.ok(!evidence.all.content[0].text.includes('SKILL BODY'));
  assert.ok(!evidence.advertisement.includes('<name>manual</name>'));
  assert.ok(evidence.hadCoreRule && !evidence.reading.includes('Always read pi .md files completely'));
  assert.ok(evidence.commandsUnchanged && evidence.cleared && evidence.active);
});

test('bash fallback, custom prompt and empty visible catalogue', () => {
  const opts = { ...options, selectedTools: ['bash', 'skill_catalog'], customPrompt: 'custom core' };
  const original = `custom core${format(skills, 'bash')}\nCurrent working directory: /project\n`;
  assert.equal(replaceSkillAdvertisement(original, opts, true, format), `custom core${BOOTSTRAP}\nCurrent working directory: /project\n`);
  assert.equal(replaceSkillAdvertisement(prompt, { ...options, skills: skills.map(s => ({ ...s, disableModelInvocation: true })) }, true, format), prompt);
});
