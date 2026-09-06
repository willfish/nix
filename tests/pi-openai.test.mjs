import assert from 'node:assert/strict';
import { existsSync, readFileSync } from 'node:fs';
import test from 'node:test';

const modelsPath = new URL('../home/config/pi/models.json', import.meta.url);
test('Pi is installed in the shared package list', () => {
  const packages = readFileSync(new URL('../home/user/packages.nix', import.meta.url), 'utf8');
  assert.ok(/^\s+pi-coding-agent\s/m.test(packages), 'Pi missing from shared packages');
});
test('Astra registry exists with separate subscription and API routes', () => {
  assert.ok(existsSync(modelsPath), 'Astra registry missing');
  const { providers } = JSON.parse(readFileSync(modelsPath));
  for (const [name, api] of [['openai', 'openai-responses'], ['openai-codex', 'openai-codex-responses']]) {
    assert.equal(providers[name].apiKey, undefined);
    const model = providers[name].models.find(m => m.id === 'gpt-6-astra');
    assert.ok(model);
    assert.equal(model.api, api);
    assert.equal(model.reasoning, true);
    assert.equal(model.thinkingLevelMap.off, null);
    assert.equal(model.thinkingLevelMap.minimal, null);
    assert.equal(model.thinkingLevelMap.xhigh, 'xhigh');
    assert.ok(model.contextWindow >= 272000);
  }
});
