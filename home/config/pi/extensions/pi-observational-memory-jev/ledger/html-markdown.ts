/**
 * Turn verbatim HTML excerpts (Confluence spans, tables, headings) into Markdown
 * Pi's compaction view can actually render when expanded with Ctrl+O.
 * The ledger stays verbatim; only the printed summary is rewritten.
 */

const ENTITIES: Record<string, string> = {
	amp: "&",
	lt: "<",
	gt: ">",
	quot: '"',
	apos: "'",
	nbsp: " ",
	ndash: "–",
	mdash: "—",
	lsquo: "‘",
	rsquo: "’",
	ldquo: "“",
	rdquo: "”",
};

export function decodeHtmlEntities(text: string): string {
	return text
		.replace(/&#x([0-9a-fA-F]+);/g, (_, hex: string) => codePoint(parseInt(hex, 16)))
		.replace(/&#(\d+);/g, (_, dec: string) => codePoint(Number(dec)))
		.replace(/&([a-zA-Z]+);/g, (match, name: string) => ENTITIES[name] ?? match);
}

function codePoint(value: number): string {
	if (!Number.isInteger(value) || value < 0 || value > 0x10ffff) return "";
	return String.fromCodePoint(value);
}

function looksLikeHtml(text: string): boolean {
	return /<\/?[a-zA-Z][^>]*>/.test(text);
}

function inlineMarkdown(html: string): string {
	let text = html;
	text = text.replace(/<br\s*\/?>/gi, " ");
	text = text.replace(/<\/?(?:p|div)\b[^>]*>/gi, " ");
	text = text.replace(/<span\b[^>]*>([\s\S]*?)<\/span>/gi, "$1");
	text = text.replace(/<(?:strong|b)\b[^>]*>([\s\S]*?)<\/(?:strong|b)>/gi, "**$1**");
	text = text.replace(/<(?:em|i)\b[^>]*>([\s\S]*?)<\/(?:em|i)>/gi, "*$1*");
	text = text.replace(/<(?:code)\b[^>]*>([\s\S]*?)<\/code>/gi, "`$1`");
	text = text.replace(/<a\b[^>]*href=["']([^"']+)["'][^>]*>([\s\S]*?)<\/a>/gi, "[$2]($1)");
	text = text.replace(/<[^>]+>/g, "");
	return decodeHtmlEntities(text).replace(/\s+/g, " ").trim();
}

function tableToMarkdown(tableHtml: string): string {
	const rows = [...tableHtml.matchAll(/<tr\b[^>]*>([\s\S]*?)<\/tr>/gi)].map((match) => match[1]);
	const parsed = rows
		.map((row) =>
			[...row.matchAll(/<(th|td)\b[^>]*>([\s\S]*?)<\/(?:th|td)>/gi)].map((match) => inlineMarkdown(match[2]) || " "),
		)
		.filter((cells) => cells.length > 0);
	if (parsed.length === 0) return inlineMarkdown(tableHtml);
	const width = Math.max(...parsed.map((row) => row.length));
	const padded = parsed.map((row) => {
		const copy = [...row];
		while (copy.length < width) copy.push("");
		return copy;
	});
	const header = padded[0];
	const separator = header.map(() => "---");
	return [
		`| ${header.join(" | ")} |`,
		`| ${separator.join(" | ")} |`,
		...padded.slice(1).map((row) => `| ${row.join(" | ")} |`),
	].join("\n");
}

function replaceTables(html: string): string {
	return html.replace(/<table\b[^>]*>[\s\S]*?<\/table>/gi, (table) => `\n\n${tableToMarkdown(table)}\n\n`);
}

export function htmlToMarkdown(text: string): string {
	if (!text) return "";
	if (!looksLikeHtml(text)) return decodeHtmlEntities(text).trim();

	let markdown = replaceTables(text);
	markdown = markdown.replace(/<br\s*\/?>/gi, "\n");
	markdown = markdown.replace(/<\/(?:p|div|h[1-6])\s*>/gi, "\n\n");
	markdown = markdown.replace(/<(?:p|div)\b[^>]*>/gi, "");
	markdown = markdown.replace(/<h([1-6])\b[^>]*>([\s\S]*?)<\/h\1>/gi, (_, depth: string, inner: string) => {
		return `${"#".repeat(Number(depth))} ${inlineMarkdown(inner)}\n\n`;
	});
	markdown = markdown.replace(/<span\b[^>]*>([\s\S]*?)<\/span>/gi, (_, inner: string) => {
		const content = inlineMarkdown(inner);
		if (!content) return "";
		if (/^\*\*.+\*\*$/.test(content)) return content;
		return `**${content}**`;
	});
	markdown = markdown.replace(/<(?:strong|b)\b[^>]*>([\s\S]*?)<\/(?:strong|b)>/gi, "**$1**");
	markdown = markdown.replace(/<(?:em|i)\b[^>]*>([\s\S]*?)<\/(?:em|i)>/gi, "*$1*");
	markdown = markdown.replace(/<(?:code)\b[^>]*>([\s\S]*?)<\/code>/gi, "`$1`");
	markdown = markdown.replace(/<a\b[^>]*href=["']([^"']+)["'][^>]*>([\s\S]*?)<\/a>/gi, "[$2]($1)");
	markdown = markdown.replace(/<li\b[^>]*>([\s\S]*?)<\/li>/gi, "- $1\n");
	markdown = markdown.replace(/<\/?(?:ul|ol)\b[^>]*>/gi, "\n");
	markdown = markdown.replace(/<[^>]+>/g, "");
	markdown = decodeHtmlEntities(markdown);
	markdown = markdown.replace(/[^\S\n]+/g, " ");
	markdown = markdown.replace(/ *\n */g, "\n");
	markdown = markdown.replace(/\n{3,}/g, "\n\n");
	return markdown.trim();
}

export function toSingleLine(text: string): string {
	return text.replace(/\s+/g, " ").trim();
}

export function formatObservationLine(timestamp: string, kind: string | undefined, content: string): string {
	const label = kind ? `[${kind}] ` : "";
	const body = toSingleLine(htmlToMarkdown(content) || content);
	return `${timestamp}  ${label}${body}`;
}

export function formatObservationMarkdown(timestamp: string, kind: string | undefined, content: string): string {
	const label = kind ? `[${kind}]` : "observation";
	const heading = `### ${label} · ${timestamp}`;
	const body = htmlToMarkdown(content).trim();
	if (!body) return heading;
	return `${heading}\n\n${body}`;
}
