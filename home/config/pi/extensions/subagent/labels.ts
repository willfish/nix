/** Switchboard presence labels follow the Pi session name unless this entry is set. */
export const LABEL_MAX = 200;
export const SWITCHBOARD_LABEL_ENTRY = "agent-bus-label";

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

type LabelHost = {
	setSessionName?: (name: string) => void;
	getSessionName?: () => unknown;
	appendEntry?: (customType: string, data?: unknown) => void;
};

function persistWorkLabel(pi: LabelHost, label: string, renameSession: boolean) {
	try {
		pi.appendEntry?.(SWITCHBOARD_LABEL_ENTRY, { label });
	} catch {
		/* Cosmetic only. */
	}
	if (!renameSession) return;
	try {
		pi.setSessionName?.(label);
	} catch {
		/* Cosmetic only. */
	}
}

export function applyWorkLabel(pi: LabelHost, agent: unknown, task?: unknown): string | undefined {
	// Launch already passes `pi --name`. Do not replace that with the role-only fallback.
	if (task === undefined) {
		try {
			const current = cleanLabelPart(pi.getSessionName?.());
			if (current) {
				persistWorkLabel(pi, current, false);
				return current;
			}
		} catch {
			/* Missing getters are treated as unnamed. */
		}
	}
	const label = teamWorkLabel(agent, task);
	if (!label) return;
	persistWorkLabel(pi, label, true);
	return label;
}
