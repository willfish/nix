export const BOOTSTRAP = '\n\nUse skill_catalog to find task-relevant skills (coding/debugging, verification, Nix, documents, browser/auth, accounting/tax). Search names or keywords; omit query and page with offset to list all. Read the selected absolute SKILL.md path before use; resolve relative references against its directory. Manual-only skills require explicit user invocation, never automatic use. Discovery grants no authorization. Existing /skill: commands and preloaded team skills still apply.';

const bound = (value, fallback, min, max) => typeof value === 'number' && Number.isFinite(value)
  ? Math.min(max, Math.max(min, Math.floor(value))) : fallback;

const STOP_WORDS = new Set('a an and are as at be before can for from help how i in into is it me my of on or please that the this to use when with'.split(' '));

function tokens(text) {
  return new Set(text.toLowerCase().split(/[^a-z0-9]+/).filter(word => word && !STOP_WORDS.has(word)).map(word => {
    // Fold common query/trigger variants without fuzzy matching or a synonym registry.
    if (/^fail(?:s|ed|ing|ures?)?$/.test(word)) return 'fail';
    if (word.length > 4 && word.endsWith('ies')) return word.slice(0, -3) + 'y';
    if (word.length > 4 && /(?:ing|ed)$/.test(word)) return word.replace(/(?:ing|ed)$/, '').replace(/([b-df-hj-np-tv-z])\1$/, '$1');
    return word.length > 3 && !word.endsWith('ss') ? word.replace(/s$/, '') : word;
  }));
}

type CatalogSkill = {
  name: string;
  description: string;
  filePath: string;
  disableModelInvocation?: boolean;
};
type CatalogCommand = {
  source: string;
  name: string;
  sourceInfo?: { path?: string };
};

export function searchCatalog(
  skills: CatalogSkill[],
  commands: CatalogCommand[],
  { query = '', limit, offset }: { query?: string; limit?: number; offset?: number } = {},
) {
  const size = bound(limit, 5, 1, 10);
  const start = bound(offset, 0, 0, Number.MAX_SAFE_INTEGER);
  const needle = typeof query === 'string' ? query.trim().toLowerCase() : '';
  const terms = [...tokens(needle)];
  const requestedName = needle.replace(/^\/?skill:/, '');
  const ranked = skills.map(skill => {
    const registered = commands.filter(command => command.source === 'skill' && command.sourceInfo?.path === skill.filePath);
    const primary = registered.find(command => command.name === `skill:${skill.name}`) ?? registered[0];
    const aliases = registered.map(command => command.name);
    const names = tokens(`${skill.name} ${aliases.join(' ')}`);
    const haystack = tokens(`${skill.name} ${skill.description} ${skill.filePath} ${aliases.join(' ')}`);
    const exact = skill.name.toLowerCase() === requestedName ? 2
      : aliases.some(name => name.toLowerCase() === `skill:${requestedName}`) ? 1 : 0;
    const score = terms.filter(term => haystack.has(term)).length;
    const nameScore = terms.filter(term => names.has(term)).length;
    return { skill, primary, exact, score, nameScore };
  }).filter(match => !needle || match.exact > 0 || match.score > 0)
    .sort((a, b) => b.exact - a.exact || b.score - a.score || b.nameScore - a.nameScore || a.skill.name.localeCompare(b.skill.name));
  const canonical = ranked.find(match => match.exact === 2);
  const aliases = ranked.filter(match => match.exact === 1);
  const matches = canonical ? [canonical] : aliases.length === 1 ? aliases : ranked;
  const results = matches.slice(start, start + size).map(({ skill, primary }) => ({
    name: skill.name,
    trigger: skill.description.replace(/\s+/g, ' ').trim().slice(0, 180),
    path: skill.filePath,
    command: primary ? `/${primary.name}` : null,
    manualOnly: skill.disableModelInvocation === true,
  }));
  return { results, total: matches.length, offset: start, nextOffset: start + results.length < matches.length ? start + results.length : null };
}

export function replaceSkillAdvertisement(
  prompt: string,
  options: {
    selectedTools?: string[];
    skills?: unknown[];
    cwd?: string;
    customPrompt?: string;
    appendSystemPrompt?: string;
    contextFiles?: { content?: string }[];
  } | undefined,
  active: boolean,
  formatSkillsForPrompt: (skills: unknown[], reader: string) => string,
) {
  if (!active || !options?.selectedTools?.includes('skill_catalog') || !options.skills?.length || typeof options.cwd !== 'string') return prompt;
  const reader = ['read', 'bash'].find(tool => options.selectedTools.includes(tool));
  if (!reader) return prompt;
  try {
    const generated = formatSkillsForPrompt(options.skills, reader);
    if (!generated || prompt.split(generated).length !== 2 || prompt.split('<available_skills>').length !== 2 || prompt.split('</available_skills>').length !== 2) return prompt;
    const contributed = [options.customPrompt, options.appendSystemPrompt, ...(options.contextFiles ?? []).map(file => file.content)];
    if (contributed.some(text => text?.includes(generated))) return prompt;
    const index = prompt.indexOf(generated);
    const tail = prompt.slice(index + generated.length);
    const cwd = `\nCurrent working directory: ${options.cwd.replace(/\\/g, '/')}`;
    if (!tail.startsWith(cwd) || (tail.length > cwd.length && tail[cwd.length] !== '\n')) return prompt;
    return prompt.slice(0, index) + BOOTSTRAP + tail;
  } catch {
    return prompt;
  }
}
