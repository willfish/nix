import { existsSync, readFileSync } from "node:fs";
import { homedir } from "node:os";
import { join } from "node:path";

export interface JevConfig {
	model: string;
	baseUrl: string;
	apiKeyEnv: string;
	keepThreshold: number;
	maxStateTokens: number;
	maxRequestTokens: number;
	maxCandidates: number;
}

export interface Config {
	chunkTokens: number;
	chunkOverlapTokens: number;
	poolTargetTokens: number;
	consolidateAtPoolTokens: number;
	compactAtContextTokens: number;
	tailTokens: number;
	journeyTargetTokens: number;
	observerConcurrency: number;
	resumeAfterMidRunCompaction: boolean;
	passive: boolean;
	debugLog: boolean;
	enabledByDefault: boolean;
	jev: JevConfig;
}

export const DEFAULTS: Config = {
	chunkTokens: 10_000,
	chunkOverlapTokens: 0,
	poolTargetTokens: 10_000,
	consolidateAtPoolTokens: 15_000,
	compactAtContextTokens: 150_000,
	tailTokens: 20_000,
	journeyTargetTokens: 1_000,
	observerConcurrency: 4,
	resumeAfterMidRunCompaction: true,
	passive: false,
	debugLog: false,
	enabledByDefault: false,
	jev: {
		model: "jev-latest",
		baseUrl: "https://api.typesafe.ai/v1/systemone",
		apiKeyEnv: "TYPESAFE_API_KEY",
		keepThreshold: 0.5,
		maxStateTokens: 25_000,
		maxRequestTokens: 30_000,
		maxCandidates: 32,
	},
};

const SETTINGS_KEY = "observational-memory-jev";
const PASSIVE_ENV = "PI_OM_PASSIVE";
const DEFAULT_ENV = "PI_OM_DEFAULT";

function positiveIntegerOrUndefined(value: unknown): number | undefined {
	return Number.isInteger(value) && typeof value === "number" && value > 0 ? value : undefined;
}

function unitIntervalOrUndefined(value: unknown): number | undefined {
	return typeof value === "number" && Number.isFinite(value) && value >= 0 && value <= 1 ? value : undefined;
}

function isRecord(value: unknown): value is Record<string, unknown> {
	return typeof value === "object" && value !== null;
}

function nonEmptyString(value: unknown): string | undefined {
	return typeof value === "string" && value.length > 0 ? value : undefined;
}

function getAgentDir(): string {
	return process.env.PI_AGENT_DIR ?? join(homedir(), ".pi", "agent");
}

function normalizeJev(value: unknown, fallback: JevConfig): JevConfig {
	if (!isRecord(value)) return fallback;
	return {
		model: nonEmptyString(value.model) ?? fallback.model,
		baseUrl: nonEmptyString(value.baseUrl) ?? fallback.baseUrl,
		apiKeyEnv: nonEmptyString(value.apiKeyEnv) ?? fallback.apiKeyEnv,
		keepThreshold: unitIntervalOrUndefined(value.keepThreshold) ?? fallback.keepThreshold,
		maxStateTokens: positiveIntegerOrUndefined(value.maxStateTokens) ?? fallback.maxStateTokens,
		maxRequestTokens: positiveIntegerOrUndefined(value.maxRequestTokens) ?? fallback.maxRequestTokens,
		maxCandidates: positiveIntegerOrUndefined(value.maxCandidates) ?? fallback.maxCandidates,
	};
}

function normalizeSettingsConfig(value: Record<string, unknown>, base: Config): Partial<Config> {
	const normalized: Partial<Config> = {};
	const numberKeys = [
		"chunkTokens",
		"chunkOverlapTokens",
		"poolTargetTokens",
		"consolidateAtPoolTokens",
		"compactAtContextTokens",
		"tailTokens",
		"journeyTargetTokens",
		"observerConcurrency",
	] as const;
	for (const key of numberKeys) {
		const normalizedValue = positiveIntegerOrUndefined(value[key]);
		if (normalizedValue !== undefined) normalized[key] = normalizedValue;
	}
	if (value.chunkOverlapTokens === 0) normalized.chunkOverlapTokens = 0;
	if (typeof value.resumeAfterMidRunCompaction === "boolean") {
		normalized.resumeAfterMidRunCompaction = value.resumeAfterMidRunCompaction;
	}
	if (typeof value.passive === "boolean") normalized.passive = value.passive;
	if (typeof value.debugLog === "boolean") normalized.debugLog = value.debugLog;
	if (typeof value.enabledByDefault === "boolean") normalized.enabledByDefault = value.enabledByDefault;
	if (value.jev !== undefined) normalized.jev = normalizeJev(value.jev, base.jev);
	return normalized;
}

function envFlag(raw: string | undefined): boolean | undefined {
	if (raw === undefined) return undefined;
	const value = raw.trim().toLowerCase();
	if (["1", "true", "yes", "on"].includes(value)) return true;
	if (["0", "false", "no", "off"].includes(value)) return false;
	return undefined;
}

export function readEnvConfig(env: NodeJS.ProcessEnv = process.env): Partial<Config> {
	const config: Partial<Config> = {};
	const passive = envFlag(env[PASSIVE_ENV]);
	if (passive !== undefined) config.passive = passive;
	const enabledByDefault = envFlag(env[DEFAULT_ENV]);
	if (enabledByDefault !== undefined) config.enabledByDefault = enabledByDefault;
	return config;
}

function readNamespacedConfig(path: string, base: Config): Partial<Config> {
	if (!existsSync(path)) return {};
	try {
		const raw = JSON.parse(readFileSync(path, "utf-8")) as Record<string, unknown>;
		const nested = raw[SETTINGS_KEY];
		return isRecord(nested) ? normalizeSettingsConfig(nested, base) : {};
	} catch {
		return {};
	}
}

export function loadConfig(cwd: string, env: NodeJS.ProcessEnv = process.env): Config {
	const globalPath = join(getAgentDir(), "settings.json");
	const projectPath = join(cwd, ".pi", "settings.json");
	const globalConfig = readNamespacedConfig(globalPath, DEFAULTS);
	const projectConfig = readNamespacedConfig(projectPath, DEFAULTS);
	const envConfig = readEnvConfig(env);
	return {
		...DEFAULTS,
		...globalConfig,
		...projectConfig,
		...envConfig,
		jev: {
			...DEFAULTS.jev,
			...globalConfig.jev,
			...projectConfig.jev,
		},
	};
}

export function resolveJevApiKey(config: Config, env: NodeJS.ProcessEnv = process.env): string {
	return (env[config.jev.apiKeyEnv] ?? env.TYPESAFE_API_KEY ?? env.PI_JEV_API_KEY ?? "").trim();
}
