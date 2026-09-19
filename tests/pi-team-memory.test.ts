import assert from 'node:assert/strict';
import test from 'node:test';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { shouldResumeAfterOmCompact } from '../home/config/pi/extensions/pi-observational-memory-jev/hooks/resume.ts';

test('team children force om off and do not inherit coordinator memory', () => {
  const index = readFileSync(fileURLToPath(new URL('../home/config/pi/extensions/subagent/index.ts', import.meta.url)), 'utf8');
  const team = readFileSync(fileURLToPath(new URL('../home/config/pi/extensions/subagent/team.ts', import.meta.url)), 'utf8');
  const session = readFileSync(fileURLToPath(new URL('../home/config/pi/extensions/pi-observational-memory-jev/memory/session.ts', import.meta.url)), 'utf8');
  const compact = readFileSync(fileURLToPath(new URL('../home/config/pi/extensions/pi-observational-memory-jev/hooks/compaction-trigger.ts', import.meta.url)), 'utf8');
  assert.doesNotMatch(index, /parentObservationalMemoryRoot/);
  assert.doesNotMatch(index, /observationalMemoryBriefing/);
  assert.doesNotMatch(index, /PI_OM_PARENT_MEMORY/);
  assert.doesNotMatch(team, /PI_OM_PARENT_MEMORY/);
  assert.doesNotMatch(session, /envParentMemoryRoot/);
  assert.match(team, /PI_OM_DEFAULT: '0'/);
  assert.match(team, /teamPaneEnv\(agent\)/);
  assert.match(compact, /shouldResumeAfterOmCompact\(runtime, turnWillContinue\(event\)\)/);
});

test('om compact resume stays off for team children and when om is gated', () => {
  const runtime = { enabled: true, config: { resumeAfterMidRunCompaction: true, passive: false } };
  assert.equal(shouldResumeAfterOmCompact(runtime, true, {}), true);
  assert.equal(shouldResumeAfterOmCompact(runtime, true, { PI_TEAM_CHILD: '1' }), false);
  assert.equal(shouldResumeAfterOmCompact(runtime, false, {}), false);
  assert.equal(shouldResumeAfterOmCompact({ ...runtime, enabled: false }, true, {}), false);
  assert.equal(shouldResumeAfterOmCompact({ enabled: true, config: { resumeAfterMidRunCompaction: true, passive: true } }, true, {}), false);
});
