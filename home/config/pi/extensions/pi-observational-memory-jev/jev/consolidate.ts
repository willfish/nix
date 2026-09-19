import { estimateStringTokens } from "../tokens.js";
import { observationToLine, sortObservations } from "../ledger/render.js";
import type { Observation, ObservationKind } from "../ledger/types.js";
import { noulAnswer } from "./client.js";
import type { JevAsker, JevQuestions } from "./types.js";

export type TopicWrite = {
	filename: string;
	title: string;
	summary: string;
	body: string;
	updated: string;
};

export type ConsolidateResult = {
	writes: TopicWrite[];
	journey: string;
	droppedTimestamps: string[];
	requests: number;
	inputTokens: number;
	outputTokens: number;
};

const KIND_FILES: Record<ObservationKind, { filename: string; title: string }> = {
	fact: { filename: "facts.md", title: "Facts" },
	decision: { filename: "decisions.md", title: "Decisions" },
	constraint: { filename: "constraints.md", title: "Constraints" },
	question: { filename: "questions.md", title: "Questions" },
	correction: { filename: "corrections.md", title: "Corrections" },
	hypothesis: { filename: "hypotheses.md", title: "Hypotheses" },
};

function kindOf(observation: Observation): ObservationKind {
	return observation.kind ?? "fact";
}

function questionsFor(observations: Observation[]): JevQuestions {
	const questions: JevQuestions = {};
	for (const observation of observations) {
		questions[`durable_${observation.timestamp}`] = {
			type: "noul",
			instructions: `Should this observation be promoted into durable topic memory?\n${observationToLine(observation)}`,
			criteria: {
				true: "It should remain greppable after it leaves the short-term buffer.",
				false: "It was only useful in the short term and can be dropped without writing a topic file.",
			},
		};
	}
	return questions;
}

function appendTopicBody(existingBody: string, lines: string[]): string {
	const current = existingBody.trim();
	const addition = lines.join("\n");
	return current.length > 0 ? `${current}\n${addition}\n` : `${addition}\n`;
}

function firstSentence(text: string, max = 140): string {
	const trimmed = text.trim();
	if (trimmed.length <= max) return trimmed;
	return `${trimmed.slice(0, max - 1)}…`;
}

export function renderTopicFile(write: TopicWrite): string {
	return (
		`---\n` +
		`id: ${write.filename.replace(/\.md$/, "")}\n` +
		`title: ${write.title}\n` +
		`summary: ${write.summary}\n` +
		`updated: ${write.updated}\n` +
		`---\n\n` +
		write.body
	);
}

export function trimJourney(journey: string, targetTokens: number): string {
	const trimmed = journey.trim();
	if (estimateStringTokens(trimmed) <= targetTokens) return trimmed;
	const sections = trimmed.split(/\n(?=## )/);
	while (sections.length > 1 && estimateStringTokens(sections.join("\n")) > targetTokens) {
		sections.shift();
	}
	return sections.join("\n").trim();
}

export function appendJourney(existing: string | undefined, promote: Observation[], when: string): string {
	const sorted = sortObservations(promote);
	const lines = sorted.map((observation) => `- ${observationToLine(observation)}`);
	const section = `## ${when}\n${lines.join("\n")}`;
	const current = existing?.trim();
	return current ? `${current}\n\n${section}` : section;
}

/**
 * Promote overflow observations into kind-keyed topic files. Jev only answers
 * "still durable?"; the coordinator writes verbatim lines. Failed Jev calls throw
 * so the orchestrator does not tombstone an unwritten batch.
 */
export async function consolidateOverflow(
	promote: Observation[],
	existingBodies: Record<string, string>,
	existingJourney: string | undefined,
	asker: JevAsker,
	options: { keepThreshold: number; journeyTargetTokens: number; updated: string },
	signal?: AbortSignal,
): Promise<ConsolidateResult> {
	if (promote.length === 0) {
		return { writes: [], journey: existingJourney?.trim() ?? "", droppedTimestamps: [], requests: 0, inputTokens: 0, outputTokens: 0 };
	}

	const state = {
		context: "Decide which short-term observations should become durable topic memory. Do not rewrite them.",
		observations: promote.map(observationToLine),
	};
	const response = await asker.ask(state, questionsFor(promote), signal);
	const durable: Observation[] = [];
	for (const observation of promote) {
		const score = noulAnswer(response.answers, `durable_${observation.timestamp}`);
		if (score >= options.keepThreshold) durable.push(observation);
	}

	const grouped = new Map<ObservationKind, Observation[]>();
	for (const observation of durable) {
		const kind = kindOf(observation);
		const list = grouped.get(kind) ?? [];
		list.push(observation);
		grouped.set(kind, list);
	}

	const writes: TopicWrite[] = [];
	for (const [kind, observations] of grouped) {
		const meta = KIND_FILES[kind];
		const body = appendTopicBody(
			existingBodies[meta.filename] ?? "",
			sortObservations(observations).map((observation) => `- ${observationToLine(observation)}`),
		);
		writes.push({
			filename: meta.filename,
			title: meta.title,
			summary: firstSentence(observations[observations.length - 1]?.content ?? meta.title),
			body,
			updated: options.updated,
		});
	}

	const journey = trimJourney(appendJourney(existingJourney, durable, options.updated), options.journeyTargetTokens);

	const input = response.usage?.input_tokens;
	const output = response.usage?.output_tokens;
	return {
		writes,
		journey,
		droppedTimestamps: promote.map((observation) => observation.timestamp),
		requests: 1,
		inputTokens: typeof input === "number" && Number.isFinite(input) && input > 0 ? Math.round(input) : 0,
		outputTokens: typeof output === "number" && Number.isFinite(output) && output > 0 ? Math.round(output) : 0,
	};
}
