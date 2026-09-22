import assert from 'node:assert/strict';
import { mkdtemp, rm, writeFile } from 'node:fs/promises';
import { readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import test from 'node:test';
import hiddenModels, {
  filterProviderModels,
  hiddenModelsPaths,
  isHiddenModel,
  modelKey,
  readHiddenModelsConfig,
} from '../home/config/pi/extensions/hidden-models.ts';

const config = JSON.parse(readFileSync(new URL('../home/config/pi/hidden-models.json', import.meta.url), 'utf8'));
const piNix = readFileSync(new URL('../home/user/pi.nix', import.meta.url), 'utf8');

test('hidden model keys cover the OpenCode console denylist', () => {
  assert.deepEqual(config.providers, ['opencode-go', 'openrouter']);
  assert.equal(config.ids.length, 46);
  assert.equal(new Set(config.ids).size, 46);
  for (const id of ['claude-sonnet-4-5', 'gpt-5.4', 'gpt-6-astra', 'qwen3.6-plus']) {
    assert.ok(config.ids.includes(id), id);
  }
});

test('matching normalises OpenRouter paths, dots, and batch suffixes', () => {
  assert.equal(isHiddenModel('claude-sonnet-4-5', config.ids), true);
  assert.equal(isHiddenModel('anthropic/claude-sonnet-4.5', config.ids), true);
  assert.equal(isHiddenModel('anthropic/claude-sonnet-4.5:batch', config.ids), true);
  assert.equal(isHiddenModel('openai/gpt-5.4', config.ids), true);
  assert.equal(isHiddenModel('gpt-5', config.ids), false);
  assert.equal(isHiddenModel('deepseek-v4-flash', config.ids), false);
  assert.equal(isHiddenModel('deepseek-v4-flash-vision-exp', config.ids), true);
  assert.equal(isHiddenModel('google/gemini-3-flash-preview', config.ids), false);
  assert.equal(modelKey('openai/gpt-5.1-codex:batch'), 'gpt-5-1-codex');
});

test('filter drops hidden catalog rows and keeps provider-local fields', () => {
  const kept = filterProviderModels([
    { id: 'glm-5.1', name: 'GLM 5.1', api: 'openai-completions', baseUrl: 'https://opencode.ai/zen/go/v1', provider: 'opencode-go' },
    { id: 'kimi-k2.5', name: 'Kimi K2.5', api: 'openai-completions', baseUrl: 'https://opencode.ai/zen/go/v1', provider: 'opencode-go' },
  ], config.ids);
  assert.deepEqual(kept.map((model) => model.id), ['kimi-k2.5']);
  assert.equal(kept[0].provider, undefined);
  assert.equal(kept[0].api, 'openai-completions');
});

test('paths follow PI_CODING_AGENT_DIR then the default agent dir', () => {
  assert.deepEqual(
    hiddenModelsPaths({ PI_CODING_AGENT_DIR: '/tmp/pi-profile' }, () => '/home/will'),
    {
      hiddenPath: '/tmp/pi-profile/hidden-models.json',
      storePath: '/tmp/pi-profile/models-store.json',
    },
  );
  assert.match(hiddenModelsPaths({}, () => '/home/will').hiddenPath, /\/home\/will\/\.pi\/agent\/hidden-models\.json$/);
});

test('extension replaces OpenCode Go and OpenRouter with the remaining catalog', async (t) => {
  const dir = await mkdtemp(join(tmpdir(), 'pi-hidden-models-'));
  t.after(() => rm(dir, { recursive: true, force: true }));
  const hiddenPath = join(dir, 'hidden-models.json');
  const storePath = join(dir, 'models-store.json');
  await writeFile(hiddenPath, JSON.stringify(config));
  await writeFile(storePath, JSON.stringify({
    'opencode-go': {
      models: [
        { id: 'glm-5.1', api: 'openai-completions', baseUrl: 'https://go.example' },
        { id: 'kimi-k2.5', api: 'openai-completions', baseUrl: 'https://go.example' },
      ],
    },
    openrouter: {
      models: [
        { id: 'anthropic/claude-sonnet-4.5', api: 'openai-completions', baseUrl: 'https://openrouter.example' },
        { id: 'inception/mercury-2', api: 'openai-completions', baseUrl: 'https://openrouter.example' },
      ],
    },
  }));

  const registered = [];
  const handlers = new Map();
  hiddenModels({
    registerProvider: (name, providerConfig) => registered.push({ name, providerConfig }),
    on: (name, handler) => handlers.set(name, handler),
  }, { hiddenPath, storePath });

  assert.deepEqual(registered.map((entry) => entry.name), ['opencode-go', 'openrouter']);
  assert.deepEqual(registered[0].providerConfig.models.map((model) => model.id), ['kimi-k2.5']);
  assert.deepEqual(registered[1].providerConfig.models.map((model) => model.id), ['inception/mercury-2']);
  assert.equal(typeof registered[0].providerConfig.refreshModels, 'function');
  assert.equal(readHiddenModelsConfig(hiddenPath).ids.length, 46);
  assert.ok(handlers.has('session_start'));

  registered.length = 0;
  await handlers.get('session_start')({}, {
    modelRegistry: {
      getAvailable: async () => [
        { provider: 'opencode-go', id: 'glm-5.2', api: 'openai-completions', baseUrl: 'https://go.example' },
        { provider: 'opencode-go', id: 'kimi-k2.5', api: 'openai-completions', baseUrl: 'https://go.example' },
        { provider: 'openrouter', id: 'openai/gpt-5.4', api: 'openai-completions', baseUrl: 'https://openrouter.example' },
        { provider: 'xai', id: 'grok-4.5', api: 'openai-responses', baseUrl: 'https://api.x.ai/v1' },
      ],
    },
  });
  assert.deepEqual(registered.map((entry) => entry.name), ['opencode-go', 'openrouter']);
  assert.deepEqual(registered[0].providerConfig.models.map((model) => model.id), ['kimi-k2.5']);
  assert.deepEqual(registered[1].providerConfig.models.map((model) => model.id), ['inception/mercury-2']);
});

test('Home Manager deploys the denylist next to the extension', () => {
  assert.match(piNix, /hidden-models\.json/);
  assert.match(piNix, /extensions\/hidden-models\.ts/);
});
