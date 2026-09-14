import assert from 'node:assert/strict';
import test from 'node:test';
import { mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import orchestratorAddendum, {
  appendOrchestratorAddendum,
  isOrchestratorProcess,
  orchestratorAddendumPath,
  readOrchestratorAddendum,
} from '../home/config/pi/extensions/orchestrator-addendum.ts';

const addendum = 'You are the starting Pi agent for this session.\n## Final voice summary';

test('parent process is the orchestrator unless marked as a team child', () => {
  assert.equal(isOrchestratorProcess({}), true);
  assert.equal(isOrchestratorProcess({ PI_TEAM_CHILD: '0' }), true);
  assert.equal(isOrchestratorProcess({ PI_TEAM_CHILD: '1' }), false);
  assert.equal(isOrchestratorProcess({}, () => undefined), true);
  assert.equal(isOrchestratorProcess({}, () => '/tmp/pi-team'), false);
});

test('addendum path follows PI_CODING_AGENT_DIR then the default agent dir', () => {
  assert.equal(orchestratorAddendumPath({ PI_CODING_AGENT_DIR: '/tmp/pi-profile' }), '/tmp/pi-profile/ORCHESTRATOR.md');
  assert.match(orchestratorAddendumPath({}), /\.pi\/agent\/ORCHESTRATOR\.md$/);
});

test('addendum appends once and ignores empty or missing files', () => {
  assert.equal(appendOrchestratorAddendum('base', addendum), `base\n\n${addendum}`);
  assert.equal(appendOrchestratorAddendum(`base\n\n${addendum}`, addendum), `base\n\n${addendum}`);
  assert.equal(appendOrchestratorAddendum('base', '  \n'), 'base');
  assert.equal(readOrchestratorAddendum('/no/such/ORCHESTRATOR.md'), '');
});

test('extension appends for the parent and skips children or missing files', async (t) => {
  const dir = await mkdtemp(join(tmpdir(), 'pi-orchestrator-'));
  t.after(() => rm(dir, { recursive: true, force: true }));
  const path = join(dir, 'ORCHESTRATOR.md');
  await writeFile(path, addendum);

  const run = (env: NodeJS.ProcessEnv, flag?: unknown, addendumPath = path) => {
    const handlers = new Map();
    orchestratorAddendum({
      getFlag: () => flag,
      on: (name, handler) => handlers.set(name, handler),
    }, { env, addendumPath });
    return handlers.get('before_agent_start')({ systemPrompt: 'base prompt' });
  };

  assert.deepEqual(run({}), { systemPrompt: `base prompt\n\n${addendum}` });
  assert.equal(run({ PI_TEAM_CHILD: '1' }), undefined);
  assert.equal(run({}, '/tmp/pi-team'), undefined);
  assert.equal(run({}, undefined, join(dir, 'missing.md')), undefined);
});
