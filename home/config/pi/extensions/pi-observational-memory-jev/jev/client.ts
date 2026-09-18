import { DEFAULT_JEV_MODEL, SYSTEM_ONE_URL, type JevAsker, type JevQuestions, type JevResponse, type JevState } from "./types.js";

export interface JevClientOptions {
	apiKey?: string;
	model?: string;
	baseUrl?: string;
	fetch?: typeof fetch;
}

export function buildJevRequest(
	params: { apiKey: string; model?: string; baseUrl?: string },
	state: JevState,
	questions: JevQuestions,
): { url: string; method: "POST"; headers: Record<string, string>; body: string } {
	return {
		url: params.baseUrl ?? SYSTEM_ONE_URL,
		method: "POST",
		headers: {
			authorization: `Bearer ${params.apiKey}`,
			"content-type": "application/json",
		},
		body: JSON.stringify({
			model: params.model ?? DEFAULT_JEV_MODEL,
			state,
			questions,
		}),
	};
}

export function parseJevResponse(status: number, ok: boolean, text: string): JevResponse {
	if (!ok) {
		throw new Error(`Jev request failed (${status}): ${text.slice(0, 200)}`);
	}
	let parsed: unknown;
	try {
		parsed = JSON.parse(text);
	} catch {
		throw new Error("Jev returned malformed JSON");
	}
	if (
		parsed === null ||
		typeof parsed !== "object" ||
		!("answers" in parsed) ||
		(parsed as { answers: unknown }).answers === null ||
		typeof (parsed as { answers: unknown }).answers !== "object"
	) {
		throw new Error("Jev response is missing answers");
	}
	return parsed as JevResponse;
}

export function noulAnswer(answers: JevResponse["answers"], name: string): number {
	const answer = answers[name];
	if (!answer || !("noul" in answer) || typeof answer.noul !== "number" || !Number.isFinite(answer.noul)) {
		throw new Error(`Invalid Jev noul answer for ${name}`);
	}
	return answer.noul;
}

export function choiceAnswer(answers: JevResponse["answers"], name: string): string {
	const answer = answers[name];
	if (!answer || !("choice" in answer) || typeof answer.choice !== "string" || answer.choice.length === 0) {
		throw new Error(`Invalid Jev choice answer for ${name}`);
	}
	return answer.choice;
}

export class JevClient implements JevAsker {
	private readonly apiKey: string;
	private readonly model: string | undefined;
	private readonly baseUrl: string | undefined;
	private readonly fetcher: typeof fetch;

	constructor(options: JevClientOptions = {}) {
		this.apiKey = options.apiKey ?? process.env.TYPESAFE_API_KEY ?? "";
		this.model = options.model;
		this.baseUrl = options.baseUrl;
		this.fetcher = options.fetch ?? fetch;
	}

	async ask(state: JevState, questions: JevQuestions, signal?: AbortSignal): Promise<JevResponse> {
		if (!this.apiKey) throw new Error("TYPESAFE_API_KEY is not configured");
		const request = buildJevRequest(
			{ apiKey: this.apiKey, model: this.model, baseUrl: this.baseUrl },
			state,
			questions,
		);
		const response = await this.fetcher(request.url, {
			method: request.method,
			headers: request.headers,
			body: request.body,
			signal,
		});
		return parseJevResponse(response.status, response.ok, await response.text());
	}
}
