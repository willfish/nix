import assert from 'node:assert/strict';
import { readdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import test from 'node:test';
import {
  agentLaunchFlags,
  parseThinkingLevel,
  resolveLaunchConfig,
} from '../home/config/pi/extensions/subagent/launch.ts';

const personaDir = fileURLToPath(new URL('../home/config/pi/agents/', import.meta.url));

const ROLE_LAUNCH = {
  scout: { model: 'xai/grok-4.6', thinking: 'low' },
  planner: { model: 'xai/grok-4.6', thinking: 'medium' },
  worker: { model: 'xai/grok-4.6', thinking: 'medium' },
  builder: { model: 'xai/grok-4.6', thinking: 'medium' },
  'test-engineer': { model: 'xai/grok-4.6', thinking: 'medium' },
  architect: { model: 'openai-codex/gpt-6-astra', thinking: 'high' },
  sceptic: { model: 'openai-codex/gpt-6-astra', thinking: 'high' },
  reviewer: { model: 'openai-codex/gpt-6-astra', thinking: 'high' },
  'security-reviewer': { model: 'openai-codex/gpt-6-astra', thinking: 'high' },
  'domain-specialist': { model: 'openai-codex/gpt-6-astra', thinking: 'high' },
};

test('thinking levels accept YAML off and reject malformed values', () => {
  assert.equal(parseThinkingLevel(undefined, 'thinking'), undefined);
  assert.equal(parseThinkingLevel(false, 'thinking'), 'off');
  assert.equal(parseThinkingLevel('high', 'thinking'), 'high');
  for (const value of [true, 12, {}, [], '', ' ', 'Extreme', 'HIGH']) {
    assert.throws(() => parseThinkingLevel(value, 'Agent thinking'), /Agent thinking/);
  }
});

test('launch config prefers orchestrator override, then role, then session', () => {
  const agent = { model: 'xai/grok-4.6', thinking: 'medium' };
  const session = { model: 'session/default', thinkingLevel: 'low' };
  assert.deepEqual(
    resolveLaunchConfig(agent, { model: 'openai-codex/gpt-6-astra', thinking: 'high' }, session),
    { model: 'openai-codex/gpt-6-astra', thinking: 'high' },
  );
  assert.deepEqual(resolveLaunchConfig(agent, { thinking: 'xhigh' }, session), {
    model: 'xai/grok-4.6', thinking: 'xhigh',
  });
  assert.deepEqual(resolveLaunchConfig(agent, { model: 'openai-codex/gpt-6-astra' }, session), {
    model: 'openai-codex/gpt-6-astra', thinking: 'medium',
  });
  assert.deepEqual(resolveLaunchConfig(agent, {}, session), {
    model: 'xai/grok-4.6', thinking: 'medium',
  });
  assert.deepEqual(resolveLaunchConfig({}, {}, session), {
    model: 'session/default', thinking: 'low',
  });
  assert.deepEqual(resolveLaunchConfig({ model: 'xai/grok-4.6' }, {}, session), {
    model: 'xai/grok-4.6', thinking: 'low',
  });
});

test('launch flags pass model and thinking independently', () => {
  assert.deepEqual(agentLaunchFlags({ model: 'xai/grok-4.6', thinking: 'high' }), [
    '--model', 'xai/grok-4.6', '--thinking', 'high',
  ]);
  assert.deepEqual(agentLaunchFlags({ model: 'openai-codex/gpt-6-astra' }), [
    '--model', 'openai-codex/gpt-6-astra',
  ]);
  assert.deepEqual(agentLaunchFlags({ thinking: 'low' }), ['--thinking', 'low']);
  assert.deepEqual(agentLaunchFlags({}), []);
});

test('every team role pins grok 4.6 or Astra with a thinking level', () => {
  const files = readdirSync(personaDir).filter(name => name.endsWith('.md'));
  assert.deepEqual(files.map(name => name.replace(/\.md$/, '')).sort(), Object.keys(ROLE_LAUNCH).sort());
  for (const file of files) {
    const name = file.replace(/\.md$/, '');
    const text = readFileSync(join(personaDir, file), 'utf8');
    const header = text.slice(0, text.indexOf('\n---', 4));
    assert.match(header, new RegExp(`^name: ${name}$`, 'm'));
    assert.match(header, new RegExp(`^model: ${ROLE_LAUNCH[name].model}$`, 'm'));
    assert.match(header, new RegExp(`^thinking: ${ROLE_LAUNCH[name].thinking}$`, 'm'));
  }
});
