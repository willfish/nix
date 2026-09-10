// /usage: remaining allowance for the active model.
// For openai-codex (ChatGPT subscription) this queries the same backend the
// ChatGPT app uses (wham/usage) with the stored OAuth access token, refreshing
// it like pi does if expired. Usage-based providers have no window, so they
// get the session context figure instead. Never mutates models or settings.
import { readFileSync, writeFileSync, renameSync } from 'node:fs';
import { homedir } from 'node:os';
import path from 'node:path';

const USAGE_URL = 'https://chatgpt.com/backend-api/wham/usage';
const REFRESH_URL = 'https://auth.openai.com/oauth/token';
// Same public Codex OAuth client id pi uses for its token exchange.
const CODEX_CLIENT_ID = 'app_EMoamEEZ73f0CkXaXp7hrann';
const CODEX_PROVIDER = 'openai-codex';
const EXPIRY_MARGIN_MS = 5 * 60 * 1000;

function agentDir() {
  return process.env.PI_CODING_AGENT_DIR
    ? path.join(process.env.PI_CODING_AGENT_DIR, 'agent')
    : path.join(homedir(), '.pi', 'agent');
}

function formatSeconds(total) {
  if (!Number.isFinite(total) || total <= 0) return 'soon';
  const s = Math.floor(total);
  const days = Math.floor(s / 86400);
  const hours = Math.floor((s % 86400) / 3600);
  const minutes = Math.floor((s % 3600) / 60);
  if (days > 0) return `${days}d ${hours}h`;
  if (hours > 0) return `${hours}h ${minutes}m`;
  return `${Math.max(minutes, 1)}m`;
}

function windowLabel(seconds) {
  if (Number.isFinite(seconds) && seconds >= 86400) return `${Math.round(seconds / 86400)}-day`;
  if (Number.isFinite(seconds) && seconds > 0) return `${Math.round(seconds / 3600)}-hour`;
  return 'current';
}

function formatAvailableAt(value) {
  if (typeof value === 'number' && Number.isFinite(value)) {
    const ms = value < 1e12 ? value * 1000 : value;
    return new Date(ms).toISOString().replace('T', ' ').slice(0, 16) + ' UTC';
  }
  if (typeof value === 'string' && value) return value;
  return 'an unknown time';
}

async function tokenFromRegistry(ctx, provider) {
  try {
    const result = await ctx.modelRegistry?.getProviderAuth?.(provider);
    const key = result?.auth?.apiKey;
    return typeof key === 'string' && key ? key : null;
  } catch {
    return null;
  }
}

// Fallback for when the registry yields no usable token: read the credential
// file directly and refresh it with the same grant pi's login flow uses.
function tokenFromStore() {
  const store = JSON.parse(readFileSync(path.join(agentDir(), 'auth.json'), 'utf8'));
  const credential = store?.[CODEX_PROVIDER];
  if (!credential?.access || !credential.refresh) return { token: credential?.access ?? null, refreshed: false };
  if (Number.isFinite(credential.expires) && credential.expires - Date.now() > EXPIRY_MARGIN_MS) {
    return { token: credential.access, refreshed: false };
  }
  return { token: null, store, credential, refreshed: false };
}

async function refreshStoredToken(store, credential) {
  const response = await fetch(REFRESH_URL, {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: new URLSearchParams({
      grant_type: 'refresh_token',
      refresh_token: credential.refresh,
      client_id: CODEX_CLIENT_ID,
    }),
  });
  if (!response.ok) throw new Error(`token refresh failed (HTTP ${response.status})`);
  const data = await response.json();
  credential.access = data.access_token;
  if (data.refresh_token) credential.refresh = data.refresh_token;
  const seconds = Number.isFinite(data.expires_in) ? data.expires_in : 30 * 86400;
  credential.expires = Date.now() + seconds * 1000;
  const file = path.join(agentDir(), 'auth.json');
  const temp = `${file}.usage-tmp`;
  writeFileSync(temp, JSON.stringify(store, null, 2) + '\n', { mode: 0o600 });
  renameSync(temp, file);
  return credential.access;
}

async function codexToken(ctx) {
  let token = await tokenFromRegistry(ctx, CODEX_PROVIDER);
  if (token) return token;
  const stored = tokenFromStore();
  if (stored.token) return stored.token;
  if (!stored.credential) throw new Error('openai-codex credentials not found (run /login).');
  return refreshStoredToken(stored.store, stored.credential);
}

async function fetchUsage(token) {
  const response = await fetch(USAGE_URL, {
    headers: { Authorization: `Bearer ${token}` },
  });
  if (response.status === 401) throw new Error('unauthorized (401); run /login openai-codex.');
  if (!response.ok) throw new Error(`usage endpoint returned HTTP ${response.status}.`);
  return response.json();
}

function renderWindow(window, label) {
  if (!window || !Number.isFinite(window.used_percent)) return null;
  const remaining = Math.max(0, 100 - Math.round(window.used_percent));
  const reset = Number.isFinite(window.reset_after_seconds)
    ? `resets in ${formatSeconds(window.reset_after_seconds)}`
    : '';
  return `${label}: ${remaining}% remaining${reset ? `, ${reset}` : ''}`;
}

function sessionLine(ctx) {
  const tokens = ctx.getContextUsage?.()?.tokens;
  const window = ctx.model?.contextWindow;
  if (!Number.isFinite(tokens) || !Number.isFinite(window)) return null;
  return `Session context: ${Math.round(tokens / 1000)}k/${Math.round(window / 1000)}k tokens.`;
}

function renderCodex(data, ctx) {
  const lines = [];
  const plan = data.plan_type ? String(data.plan_type) : 'subscription';
  const limit = data.rate_limit;
  const reached = limit?.limit_reached || limit?.allowed === false;
  lines.push(reached
    ? `Astra (${plan}): rate limit REACHED.`
    : `Astra (${plan}): allowance available.`);
  for (const [window, label] of [[limit?.primary_window, `${windowLabel(limit?.primary_window?.limit_window_seconds)} window`], [limit?.secondary_window, `${windowLabel(limit?.secondary_window?.limit_window_seconds)} window`]]) {
    const line = renderWindow(window, label);
    if (line) lines.push(line);
  }
  const model = data.model_usage?.[ctx.model?.id];
  if (model) {
    lines.push(model.available
      ? `${ctx.model.id}: available.`
      : `${ctx.model.id}: unavailable until ${formatAvailableAt(model.available_at)}.`);
  }
  const resetCredits = data.rate_limit_reset_credits?.applicable_available_count;
  if (Number.isFinite(resetCredits) && resetCredits > 0) {
    lines.push(`Reset credits available: ${resetCredits}.`);
  }
  const credits = data.credits;
  if (credits?.overage_limit_reached || data.spend_control?.reached) {
    lines.push('Credits/overage limit reached; new usage is blocked until it resets.');
  }
  const session = sessionLine(ctx);
  if (session) lines.push(session);
  return lines;
}

export default function (pi) {
  pi.registerCommand('usage', {
    description: 'Show remaining allowance for the current model',
    async handler(_args, ctx) {
      if (!ctx.hasUI) return;
      const model = ctx.model;
      try {
        if (model?.provider !== CODEX_PROVIDER) {
          const lines = [`${model?.provider ?? '?'}/${model?.id ?? '?'}: usage-based provider (no subscription window).`];
          const session = sessionLine(ctx);
          if (session) lines.push(session);
          ctx.ui.notify(lines.join('\n'), 'info');
          return;
        }
        const token = await codexToken(ctx);
        if (!token) {
          ctx.ui.notify('No usable openai-codex token; run /login openai-codex.', 'warning');
          return;
        }
        const data = await fetchUsage(token);
        const reached = data.rate_limit?.limit_reached || data.rate_limit?.allowed === false;
        ctx.ui.notify(renderCodex(data, ctx).join('\n'), reached ? 'warning' : 'info');
      } catch (error) {
        ctx.ui.notify(`/usage: ${error.message}`, 'error');
      }
    },
  });
}
