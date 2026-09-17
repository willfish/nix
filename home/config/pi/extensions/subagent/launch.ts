export const THINKING_LEVELS = ["off", "minimal", "low", "medium", "high", "xhigh", "max"] as const;
export type AgentThinkingLevel = (typeof THINKING_LEVELS)[number];

export interface AgentLaunchOverride {
	model?: string;
	thinking?: string;
}

export interface AgentLaunchDefaults {
	model?: string;
	thinkingLevel?: string;
}

export interface AgentLaunchConfig {
	model?: string;
	thinking?: AgentThinkingLevel;
}

export function nonemptyString(value: unknown): string | undefined {
	return typeof value === "string" && value.trim() ? value.trim() : undefined;
}

/**
 * YAML 1.1 treats `off` as boolean false. Accept that spelling so role files
 * can write `thinking: off` without quoting.
 */
export function parseThinkingLevel(value: unknown, label: string): AgentThinkingLevel | undefined {
	if (value === undefined) return undefined;
	if (value === false) return "off";
	const thinking = nonemptyString(value);
	if (thinking && (THINKING_LEVELS as readonly string[]).includes(thinking)) {
		return thinking as AgentThinkingLevel;
	}
	throw new Error(`${label} must be one of ${THINKING_LEVELS.join(", ")}`);
}

export function resolveLaunchConfig(
	agent: { model?: string; thinking?: AgentThinkingLevel } = {},
	override: AgentLaunchOverride = {},
	session: AgentLaunchDefaults = {},
): AgentLaunchConfig {
	const thinking =
		parseThinkingLevel(override.thinking, "Override thinking") ??
		agent.thinking ??
		parseThinkingLevel(session.thinkingLevel, "Session thinking");
	return {
		model: nonemptyString(override.model) ?? nonemptyString(agent.model) ?? nonemptyString(session.model),
		thinking,
	};
}

export function agentLaunchFlags(launch: AgentLaunchConfig): string[] {
	const args: string[] = [];
	if (launch.model) args.push("--model", launch.model);
	if (launch.thinking) args.push("--thinking", launch.thinking);
	return args;
}
