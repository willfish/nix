// Ordered subsequences, with a preference for adjacent letters and word starts.
export function fuzzy(query, value) {
  const text = String(value ?? "").toLowerCase();
  let score = 0;
  for (const word of query.trim().toLowerCase().split(/\s+/).filter(Boolean)) {
    let at = -1;
    for (const letter of word) {
      const next = text.indexOf(letter, at + 1);
      if (next < 0) return null;
      score += 10 + (next === at + 1 ? 8 : 0) + (next === 0 || /[\s/_.·-]/.test(text[next - 1]) ? 5 : 0);
      score -= Math.min(next - at - 1, 12);
      at = next;
    }
    if (text.includes(word)) score += 20;
  }
  return score;
}

export function agentRows(all, terminals, spaces, query) {
  const raw = new Map(terminals.map(t => [t.address, t]));
  const places = new Map(spaces.map(s => [s.id, s]));
  return all.map(a => {
    const terminal = raw.get(a.address);
    const space = places.get(a.space);
    const display = terminal?.labels?.["user.agent.display"] || a.program?.name || a.app || "Agent";
    const path = terminal?.foreground?.cwd || terminal?.cwd || space?.folder || "";
    const title = a.title || terminal?.title || terminal?.name || "";
    const context = [a.space_name || space?.name, a.tab_name, title, path, space?.machine || "local", a.address].filter(Boolean).join(" · ");
    const score = fuzzy(query, [display, context, a.status, a.app, a.program?.name, space?.branch].join(" "));
    return { ...a, key: a.address, display, context, score };
  }).filter(a => a.score !== null).sort((a, b) => b.score - a.score);
}
