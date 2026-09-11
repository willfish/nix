import { readFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";

/** Accept YAML lists or comma-separated names without silently dropping bad entries. */
export function parseSkillList(value, label = "skills") {
	if (value === undefined) return [];
	const names = typeof value === "string" ? value.split(",") : value;
	if (!Array.isArray(names)) {
		throw new Error(`${label} must be a list of skill names or a comma-separated string`);
	}
	return Array.from(names, (name, index) => {
		if (typeof name !== "string" || !name.trim() || /[\s,\/\\\x00-\x1f]/u.test(name.trim())) {
			throw new Error(`${label}[${index}] must be a non-empty skill name, not a path or nested list`);
		}
		return name.trim();
	});
}

/** Resolve only against the caller's trusted active Pi skills, never filesystem discovery. */
export async function withSkills(agent, requestedSkills = [], availableSkills = []) {
	const label = `Agent ${JSON.stringify(agent.name ?? "unnamed")}`;
	const names = [...new Set([
		...parseSkillList(agent.skills, `${label} skills`),
		...parseSkillList(requestedSkills, "Requested skills"),
	])];
	if (!Array.isArray(availableSkills)) throw new Error("Available skills must be a list");

	// Validate every name before starting file reads or returning a launchable prompt.
	const selected = names.map((name) => {
		const skill = availableSkills.find((candidate) => candidate?.name === name);
		if (!skill) throw new Error(`${label}: skill ${JSON.stringify(name)} is not in the active Pi skills`);
		if (typeof skill.filePath !== "string" || !skill.filePath.trim()) {
			throw new Error(`${label}: skill ${JSON.stringify(name)} has no filePath`);
		}
		return { name, filePath: resolve(skill.filePath) };
	});
	const sections = await Promise.all(selected.map(async ({ name, filePath }) => {
		let content;
		try {
			content = await readFile(filePath, "utf8");
		} catch (cause) {
			throw new Error(`${label}: cannot read skill ${JSON.stringify(name)} at ${filePath}: ${cause.message}`, { cause });
		}
		return `## Loaded skill: ${name}\nSkill file: ${filePath}\nReference base directory: ${dirname(filePath)}\nResolve relative references, scripts, and assets against this base directory.\n\n${content}`;
	}));
	return [agent.systemPrompt, ...sections].join("\n\n");
}
