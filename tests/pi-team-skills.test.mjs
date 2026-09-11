import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdtemp, mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { dirname, join, relative } from 'node:path';
import { fileURLToPath } from 'node:url';
import test from 'node:test';
import { parseSkillList, withSkills } from '../home/config/pi/extensions/subagent/skills.js';

async function fixture(t) {
  const dir = await mkdtemp(join(tmpdir(), 'pi-team-skills-'));
  t.after(() => rm(dir, { recursive: true, force: true }));
  return dir;
}

test('skill declarations accept CSV and YAML list values, including absent and empty lists', () => {
  assert.deepEqual(parseSkillList(undefined), []);
  assert.deepEqual(parseSkillList([]), []);
  assert.deepEqual(parseSkillList(' one, two '), ['one', 'two']);
  assert.deepEqual(parseSkillList([' one ', 'two']), ['one', 'two']);
});

test('malformed declarations fail without dropping invalid entries', async () => {
  for (const value of [null, true, 123, {}, '', ' ', 'one,,two', ['one', null], ['one', 1], [['one']], [''], ['a b'], ['../skill'], ['a,b'], new Array(1)]) {
    assert.throws(() => parseSkillList(value, 'Persona skills'), /Persona skills/);
    await assert.rejects(withSkills({ name: 'builder', skills: value, systemPrompt: 'Inherited' }), /Agent.*skills/);
    await assert.rejects(withSkills({ name: 'builder', systemPrompt: 'Inherited' }, value), /Requested skills/);
  }
});

test('no skills preserves the inherited system prompt exactly', async () => {
  assert.equal(await withSkills({ systemPrompt: '\nOriginal prompt\n' }), '\nOriginal prompt\n');
});

test('loads full content once per name in persona-first order with absolute reference paths', async (t) => {
  const dir = await fixture(t);
  const first = join(dir, 'first', 'SKILL.md');
  const second = join(dir, 'second', 'SKILL.md');
  await mkdir(dirname(first));
  await mkdir(dirname(second));
  const full = `---\nname: first\ndescription: Actual instructions\n---\n${'Full content\n'.repeat(2200)}Read references/details.md\nEND-OF-FIRST\n`;
  await writeFile(first, full);
  await writeFile(second, 'SECOND-SKILL-CONTENT');
  const agent = { name: 'builder', systemPrompt: 'Inherited role prompt', skills: ['first', 'first'] };
  const requested = ['second', 'first', 'second'];
  const available = [{ name: 'first', filePath: relative(process.cwd(), first) }, { name: 'second', filePath: second }];
  const prompt = await withSkills(agent, requested, available);
  assert.ok(prompt.startsWith('Inherited role prompt\n\n'));
  assert.ok(prompt.includes(full));
  assert.ok(prompt.includes(`Skill file: ${first}`));
  assert.ok(prompt.includes(`Reference base directory: ${dirname(first)}`));
  assert.ok(prompt.includes('Resolve relative references, scripts, and assets against this base directory.'));
  assert.equal(prompt.split('## Loaded skill: first').length, 2);
  assert.equal(prompt.split('## Loaded skill: second').length, 2);
  assert.ok(prompt.indexOf('END-OF-FIRST') < prompt.indexOf('SECOND-SKILL-CONTENT'));
  assert.deepEqual(agent.skills, ['first', 'first']);
  assert.deepEqual(requested, ['second', 'first', 'second']);
});

test('missing names fail before file reads, and file failures include skill and path', async (t) => {
  const dir = await fixture(t);
  const filePath = join(dir, 'missing', 'SKILL.md');
  const agent = { name: 'architect', systemPrompt: '', skills: ['present'] };
  const available = [{ name: 'present', filePath }];
  await assert.rejects(withSkills(agent, ['unknown'], available), /architect.*unknown.*active Pi skills/);
  await assert.rejects(withSkills(agent, [], available), error => {
    assert.match(error.message, /architect.*cannot read skill "present"/);
    assert.ok(error.message.includes(filePath));
    assert.equal(error.cause.code, 'ENOENT');
    return true;
  });
  await assert.rejects(withSkills(agent, [], [{ name: 'present' }]), /present.*no filePath/);
  await assert.rejects(withSkills(agent, [], null), /Available skills must be a list/);
});

test('real Pi frontmatter discovery retains strict skills and existing permissive tools semantics', { timeout: 30000 }, async (t) => {
  const dir = await fixture(t);
  const agentDir = join(dir, 'agent');
  const projectDir = join(dir, '.pi', 'agents');
  await mkdir(agentDir);
  await mkdir(projectDir, { recursive: true });
  const activeSkill = join(dir, 'SKILL.md');
  await writeFile(activeSkill, '---\nname: fixture-skill\ndescription: Trusted CLI skill fixture\n---\nFull fixture content');
  await writeFile(join(agentDir, 'settings.json'), JSON.stringify({ enableSkillCommands: false }));
  const output = join(dir, 'result.json');
  const extension = join(dir, 'check.ts');
  const agentsPath = fileURLToPath(new URL('../home/config/pi/extensions/subagent/agents.ts', import.meta.url));
  const personaDir = fileURLToPath(new URL('../home/config/pi/agents/', import.meta.url));
  await writeFile(extension, `
    import { discoverAgents } from ${JSON.stringify(agentsPath)};
    import { writeFileSync, readFileSync, unlinkSync } from 'node:fs';
    export default function(pi) {
      pi.registerCommand('check-skills', { handler: async (_args, ctx) => {
        const results = [];
        const target = ${JSON.stringify(join(projectDir, 'test.md'))};
        for (const declaration of ['skills: [one, two]', 'skills: one, two', 'skills: []', '', 'skills: null', 'skills: 12', 'skills: true', 'skills: {}', 'skills: [one, 2]', 'skills: [[one]]', 'skills: ""', 'skills: one,,two']) {
          writeFileSync(target, '---\\nname: test\\ndescription: Test agent\\ntools: [read, 12, bash]\\n' + declaration + '\\n---\\nInherited prompt');
          try { results.push({ declaration, agent: discoverAgents(${JSON.stringify(dir)}, 'project').agents[0] }); }
          catch (error) { results.push({ declaration, error: error.message }); }
        }
        unlinkSync(target);
        for (const role of ['architect', 'builder', 'sceptic', 'test-engineer', 'security-reviewer', 'domain-specialist']) {
          writeFileSync(${JSON.stringify(projectDir)} + '/' + role + '.md', readFileSync(${JSON.stringify(personaDir)} + '/' + role + '.md'));
        }
        writeFileSync(${JSON.stringify(output)}, JSON.stringify({ results, personas: discoverAgents(${JSON.stringify(dir)}, 'project').agents, activeSkills: ctx.getSystemPromptOptions().skills, skillCommands: pi.getCommands().filter(command => command.source === 'skill') }));
      }});
    }
  `);
  const result = spawnSync(process.env.PI_TEAM_TEST_BIN ?? 'pi', [
    '--offline', '--no-extensions', '--no-skills', '--no-prompt-templates', '--no-context-files',
    '--no-session', '--skill', activeSkill, '-e', extension, '-p', '/check-skills',
  ], { cwd: dir, env: { ...process.env, PI_CODING_AGENT_DIR: agentDir, CAPTURE_PROMPTS: '0' }, encoding: 'utf8', timeout: 25000 });
  assert.equal(result.status, 0, `${result.error ?? ''}\n${result.stderr}\n${result.stdout}`);
  const { results, personas, activeSkills, skillCommands } = JSON.parse(await readFile(output, 'utf8'));
  assert.deepEqual(activeSkills.map(({ name, filePath }) => ({ name, filePath })), [{ name: 'fixture-skill', filePath: activeSkill }]);
  assert.deepEqual(skillCommands.map(({ name, sourceInfo }) => ({ name, filePath: sourceInfo.path })), [{ name: 'skill:fixture-skill', filePath: activeSkill }]);
  for (const [i, result] of results.entries()) {
    if (i < 4) {
      assert.deepEqual(result.agent.skills, i < 2 ? ['one', 'two'] : []);
      assert.deepEqual(result.agent.tools, ['read', 'bash']);
      assert.equal(result.agent.systemPrompt.trim(), 'Inherited prompt');
    } else {
      assert.match(result.error, /Agent skills in .*test\.md/);
    }
  }
  const expected = {
    architect: ['chain-of-verification'], builder: ['verification-before-completion'],
    sceptic: ['code-review-workflow'],
    'test-engineer': [], 'security-reviewer': [], 'domain-specialist': [],
  };
  assert.equal(personas.length, 6);
  for (const persona of personas) assert.deepEqual(persona.skills, expected[persona.name]);
  const optional = personas.filter(persona => persona.skills.length === 0);
  for (const persona of optional) {
    // Empty defaults do not preload a framework/domain workflow or any other role's body.
    const prompt = await withSkills(persona, [], activeSkills);
    assert.equal(prompt, persona.systemPrompt);
    for (const other of personas.filter(other => other !== persona)) assert.ok(!prompt.includes(other.systemPrompt));
    const augmented = await withSkills(persona, ['fixture-skill'], activeSkills);
    assert.ok(augmented.includes('Full fixture content'));
    assert.equal((augmented.match(/## Loaded skill:/g) ?? []).length, 1);
  }
  for (const name of ['security-reviewer', 'domain-specialist', 'sceptic']) {
    assert.ok(!personas.find(persona => persona.name === name).tools.some(tool => ['edit', 'write'].includes(tool)));
  }
  assert.ok(personas.find(persona => persona.name === 'test-engineer').tools.includes('edit'));
});
