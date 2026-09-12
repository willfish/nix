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
test('Pi settings defaults enable quiet startup without clobbering user keys', () => {
  const defaults = JSON.parse(readFileSync(new URL('../home/config/pi/settings-defaults.json', import.meta.url)));
  assert.equal(defaults.quietStartup, true);
  assert.equal(defaults.editorPaddingX, 1);
  const keybindings = JSON.parse(readFileSync(new URL('../home/config/pi/keybindings-defaults.json', import.meta.url)));
  assert.equal(keybindings['app.session.rename'], 'ctrl+shift+r');
  const pi = readFileSync(new URL('../home/user/pi.nix', import.meta.url), 'utf8');
  assert.match(pi, /merge-settings\.py/);
  assert.match(pi, /settings-defaults\.json/);
  assert.match(pi, /keybindings-defaults\.json/);
  const appearance = readFileSync(new URL('../home/user/appearance.nix', import.meta.url), 'utf8');
  assert.doesNotMatch(appearance, /piEditorPadding/);
});
test('Home Manager wires OpenCode and OpenRouter keys onto built-in catalogs', () => {
  const pi = readFileSync(new URL('../home/user/pi.nix', import.meta.url), 'utf8');
  const secrets = readFileSync(new URL('../home/user/secrets.nix', import.meta.url), 'utf8');
  for (const name of ['OPENCODE_API_KEY', 'OPENCODE_GO_KEY', 'OPENROUTER_API_KEY']) {
    assert.match(secrets, new RegExp(`"${name}"`));
    assert.match(pi, new RegExp(`sopsApiKey "${name}"`));
  }
  const { providers } = JSON.parse(readFileSync(modelsPath));
  assert.equal(providers.opencode, undefined);
  assert.equal(providers['opencode-go'], undefined);
  assert.equal(providers.openrouter, undefined);
});
