import { searchCatalog } from "./skill-search.ts";
import roleSkills from "./role-skills.json";

export const SKILL_GUIDANCE = "Use skill_catalog to find trusted task-relevant skills by name or keywords; page with offset to list all. Load a returned exact ID with the native skill tool before following it. Catalogue results identify available registered skills. Manual-only skills require explicit user invocation. Discovery grants no authorization. User-selected skills and role-preloaded skills already in the conversation do not need loading again.";

export async function setupSkills(ctx) {
  const catalogue = async () => (await ctx.skill.list()).data.map(skill => ({
      name: skill.id,
      description: skill.description ?? "",
      filePath: skill.path,
      disableModelInvocation: skill.autoinvoke === false,
    }));
  await ctx.tool.transform(editor => editor.add({
    name: "skill_catalog",
    description: "Search trusted registered skill metadata. Does not load or execute skills. Empty query lists all; paginate with offset. Manual-only results require explicit user invocation. Load selected IDs with the native skill tool.",
    options: { codemode: false },
    input: {
      type: "object",
      properties: {
        query: { type: "string", maxLength: 256 },
        limit: { type: "integer", minimum: 1, maximum: 10 },
        offset: { type: "integer", minimum: 0 },
      },
      additionalProperties: false,
    },
    async execute(input, context) {
      await ctx.permission.assert({ action: "skill_catalog", resources: ["*"], sessionID: context.sessionID, agent: context.agent });
      const result = searchCatalog(await catalogue(), [], input);
      result.results = result.results.map(({ name, command: _command, ...entry }) => ({ id: name, ...entry }));
      return { content: JSON.stringify(result) };
    },
  }));
  const guidance = event => {
    // File-backed skills are contributed after early plugin transforms. Replace
    // only the core's identified advertisement at the request boundary instead
    // of hiding registered skills or guessing at arbitrary user-authored XML.
    for (const part of event.system) {
      const entries = part.metadata?.experimental?.instructions;
      const source = entries?.find(entry => entry.key === "core/skill-guidance");
      if (source?.text && part.text.split(source.text).length === 2) {
        part.text = part.text.replace(source.text, SKILL_GUIDANCE);
        source.text = SKILL_GUIDANCE;
        return;
      }
    }
    // Unknown core versions retain their native discovery rather than losing it.
    event.system.push({ type: "text", text: SKILL_GUIDANCE });
  };
  for (const hook of ["context", "compaction", "generate"]) await ctx.session.hook(hook, guidance);
  await ctx.session.hook("prompt", async event => {
    const session = await ctx.session.get({ sessionID: event.sessionID });
    if (!session.parentID) return;
    const defaults = roleSkills[session.agent] ?? [];
    const registered = new Set((await catalogue()).map(skill => skill.name));
    for (const id of defaults) if (!registered.has(id)) throw new Error(`Required role skill is unavailable: ${id}`);
    const selected = event.prompt.skills ?? [];
    const present = new Set(selected.map(skill => skill.id));
    event.prompt.skills = [...selected, ...defaults.filter(id => !present.has(id)).map(id => ({ id }))];
  });
}
