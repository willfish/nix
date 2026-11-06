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

export const folderKey = path => String(path || "").replace(/\/+$/, "") || "/";
export const folderName = path => folderKey(path).split("/").pop() || "/";

// Only an existing descendant of home may be used by the fallback resolver.
export function homeCandidate(home, query) {
  const parts = query.trim().split("/");
  if (!parts.length || parts.some(p => !p || p === "." || p === "..") || /[\x00-\x1f\x7f]/.test(query)) return null;
  return `${folderKey(home)}/${parts.join("/")}`;
}

export function spaceRows(spaces, query, folders) {
  const existing = spaces.filter(s => !s.active).map(s => ({
    key: `space:${s.id}`, kind: "space", id: s.id, name: s.name,
    detail: [s.folder, s.branch, s.machine || "local"].filter(Boolean).join(" · "),
    status: s.status,
    score: fuzzy(query, [s.name, s.folder, s.branch, s.machine || "local"].join(" ")),
  })).filter(s => s.score !== null).sort((a, b) => b.score - a.score);
  if (!query.trim()) return existing;
  const seen = new Set();
  for (const folder of folders) {
    const key = folderKey(folder);
    if (seen.has(key)) continue;
    seen.add(key);
    const open = spaces.find(s => !s.machine && folderKey(s.folder) === key);
    if (open) {
      // A zoxide match may find an existing space even when its name doesn't match.
      if (!open.active && !existing.some(s => s.id === open.id)) existing.push({ key: `space:${open.id}`, kind: "space", id: open.id, name: open.name, detail: `${open.folder} · local`, status: open.status });
      continue;
    }
    existing.push({ key: `create:${key}`, kind: "create", folder: key, name: `Create space: ${folderName(key)}`, detail: `${key} · local` });
  }
  return existing;
}
