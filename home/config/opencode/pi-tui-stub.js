// Satisfies the Switchboard viewer's import without loading Pi's TUI.
export function truncateToWidth(text, width) {
  const value = String(text ?? "");
  return width > 0 && value.length > width ? value.slice(0, width) : value;
}

export function wrapTextWithAnsi(text) {
  return String(text ?? "").split("\n");
}
