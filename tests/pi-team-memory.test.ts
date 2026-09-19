import assert from 'node:assert/strict';
import test from 'node:test';
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  composeChildSystemPrompt,
  observationalMemoryBriefing,
  parentObservationalMemoryRoot,
} from '../home/config/pi/extensions/subagent/memory.ts';
import { envParentMemoryRoot, seedFromParentMemory } from '../home/config/pi/extensions/pi-observational-memory-jev/memory/parent-env.ts';
import { shouldResumeAfterOmCompact } from '../home/config/pi/extensions/pi-observational-memory-jev/hooks/resume.ts';

function tempDir(t, prefix) {
  const dir = mkdtempSync(join(tmpdir(), prefix));
  t.after(() => rmSync(dir, { recursive: true, force: true }));
  return dir;
}

test('parent observational memory root accepts only a real .memory session directory', (t) => {
  const cwd = tempDir(t, 'pi-om-root-');
  const sessionId = 'sess-parent';
  const root = join(cwd, '.memory', sessionId);
  assert.equal(parentObservationalMemoryRoot(cwd, sessionId), undefined);
  mkdirSync(root, { recursive: true });
  assert.equal(parentObservationalMemoryRoot(cwd, sessionId), root);
  assert.equal(parentObservationalMemoryRoot(cwd, '../escape'), undefined);
  assert.equal(parentObservationalMemoryRoot(cwd, 'sess-parent/../other'), undefined);
  assert.equal(parentObservationalMemoryRoot('', sessionId), undefined);
});

test('briefing is INDEX and journey only, with the absolute parent root', (t) => {
  const cwd = tempDir(t, 'pi-om-brief-');
  const root = join(cwd, '.memory', 'sess-parent');
  mkdirSync(root, { recursive: true });
  writeFileSync(join(root, 'JOURNEY.md'), 'Stay on the calculator path.\n');
  writeFileSync(join(root, 'INDEX.md'), '# Memory index\n\n- `facts.md` - Rails must not require autoloadables\n');
  writeFileSync(join(root, 'facts.md'), 'FULL TOPIC BODY MUST NOT APPEAR\n');
  const briefing = observationalMemoryBriefing(root);
  assert.match(briefing, /Parent observational memory/);
  assert.ok(briefing.includes(root));
  assert.match(briefing, /Stay on the calculator path/);
  assert.match(briefing, /Rails must not require autoloadables/);
  assert.doesNotMatch(briefing, /FULL TOPIC BODY MUST NOT APPEAR/);
  assert.equal(observationalMemoryBriefing(root + '-missing'), '');
  assert.equal(composeChildSystemPrompt('Role prompt', briefing).startsWith('Role prompt\n\n'), true);
  assert.equal(composeChildSystemPrompt('Role prompt', ''), 'Role prompt');
});

test('team children seed a private copy from PI_OM_PARENT_MEMORY', (t) => {
  const cwd = tempDir(t, 'pi-om-seed-');
  const parent = join(cwd, '.memory', 'parent');
  mkdirSync(parent, { recursive: true });
  writeFileSync(join(parent, 'decisions.md'), 'Use direnv exec\n');
  mkdirSync(join(parent, '.runs'));
  writeFileSync(join(parent, '.runs', 'worker.json'), 'not copied\n');
  const childId = 'child-session';
  assert.equal(envParentMemoryRoot({ PI_OM_PARENT_MEMORY: '/tmp/not-memory/parent' }), undefined);
  assert.equal(envParentMemoryRoot({ PI_OM_PARENT_MEMORY: parent }), parent);
  const root = join(cwd, '.memory', childId);
  seedFromParentMemory(parent, root);
  assert.equal(readFileSync(join(root, 'decisions.md'), 'utf8'), 'Use direnv exec\n');
  assert.equal(existsSync(join(root, '.runs')), false);
});

test('subagent dispatch wires parent memory into interactive and headless launches', () => {
  const index = readFileSync(fileURLToPath(new URL('../home/config/pi/extensions/subagent/index.ts', import.meta.url)), 'utf8');
  const team = readFileSync(fileURLToPath(new URL('../home/config/pi/extensions/subagent/team.ts', import.meta.url)), 'utf8');
  const session = readFileSync(fileURLToPath(new URL('../home/config/pi/extensions/pi-observational-memory-jev/memory/session.ts', import.meta.url)), 'utf8');
  const compact = readFileSync(fileURLToPath(new URL('../home/config/pi/extensions/pi-observational-memory-jev/hooks/compaction-trigger.ts', import.meta.url)), 'utf8');
  assert.match(session, /envParentMemoryRoot\(env\)/);
  assert.match(session, /seedFromParentMemory\(parent, root\)/);
  assert.match(compact, /shouldResumeAfterOmCompact\(runtime, turnWillContinue\(event\)\)/);
  assert.match(index, /parentObservationalMemoryRoot\(ctx\.cwd, ctx\.sessionManager\?\.getSessionId\?\.\(\)\)/);
  assert.match(index, /observationalMemoryBriefing\(dispatchDefaults\.parentMemoryRoot\)/);
  assert.match(index, /parentMemoryRoot: dispatchDefaults\.parentMemoryRoot/);
  assert.match(index, /PI_OM_PARENT_MEMORY: dispatchDefaults\.parentMemoryRoot/);
  assert.match(team, /PI_OM_DEFAULT: '0'/);
  assert.match(team, /teamPaneEnv\(agent, parentMemoryRoot\)/);
});

test('om compact resume stays off for team children and when om is gated', () => {
  const runtime = { enabled: true, config: { resumeAfterMidRunCompaction: true, passive: false } };
  assert.equal(shouldResumeAfterOmCompact(runtime, true, {}), true);
  assert.equal(shouldResumeAfterOmCompact(runtime, true, { PI_TEAM_CHILD: '1' }), false);
  assert.equal(shouldResumeAfterOmCompact(runtime, false, {}), false);
  assert.equal(shouldResumeAfterOmCompact({ ...runtime, enabled: false }, true, {}), false);
  assert.equal(shouldResumeAfterOmCompact({ enabled: true, config: { resumeAfterMidRunCompaction: true, passive: true } }, true, {}), false);
});
