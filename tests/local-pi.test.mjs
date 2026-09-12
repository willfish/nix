import assert from 'node:assert/strict';
import { existsSync, readFileSync } from 'node:fs';
import test from 'node:test';

const extensionPath = new URL('../home/config/local-llm/pi-qwen.js', import.meta.url);
test('local Qwen request adapter is available', () => {
  assert.ok(existsSync(extensionPath), 'missing local Qwen request adapter');
});

if (existsSync(extensionPath)) {
  const { default: extension } = await import(extensionPath);
  function adapt(payload, level = 'medium', provider = 'relay') {
    let handler;
    extension({
      on(name, callback) {
        assert.equal(name, 'before_provider_request');
        handler = callback;
      },
      getThinkingLevel: () => level,
    });
    return handler({ payload }, { model: { provider } });
  }
  test('medium thinking preserves the cache and uses Qwen thinking sampling', () => {
    const original = { model: 'qwen3.8-27b', chat_template_kwargs: { enable_thinking: true } };
    const result = adapt(original);
    assert.deepEqual(result.chat_template_kwargs, {
      enable_thinking: true, preserve_thinking: true, reasoning_effort: 'medium',
    });
    assert.equal(result.temperature, 1);
    assert.equal(result.top_p, 0.95);
    assert.equal(result.presence_penalty, 0);
    assert.equal(result.repeat_penalty, 1);
    assert.equal(original.temperature, undefined);
  });
  test('off stays off, including summary requests while the UI is on medium', () => {
    const result = adapt({ chat_template_kwargs: { enable_thinking: false } });
    assert.equal(result.chat_template_kwargs.enable_thinking, false);
    assert.equal(result.chat_template_kwargs.reasoning_effort, undefined);
    assert.equal(result.temperature, 0.7);
    assert.equal(result.top_p, 0.8);
    assert.equal(result.presence_penalty, 1.5);
  });
  test('Pi levels map to supported Qwen template levels', () => {
    for (const [level, expected] of [['minimal', 'low'], ['low', 'low'], ['medium', 'medium'], ['high', 'xhigh'], ['xhigh', 'xhigh'], ['max', 'xhigh']]) {
      assert.equal(adapt({ chat_template_kwargs: { enable_thinking: true } }, level).chat_template_kwargs.reasoning_effort, expected);
    }
  });
  test('Andromeda uses the same thinking and sampling behavior as Relay', () => {
    for (const level of ['off', 'minimal', 'low', 'medium', 'high', 'xhigh', 'max']) {
      for (const enableThinking of [false, true]) {
        const payload = {
          model: 'qwen3.8-27b',
          messages: [{ role: 'user', content: 'Explain this change.' }],
          chat_template_kwargs: { enable_thinking: enableThinking, custom_option: 'keep' },
        };
        assert.deepEqual(
          adapt(payload, level, 'andromeda'),
          adapt(payload, level, 'relay'),
          `${level} with enable_thinking=${enableThinking}`,
        );
      }
    }
  });
  test('other providers are untouched', () => {
    assert.equal(adapt({ model: 'other' }, 'medium', 'other'), undefined);
  });
}

test('Home Manager launcher is isolated, lean and offline at startup', () => {
  const source = readFileSync(new URL('../home/user/local-llm.nix', import.meta.url), 'utf8');
  assert.match(source, /name = "qwen-pi"/);
  for (const flag of ['--offline', '--no-context-files', '--no-skills', '--no-extensions', '--no-prompt-templates']) {
    assert.ok(source.includes(flag), `missing ${flag}`);
  }
  assert.match(source, /defaultThinkingLevel = "medium";/);
  assert.match(source, /PI_CODING_AGENT_DIR/);
  for (const name of ['herdr-agent-state.ts', 'herdr-ui.js', 'herdr-model.js']) {
    assert.ok(source.includes(`/extensions/${name}`), `missing explicit ${name}`);
  }
  // The local Qwen profile is memory-constrained, so it must never load the
  // subagent extension or its agents; subagents are a standard-profile only
  // capability for Grok/OpenAI models. The prompt-history extension is pure
  // editor UI with no model calls, so qwen-pi loads it explicitly as well.
  assert.ok(!source.includes('subagent'), 'qwen-pi launcher references the subagent extension');
  assert.ok(
    source.includes('/extensions/prompt-history/index.ts'),
    'qwen-pi launcher missing the explicit prompt-history extension',
  );
  assert.match(source, /thinkingFormat = "qwen-chat-template"/);
});

test('voice installation and explicit Qwen loading share the supported-host predicate', () => {
  const standard = readFileSync(new URL('../home/user/pi.nix', import.meta.url), 'utf8');
  const qwen = readFileSync(new URL('../home/user/local-llm.nix', import.meta.url), 'utf8');
  const voice = readFileSync(new URL('../home/user/voice.nix', import.meta.url), 'utf8');
  for (const source of [standard, qwen, voice]) {
    assert.match(source, /voiceFeatures = import \.\/voice-supported\.nix \{ inherit pkgs hostName; \};/);
  }
  assert.match(standard, /home\.file\."\.pi\/agent\/extensions\/pi-voice\.js" = lib\.mkIf voiceFeatures\.stt \{\s*source = \.\.\/config\/pi\/extensions\/pi-voice\.js;\s*\};/);
  assert.equal((standard.match(/home\.file\."\.pi\/agent\/extensions\/pi-voice\.js"/g) ?? []).length, 1);
  assert.match(qwen, /lib\.optionalString voiceFeatures\.stt "export PI_VOICE_HARNESS=qwen-pi"/);
  assert.match(qwen, /lib\.optionalString voiceFeatures\.stt "--extension \$\{config\.home\.homeDirectory\}\/\.pi\/agent\/extensions\/pi-voice\.js"/);
  assert.equal((qwen.match(/\/extensions\/pi-voice\.js/g) ?? []).length, 1);
  assert.match(voice, /config = lib\.mkIf voiceStt/);
});

test('voice packages Python and clipboard support but starts only the controller at login', () => {
  const source = readFileSync(new URL('../home/user/voice.nix', import.meta.url), 'utf8');
  assert.match(source, /cp \$\{\.\.\/config\/voice\}\/\*\.py "\$out\/"/);
  assert.ok(!source.includes('*.mjs'));
  assert.match(source, /runtimeInputs = \[[\s\S]*?pkgs\.wl-clipboard[\s\S]*?\];/);
  const controller = source.split('systemd.user.services.pi-voice = {')[1].split('systemd.user.services.pi-voice-stt')[0];
  assert.match(controller, /Install\.WantedBy = \[ "default.target" \];/);
  assert.match(controller, /RuntimeDirectoryMode = "0700";/);
  assert.match(controller, /RuntimeDirectoryPreserve = "yes";/);
  assert.equal((source.match(/WantedBy/g) ?? []).length, 1);
});

test('both profiles use the pinned upstream package, not the retired implementation', () => {
  const standard = readFileSync(new URL('../home/user/pi.nix', import.meta.url), 'utf8');
  const qwen = readFileSync(new URL('../home/user/local-llm.nix', import.meta.url), 'utf8');
  const pkg = readFileSync(new URL('../home/user/pi-packages/prompt-history.nix', import.meta.url), 'utf8');
  assert.match(standard, /callPackage \.\/pi-packages\/prompt-history\.nix/);
  assert.match(standard, /home\.file\."\.pi\/agent\/extensions\/prompt-history"\.source/);
  assert.ok(!standard.includes('history-search.ts'));
  assert.ok(!qwen.includes('history-search.ts'));
  assert.ok(!existsSync(new URL('../home/config/pi/extensions/history-search.ts', import.meta.url)));
  assert.match(pkg, /rev = "[a-f0-9]{40}"/);
  assert.match(pkg, /hash = "sha256-[A-Za-z0-9+/=]+"/);
  assert.match(pkg, /doCheck = true/);
});
