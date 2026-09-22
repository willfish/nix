import { readFileSync } from "node:fs";
import { homedir } from "node:os";
import { join } from "node:path";

const MODEL_FIELDS = [
  "id",
  "name",
  "api",
  "baseUrl",
  "reasoning",
  "thinkingLevelMap",
  "input",
  "cost",
  "promptCache",
  "contextWindow",
  "maxTokens",
  "samplingParams",
  "headers",
  "compat",
] as const;

type HiddenModelsConfig = {
  providers?: string[];
  ids?: string[];
};

export function modelKey(id: string) {
  const last = id.split("/").pop() ?? id;
  return last.split(":")[0].replaceAll(".", "-").toLowerCase();
}

export function isHiddenModel(id: string, hiddenIds: Iterable<string>) {
  const hidden = hiddenIds instanceof Set
    ? hiddenIds
    : new Set([...hiddenIds].map(modelKey));
  return hidden.has(modelKey(id));
}

export function hiddenModelsPaths(
  env: NodeJS.ProcessEnv = process.env,
  home: () => string = homedir,
) {
  const dir = env.PI_CODING_AGENT_DIR?.trim() || join(home(), ".pi/agent");
  return {
    hiddenPath: join(dir, "hidden-models.json"),
    storePath: join(dir, "models-store.json"),
  };
}

export function readHiddenModelsConfig(path: string): HiddenModelsConfig {
  try {
    const parsed = JSON.parse(readFileSync(path, "utf8")) as HiddenModelsConfig;
    return {
      providers: Array.isArray(parsed.providers) ? parsed.providers : [],
      ids: Array.isArray(parsed.ids) ? parsed.ids : [],
    };
  } catch {
    return { providers: [], ids: [] };
  }
}

export function toModelDefinition(model: Record<string, unknown>) {
  const definition: Record<string, unknown> = {};
  for (const key of MODEL_FIELDS) {
    if (model[key] !== undefined) definition[key] = model[key];
  }
  return definition;
}

export function filterProviderModels(
  models: Array<Record<string, unknown>>,
  hiddenIds: Iterable<string>,
) {
  const hidden = hiddenIds instanceof Set
    ? hiddenIds
    : new Set([...hiddenIds].map(modelKey));
  return models
    .filter((model) => typeof model.id === "string" && !hidden.has(modelKey(model.id)))
    .map(toModelDefinition);
}

function readStoreModels(storePath: string, provider: string) {
  try {
    const store = JSON.parse(readFileSync(storePath, "utf8")) as Record<
      string,
      { models?: Array<Record<string, unknown>> }
    >;
    return Array.isArray(store[provider]?.models) ? store[provider].models : [];
  } catch {
    return [];
  }
}

export default function hiddenModels(
  pi: {
    registerProvider: (name: string, config: Record<string, unknown>) => void;
    on: (
      name: string,
      handler: (
        event: unknown,
        ctx: { modelRegistry?: { getAvailable?: () => unknown } },
      ) => unknown,
    ) => void;
  },
  options: {
    env?: NodeJS.ProcessEnv;
    hiddenPath?: string;
    storePath?: string;
  } = {},
) {
  const env = options.env ?? process.env;
  const paths = hiddenModelsPaths(env);
  const hiddenPath = options.hiddenPath ?? paths.hiddenPath;
  const storePath = options.storePath ?? paths.storePath;
  const config = readHiddenModelsConfig(hiddenPath);
  const providers = config.providers ?? [];
  const hiddenKeys = new Set((config.ids ?? []).map(modelKey));
  if (!providers.length || !hiddenKeys.size) return;

  const kept = (provider: string) =>
    filterProviderModels(readStoreModels(storePath, provider), hiddenKeys);

  const apply = (modelsByProvider: Record<string, Array<Record<string, unknown>>>) => {
    for (const provider of providers) {
      const models = modelsByProvider[provider] ?? [];
      if (!models.length) continue;
      pi.registerProvider(provider, {
        models,
        refreshModels: async () => {
          const next = kept(provider);
          return next.length ? next : undefined;
        },
      });
    }
  };

  apply(Object.fromEntries(providers.map((provider) => [provider, kept(provider)])));

  pi.on("session_start", async (_event, ctx) => {
    const listed = await ctx.modelRegistry?.getAvailable?.();
    const available = Array.isArray(listed) ? listed : [];
    const modelsByProvider: Record<string, Array<Record<string, unknown>>> = {};
    for (const provider of providers) {
      const fromRegistry = filterProviderModels(
        available.filter((model) => model?.provider === provider),
        hiddenKeys,
      );
      modelsByProvider[provider] = fromRegistry.length ? fromRegistry : kept(provider);
    }
    apply(modelsByProvider);
  });
}
