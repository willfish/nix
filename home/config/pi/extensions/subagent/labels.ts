/** Switchboard presence labels follow the Pi session name unless the user sets one. */
export const LABEL_MAX = 200;

export function teamWorkLabel(agent: unknown, task?: unknown): string {
	const role = cleanLabelPart(agent) || "team";
	let detail = cleanLabelPart(task);
	if (detail.toLowerCase().startsWith("task:")) detail = detail.slice(5).trim();
	const raw = detail ? `${role}: ${detail}` : role;
	return Array.from(raw).slice(0, LABEL_MAX).join("");
}

function cleanLabelPart(value: unknown): string {
	if (typeof value !== "string") return "";
	return value.replace(/[\u0000-\u001f\u007f-\u009f\u2028\u2029]/g, " ").replace(/\s+/g, " ").trim();
}

export function applyWorkLabel(
	pi: { setSessionName?: (name: string) => void; getSessionName?: () => unknown },
	agent: unknown,
	task?: unknown,
): string | undefined {
	// Launch already passes `pi --name`. Do not replace that with the role-only fallback.
	if (task === undefined) {
		try {
			const current = cleanLabelPart(pi.getSessionName?.());
			if (current) return current;
		} catch {
			/* Missing getters are treated as unnamed. */
		}
	}
	const label = teamWorkLabel(agent, task);
	if (!label) return;
	try {
		pi.setSessionName?.(label);
	} catch {
		// Cosmetic only: a missing or failing session name must not stop the child.
	}
	return label;
}
