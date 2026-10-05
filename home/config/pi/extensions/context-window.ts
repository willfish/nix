// Session-only context budgets. Never mutate the shared model registry or settings.
// Astra's Codex catalogue advertises 272k default / 872k maximum (2026-09-09).
// These are local compaction ceilings, not a guarantee of backend acceptance.
const ENTRY = 'context-window';
const ASTRA_PRESETS = [
  { name: 'lean', tokens: 272000, label: 'Lean: 272k (default)' },
  { name: 'extended', tokens: 500000, label: 'Extended: 500k (backend untested)' },
  { name: 'maximum', tokens: 872000, label: 'Maximum: 872k (backend untested)' },
];
const COMMON_BUDGETS = [64000, 128000, 256000, 500000, 1000000];
const validTokens = tokens => Number.isSafeInteger(tokens) && tokens > 0;
const label = tokens => `${tokens / 1000}k`;

function policy(ctx) {
  const model = ctx.model;
  if (!model) return;
  // Always read the unmodified registry, including after reload and reselection.
  const registered = ctx.modelRegistry.find(model.provider, model.id);
  const defaultTokens = registered?.contextWindow;
  if (!validTokens(defaultTokens)) return;
  const astra = model.provider === 'openai-codex' && model.id === 'gpt-6-astra';
  const legacy = astra ? ASTRA_PRESETS : [];
  const maximum = Math.max(defaultTokens, ...legacy.map(item => item.tokens));
  const minimum = Math.min(defaultTokens, 64000);
  const presets = [
    ...COMMON_BUDGETS.filter(tokens => tokens < defaultTokens)
      .map(tokens => ({ name: label(tokens), tokens, label: `Budget: ${label(tokens)}` })),
    { name: 'default', tokens: defaultTokens, label: `Default: ${label(defaultTokens)} (registered window)` },
    ...legacy.filter(item => item.tokens !== defaultTokens),
  ];
  return { defaultTokens, maximum, minimum, presets, legacy };
}

export default function (pi: {
  getThinkingLevel: () => string;
  setThinkingLevel: (level: string) => void;
  setModel: (model: unknown) => Promise<boolean>;
  registerCommand: (name: string, spec: Record<string, unknown>) => void;
  on: (name: string, handler: (...args: any[]) => unknown) => void;
  appendEntry: (name: string, data: Record<string, unknown>) => void;
}) {
  let applying = false;
  let completions = [];

  function status(ctx) {
    const limits = policy(ctx);
    completions = limits ? [...limits.presets, ...limits.legacy] : [];
    if (ctx.hasUI) {
      ctx.ui.setStatus(ENTRY, limits
        ? `context: ${label(ctx.model.contextWindow)}`
        : undefined);
    }
  }

  async function apply(ctx, contextWindow) {
    if (ctx.model.contextWindow === contextWindow) {
      status(ctx);
      return true;
    }
    applying = true;
    const thinkingLevel = pi.getThinkingLevel();
    try {
      // setModel accepts a Model object. A copy preserves transport, output cap,
      // pricing and reasoning metadata without changing other/new sessions.
      if (!await pi.setModel({ ...ctx.model, contextWindow })) {
        if (ctx.hasUI) ctx.ui.notify('Context unchanged: provider authentication unavailable.', 'error');
        return false;
      }
      pi.setThinkingLevel(thinkingLevel);
      status(ctx);
      return true;
    } catch (error) {
      if (ctx.hasUI) ctx.ui.notify(`Context unchanged: ${error.message}`, 'error');
      return false;
    } finally {
      applying = false;
    }
  }

  async function restore(_event, ctx) {
    if (applying) return;
    const limits = policy(ctx);
    if (!limits) {
      status(ctx);
      return false;
    }
    const { provider, id } = ctx.model;
    let tokens = limits.defaultTokens;
    for (const entry of ctx.sessionManager.getBranch()) {
      const data = entry.data;
      if (entry.type === 'custom' && entry.customType === ENTRY
        && data?.provider === provider && data.model === id) {
        if (data.choice === 'default') tokens = limits.defaultTokens;
        else if (validTokens(data.contextWindow)
          && data.contextWindow >= limits.minimum && data.contextWindow <= limits.maximum) {
          tokens = data.contextWindow;
        }
      }
    }
    return apply(ctx, tokens);
  }

  pi.on('session_start', restore);
  pi.on('session_tree', restore);
  pi.on('model_select', restore);
  // Pi emits no model_select when the same provider/id is reselected, even
  // though it replaces the model object. Restore before prompt compaction.
  pi.on('input', restore);

  pi.registerCommand('context', {
    description: 'Session context budget: model-aware presets, default, or token count',
    getArgumentCompletions(prefix) {
      const items = [...new Map(completions.map(preset => [preset.name, preset])).values()]
        .filter(preset => preset.name.startsWith(prefix))
        .map(preset => ({ value: preset.name, label: preset.label }));
      return items.length ? items : null;
    },
    async handler(args, ctx) {
      if (!ctx.hasUI) return;
      const limits = policy(ctx);
      if (!limits) {
        ctx.ui.notify('Context unchanged: active model has no valid registered context window.', 'warning');
        return;
      }
      if (applying || !ctx.isIdle()) {
        ctx.ui.notify('Wait until Pi is idle before changing context.', 'warning');
        return;
      }
      if (!await restore(undefined, ctx)) return;
      const model = ctx.model;
      const argument = args.trim().toLowerCase();
      let preset;
      if (argument) {
        preset = [...limits.presets, ...limits.legacy].find(item => item.name === argument);
        const numeric = /^(\d+)(?:\.(\d{1,3}))?(k)?$/.exec(argument);
        if (!preset && numeric) {
          // Parse decimal k values as whole tokens, avoiding floating-point drift.
          const tokens = numeric[3]
            ? Number(numeric[1]) * 1000 + Number((numeric[2] ?? '').padEnd(3, '0'))
            : numeric[2] ? NaN : Number(numeric[1]);
          if (validTokens(tokens) && tokens >= limits.minimum && tokens <= limits.maximum) {
            preset = { name: argument, tokens };
          }
        }
        if (!preset) {
          ctx.ui.notify(`Usage: /context [default|${limits.presets.map(item => label(item.tokens)).join('|')}]. `
            + `Budget must be ${label(limits.minimum)} to ${label(limits.maximum)} tokens.`, 'warning');
          return;
        }
      } else {
        const presets = [...limits.presets];
        if (!presets.some(item => item.tokens === model.contextWindow)) {
          presets.push({ name: 'custom', tokens: model.contextWindow, label: `Custom: ${label(model.contextWindow)}` });
        }
        const labels = presets.map(item => `${item.label}${item.tokens === model.contextWindow ? ' [current]' : ''}`);
        const selected = await ctx.ui.select('Session context budget', labels);
        preset = presets[labels.indexOf(selected)];
        if (!preset) return;
      }
      // A default entry must supersede an earlier numeric choice even when both
      // currently resolve to the same window, so it follows future registry changes.
      if (preset.tokens === model.contextWindow && preset.name !== 'default') return;
      const tokens = ctx.getContextUsage()?.tokens;
      if (preset.tokens < model.contextWindow && (!Number.isFinite(tokens) || tokens >= preset.tokens)) {
        const usage = Number.isFinite(tokens) ? `${tokens} tokens` : 'unknown usage';
        if (!await ctx.ui.confirm('Reduce context budget?',
          `Current context: ${usage}. Reducing to ${preset.tokens / 1000}k may trigger lossy auto-compaction on the next turn. Continue?`)) return;
      }
      // Dialogs yield control. Do not apply a stale choice to a different model
      // or change limits underneath a running request.
      if (ctx.model !== model || applying || !ctx.isIdle()) {
        ctx.ui.notify('Session changed while choosing. Run /context again when idle.', 'warning');
        return;
      }
      if (!await apply(ctx, preset.tokens)) return;
      pi.appendEntry(ENTRY, { provider: model.provider, model: model.id, contextWindow: preset.tokens,
        ...(preset.name === 'default' ? { choice: 'default' } : {}) });
      ctx.ui.notify(`Session context: ${label(preset.tokens)}. New sessions use the registered default (${label(limits.defaultTokens)}). `
        + 'Auto-compaction still reserves response space; increasing context cannot restore compacted details.'
        + (preset.tokens > limits.defaultTokens ? ' Larger requests may use more subscription allowance; backend acceptance is untested.' : ''), 'info');
    },
  });
}
