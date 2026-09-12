import { formatSkillsForPrompt, type ExtensionAPI, type Skill } from '@earendil-works/pi-coding-agent';
import { Type } from 'typebox';
import { searchCatalog, replaceSkillAdvertisement } from './catalog.ts';

export default function skillCatalog(pi: ExtensionAPI) {
  let skills: Skill[] | undefined;
  pi.on('session_start', () => { skills = undefined; });
  pi.on('session_shutdown', () => { skills = undefined; });

  pi.registerTool({
    name: 'skill_catalog',
    label: 'Skill catalogue',
    description: 'Search trusted loaded skill metadata by name, alias or keywords. Empty query lists all; paginate with offset. Default 5, maximum 10 results; triggers capped at 180 characters. Manual-only entries require explicit user invocation. Does not load or execute skills.',
    parameters: Type.Object({
      query: Type.Optional(Type.String({ maxLength: 256 })),
      limit: Type.Optional(Type.Integer({ minimum: 1, maximum: 10 })),
      offset: Type.Optional(Type.Integer({ minimum: 0, maximum: Number.MAX_SAFE_INTEGER })),
    }),
    async execute(_id, params) {
      if (!skills) throw new Error('Trusted skill registry unavailable; use the original skill advertisement or /skill: commands.');
      const result = searchCatalog(skills, pi.getCommands(), params);
      return { content: [{ type: 'text', text: JSON.stringify(result) }], details: {} };
    },
  });

  pi.on('before_agent_start', event => {
    skills = event.systemPromptOptions?.skills;
    const systemPrompt = replaceSkillAdvertisement(event.systemPrompt, event.systemPromptOptions,
      pi.getActiveTools().includes('skill_catalog'), formatSkillsForPrompt);
    if (systemPrompt !== event.systemPrompt) return { systemPrompt };
  });
}
