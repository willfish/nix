export type JevState = string | object;

export interface NoulQuestion {
	type: "noul";
	instructions: string;
	criteria?: {
		true?: string;
		false?: string;
	};
}

export interface ChoiceQuestion {
	type: "choice";
	instructions: string;
	criteria: Record<string, string | null>;
}

export type JevQuestion = NoulQuestion | ChoiceQuestion;
export type JevQuestions = Record<string, JevQuestion>;

export interface NoulAnswer {
	type?: "noul";
	noul: number;
}

export interface ChoiceAnswer {
	type?: "choice";
	choice: string;
	confidence: number;
	probabilities?: Record<string, number>;
}

export type JevAnswer = NoulAnswer | ChoiceAnswer;

export interface JevResponse {
	model?: string;
	answers: Record<string, JevAnswer>;
	usage?: {
		input_tokens?: number;
		output_tokens?: number;
	};
}

export interface JevAsker {
	ask(state: JevState, questions: JevQuestions, signal?: AbortSignal): Promise<JevResponse>;
}

export const SYSTEM_ONE_URL = "https://api.typesafe.ai/v1/systemone";
export const DEFAULT_JEV_MODEL = "jev-latest";
