export const qualify = (id, on) => on ? `${on}/${id}` : id;

export function activeTarget(spaces) {
  const space = spaces.find(s => s.active);
  const tab = space?.tabs.find(t => t.active);
  if (!tab) return null;
  const on = space.machine || null;
  const local = id => {
    if (!on) return id;
    const prefix = `${on}/`;
    if (!id.startsWith(prefix)) throw new Error("The selected tab has an unexpected machine ID.");
    return id.slice(prefix.length);
  };
  return { space: local(space.id), tab: local(tab.id), on };
}

export function sameTarget(first, second) {
  return !!first && !!second && first.space === second.space && first.tab === second.tab && first.on === second.on;
}

export function balancePlan(document, target) {
  if (!Array.isArray(document?.spaces)) throw new Error("The desktop layout format is unsupported. Nothing was changed.");
  const space = document.spaces.find(s => s.id === target.space);
  const tab = space?.tabs?.find(t => t.id === target.tab);
  if (!tab) throw new Error("That tab is no longer available. Nothing was changed.");
  const updates = [];
  const paneIds = [];
  function visit(node) {
    if (typeof node?.id !== "string") throw new Error("The pane layout has no stable ID. Nothing was changed.");
    if (node.type === "block" && (node.content?.terminal || node.content?.mod)) {
      paneIds.push(qualify(node.id, target.on));
      return 1;
    }
    if (!["row", "column"].includes(node.type) || !Array.isArray(node.children) || !node.children.length) {
      throw new Error("The pane layout format is unsupported. Nothing was changed.");
    }
    const counts = node.children.map(visit);
    if (counts.length > 1) {
      node.children.forEach((child, i) => updates.push({ id: qualify(child.id, target.on), weight: counts[i] }));
    }
    return counts.reduce((sum, count) => sum + count, 0);
  }
  visit(tab.root);
  return { updates, paneIds };
}

export function applyPlan(plan, livePaneIds, resize) {
  const expected = [...plan.paneIds].sort();
  const live = [...livePaneIds].sort();
  if (expected.length !== live.length || expected.some((id, i) => id !== live[i])) {
    throw new Error("The panes changed while balancing. Press Ctrl+B, then Shift+E again.");
  }
  // Use native resizes even when saved weights already match: the renderer keeps its own sizes.
  for (const update of plan.updates) resize(update);
}

export function decodeReply(result) {
  if (result.code !== 0) throw new Error(result.stderr.trim() || "Could not reach Herdr core.");
  let reply;
  try { reply = JSON.parse(result.stdout); }
  catch { throw new Error("Herdr core returned an unreadable layout response."); }
  if (reply.error) throw new Error(reply.error.message || "Herdr core could not read the layout.");
  if (reply.id !== "balance-panes" || !reply.result) throw new Error("Herdr core returned an unexpected layout response.");
  return reply.result;
}
