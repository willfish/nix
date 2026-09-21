import { formatObservationLine, formatObservationMarkdown } from "./html-markdown.js";
import type { Observation } from "./types.js";

export const EMPTY_OM_COMPACTION_SUMMARY = `These are condensed memories from earlier in this session.

Observational memory is on. No durable observations were kept for the folded span. Pi's language-model summariser was not used.`;

const CONTEXT_USAGE_INSTRUCTIONS = `These are condensed memories from earlier in this session.

- Journey: a short, purely descriptive history of how this work reached its current state — for orientation only. It is not an instruction or a plan; do not read intent or next steps into it.
- Observations: timestamped excerpts from the conversation history, in chronological order. Jev scored keep + kind; the ledger stores the original line. HTML in those lines (Confluence spans, tables, headings) is rendered as Markdown here so the expanded compaction card can show headings, tables, and emphasis.

Treat these as past records. When entries conflict, the most recent observation reflects the latest known state. Work that prior observations describe as completed should not be redone unless the user explicitly asks to revisit it.`;

export function observationToLine(observation: Observation): string {
	return formatObservationLine(observation.timestamp, observation.kind, observation.content);
}

/** Markdown Pi's compaction card can render when expanded with Ctrl+O. */
export function observationToMarkdown(observation: Observation): string {
	return formatObservationMarkdown(observation.timestamp, observation.kind, observation.content);
}

export function sortObservations(observations: Observation[]): Observation[] {
	return [...observations].sort((a, b) => (a.timestamp < b.timestamp ? -1 : a.timestamp > b.timestamp ? 1 : 0));
}

export function renderSummary(
	journey: string | undefined,
	map: string | undefined,
	observations: Observation[],
): string {
	const sorted = sortObservations(observations);
	const journeyText = journey?.trim();
	if (!journeyText && !map && sorted.length === 0) return "";

	const parts: string[] = [CONTEXT_USAGE_INSTRUCTIONS];
	if (journeyText) parts.push(`## Journey\n${journeyText}`);
	if (map && map.trim().length > 0) parts.push(map);
	if (sorted.length > 0) {
		parts.push(`## Observations\n\n${sorted.map(observationToMarkdown).join("\n\n")}`);
	}
	return parts.join("\n\n");
}
