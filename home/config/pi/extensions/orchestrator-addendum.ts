import { readFileSync } from "node:fs";
import { homedir } from "node:os";
import { join } from "node:path";

export function isOrchestratorProcess(
  env: NodeJS.ProcessEnv = process.env,
  getFlag?: (name: string) => unknown,
) {
  if (env.PI_TEAM_CHILD === "1") return false;
  if (typeof getFlag === "function" && getFlag("team-run")) return false;
  return true;
}

export function appendOrchestratorAddendum(prompt: string, addendum: string) {
  const text = addendum.trim();
  if (!text) return prompt;
  if (prompt.includes(text)) return prompt;
  return `${prompt}\n\n${text}`;
}

export function orchestratorAddendumPath(env: NodeJS.ProcessEnv = process.env) {
  const dir = env.PI_CODING_AGENT_DIR?.trim() || join(homedir(), ".pi/agent");
  return join(dir, "ORCHESTRATOR.md");
}

export function readOrchestratorAddendum(filePath: string) {
  try {
    return readFileSync(filePath, "utf8");
  } catch {
    return "";
  }
}

export default function orchestratorAddendum(
  pi: {
    getFlag?: (name: string) => unknown;
    on: (name: string, handler: (event: { systemPrompt: string }) => unknown) => void;
  },
  options: { env?: NodeJS.ProcessEnv; addendumPath?: string } = {},
) {
  const env = options.env ?? process.env;
  pi.on("before_agent_start", (event) => {
    if (!isOrchestratorProcess(env, (name) => pi.getFlag?.(name))) return;
    const filePath = options.addendumPath ?? orchestratorAddendumPath(env);
    const systemPrompt = appendOrchestratorAddendum(
      event.systemPrompt,
      readOrchestratorAddendum(filePath),
    );
    if (systemPrompt !== event.systemPrompt) return { systemPrompt };
  });
}
