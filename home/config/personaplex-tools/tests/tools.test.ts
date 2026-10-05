import assert from "node:assert/strict";
import { spawn, spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import {
	chmodSync,
	existsSync,
	mkdirSync,
	mkdtempSync,
	readFileSync,
	readdirSync,
	readlinkSync,
	rmSync,
	statSync,
	symlinkSync,
	writeFileSync,
} from "node:fs";
import { createServer } from "node:https";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { after, before, test } from "node:test";
import { gzipSync } from "node:zlib";
const fixture = process.env.PERSONAPLEX_FIXTURE,
	binary = process.env.PERSONAPLEX_MODELS_BIN,
	patcher = process.env.PERSONAPLEX_PATCH_BIN;
assert.ok(
	fixture && binary && patcher,
	"Set PERSONAPLEX_FIXTURE, PERSONAPLEX_MODELS_BIN and PERSONAPLEX_PATCH_BIN",
);
const token = "fixture-not-a-real-hf-token";
const tlsRoot = mkdtempSync(join(tmpdir(), "personaplex-tls-")),
	key = join(tlsRoot, "key.pem"),
	cert = join(tlsRoot, "cert.pem");
before(() => {
	const r = spawnSync(
		"openssl",
		[
			"req",
			"-x509",
			"-newkey",
			"rsa:2048",
			"-nodes",
			"-keyout",
			key,
			"-out",
			cert,
			"-days",
			"1",
			"-subj",
			"/CN=localhost",
			"-addext",
			"subjectAltName=DNS:localhost,IP:127.0.0.1",
		],
		{ encoding: "utf8" },
	);
	assert.equal(r.status, 0, r.stderr);
});
after(() => rmSync(tlsRoot, { recursive: true, force: true }));
function root(t: any) {
	const r = mkdtempSync(join(tmpdir(), "personaplex-tools-"));
	t.after(() => rmSync(r, { recursive: true, force: true }));
	return r;
}
function invoke(cmd: string, args: string[], options: any = {}) {
	return new Promise<any>((resolve, reject) => {
		const child = spawn(cmd, args, {
			env: {
				...process.env,
				HF_TOKEN: token,
				HF_HOME: tlsRoot,
				SSL_CERT_FILE: cert,
				...options.env,
			},
			stdio: ["pipe", "pipe", "pipe"],
		});
		let stdout = "",
			stderr = "";
		const timeout = setTimeout(() => {
			child.kill("SIGKILL");
			reject(new Error("Fixture timeout"));
		}, 15000);
		child.stdout.on("data", (d) => (stdout += d));
		child.stderr.on("data", (d) => (stderr += d));
		child.on("error", reject);
		child.on("close", (status, signal) => {
			clearTimeout(timeout);
			if (signal) reject(new Error(`${signal}: ${stderr}`));
			else resolve({ status, stdout, stderr });
		});
		child.stdin.end(options.input || "");
	});
}
async function server(
	t: any,
	handler: any,
	tls: any = { key: readFileSync(key), cert: readFileSync(cert) },
) {
	const requests: any[] = [];
	const s = createServer(tls, (q, r) => {
		requests.push({ path: q.url, auth: q.headers.authorization });
		handler(q, r, requests.length);
	});
	await new Promise<void>((resolve) => s.listen(0, "127.0.0.1", resolve));
	t.after(() => {
		s.closeAllConnections();
		s.close();
	});
	return { url: `https://127.0.0.1:${(s.address() as any).port}`, requests };
}
const hash = (data: any) => createHash("sha256").update(data).digest("hex");
function archive(entries: any[]) {
	const out: Buffer[] = [];
	for (const entry of entries) {
		const data = Buffer.from(entry.data || ""),
			h = Buffer.alloc(512);
		const field = (offset: number, len: number, value: string) =>
			h.write(value, offset, len, "utf8");
		field(0, 100, entry.name);
		field(100, 8, (entry.mode ?? 0o644).toString(8).padStart(7, "0") + "\0");
		field(108, 8, "0000000\0");
		field(116, 8, "0000000\0");
		field(124, 12, data.length.toString(8).padStart(11, "0") + "\0");
		field(
			136,
			12,
			(entry.mtime ?? 1234567890).toString(8).padStart(11, "0") + "\0",
		);
		h.fill(32, 148, 156);
		field(156, 1, entry.type || "0");
		if (entry.link) field(157, 100, entry.link);
		field(257, 6, "ustar\0");
		field(263, 2, "00");
		field(
			148,
			8,
			h
				.reduce((sum, b) => sum + b, 0)
				.toString(8)
				.padStart(6, "0") + "\0 ",
		);
		out.push(h, data, Buffer.alloc((512 - (data.length % 512)) % 512));
	}
	return gzipSync(Buffer.concat([...out, Buffer.alloc(1024)]));
}
function data(t: any) {
	const dir = root(t),
		destination = join(dir, "data"),
		manifest = join(dir, "assets.json");
	const blobs: any = {
		"model.safetensors": Buffer.from("model-data"),
		"tokenizer.bin": Buffer.from("tokenizer-data"),
		"voices.tgz": archive([
			{ name: "voices", type: "5", mode: 0o777 },
			{ name: "voices/NATF2.pt", data: "voice-data" },
		]),
		"dist.tgz": archive([
			{ name: "dist", type: "5" },
			{ name: "dist/index.html", data: "<head>test</head>" },
		]),
	};
	const assets = Object.entries(blobs).map(([name, value]: any) => ({
		name,
		bytes: value.length,
		sha256: hash(value),
	}));
	writeFileSync(manifest, JSON.stringify(assets));
	return { dir, destination, manifest, blobs, assets };
}
function install(f: any, base: string, check = false, options: any = {}) {
	return invoke(
		fixture!,
		[
			"install",
			f.destination,
			f.manifest,
			base,
			check ? "1" : "0",
			String(options.idle ?? 2000),
		],
		options,
	);
}
function noPartial(path: string) {
	if (existsSync(path))
		for (const name of readdirSync(path, { recursive: true }))
			assert.ok(!String(name).endsWith(".partial"), String(name));
}
const handler =
	"    async def handle_chat(self, request):\n        ws = web.WebSocketResponse()";
const webRoot =
	'return web.FileResponse(os.path.join(static_path, "index.html"))';

test("manifest pins all five assets, the revision, order, 64-bit sizes and timeout", async () => {
	const r = await invoke(fixture!, ["assets"]);
	assert.equal(r.status, 0);
	const p = JSON.parse(r.stdout);
	assert.equal(
		p.base,
		"https://huggingface.co/nvidia/personaplex-7b-v1/resolve/fdaf4090a61cb315c138a1faee287ffd6c716309",
	);
	assert.equal(p.idle_ms, 60000);
	assert.deepEqual(p.assets, [
		{
			name: "model.safetensors",
			bytes: 16742874000,
			sha256:
				"db1290db583cdaa6cb4de444ed279e0b586ca2a372b41434b07a7461c8c0e2f4",
		},
		{
			name: "tokenizer-e351c8d8-checkpoint125.safetensors",
			bytes: 384644900,
			sha256:
				"09b782f0629851a271227fb9d36db65c041790365f11bbe5d3d59369cf863f50",
		},
		{
			name: "tokenizer_spm_32k_3.model",
			bytes: 552778,
			sha256:
				"78d4336533ddc26f9acf7250d7fb83492152196c6ea4212c841df76933f18d2d",
		},
		{
			name: "voices.tgz",
			bytes: 6095521,
			sha256:
				"8564e9ca7a06ca723b07c3a77c623f0faa5937d04b2647b3a727b06c5ca0b7bb",
		},
		{
			name: "dist.tgz",
			bytes: 598195,
			sha256:
				"8de47fe2477491fac3dca404185d0430c8d39f9e0daf4c90ada5f03fdf830f45",
		},
	]);
});
test("public CLI validates arguments and check-only creates a private root without accessing credentials", async (t) => {
	const dir = root(t),
		path = join(dir, "parent/data");
	for (const args of [[], ["--bad"], ["--data-dir"]])
		assert.equal((await invoke(binary!, args)).status, 2);
	assert.equal((await invoke(binary!, ["--help"])).status, 0);
	const r = await invoke(binary!, ["--data-dir", path, "--check-only"], {
		env: { HF_TOKEN: undefined, HF_HOME: join(dir, "absent") },
	});
	assert.equal(r.status, 1);
	assert.match(
		r.stderr,
		/PersonaPlex setup: Missing or invalid model.safetensors/,
	);
	assert.equal(statSync(path).mode & 0o777, 0o700);
	assert.deepEqual(readdirSync(path), []);
});
test("credentials prefer the environment, fall back through HF_HOME, and trim Python Unicode whitespace", async (t) => {
	const dir = root(t);
	writeFileSync(join(dir, "token"), "\u2007" + token + "\u0085\u001c");
	for (const value of [token, ""]) {
		const r = await invoke(fixture!, ["token", token], {
			env: { HF_TOKEN: value, HF_HOME: dir },
		});
		assert.equal(r.status, 0, r.stderr);
		assert.equal(r.stdout, "match\n");
	}
	const home = root(t);
	mkdirSync(join(home, ".cache/huggingface"), { recursive: true });
	writeFileSync(join(home, ".cache/huggingface/token"), token + "\n");
	const r = await invoke(fixture!, ["token", token], {
		env: { HF_TOKEN: undefined, HF_HOME: undefined, HOME: home },
	});
	assert.equal(r.status, 0, r.stderr);
});
test("missing or malformed credentials fail closed without printing their contents", async (t) => {
	const dir = root(t);
	for (const env of [
		{ HF_TOKEN: undefined, HF_HOME: dir },
		{ HF_TOKEN: "private\r\ninjected", HF_HOME: dir },
	]) {
		const r = await invoke(fixture!, ["token", token], { env });
		assert.equal(r.status, 1);
		assert.ok(!r.stderr.includes("private"));
	}
	writeFileSync(join(dir, "token"), Buffer.from([255]));
	assert.equal(
		(
			await invoke(fixture!, ["token", token], {
				env: { HF_TOKEN: undefined, HF_HOME: dir },
			})
		).status,
		1,
	);
});
test("cached assets and required extracted files pass check-only with no token reads or requests", async (t) => {
	const f = data(t);
	mkdirSync(f.destination);
	for (const a of f.assets)
		writeFileSync(join(f.destination, a.name), f.blobs[a.name]);
	mkdirSync(join(f.destination, "voices"));
	mkdirSync(join(f.destination, "dist"));
	writeFileSync(join(f.destination, "voices/NATF2.pt"), "voice");
	writeFileSync(join(f.destination, "dist/index.html"), "html");
	const remote = await server(t, (_q: any, r: any) => r.end("not needed"));
	const r = await install(f, remote.url, true, {
		env: { HF_TOKEN: undefined, HF_HOME: join(f.dir, "does-not-exist") },
	});
	assert.equal(r.status, 0, r.stderr);
	assert.equal(remote.requests.length, 0);
	assert.deepEqual(
		r.stdout.trim().split("\n"),
		f.assets.map((a: any) => `Verified ${a.name}`),
	);
	rmSync(join(f.destination, "dist/index.html"));
	const missing = await install(f, remote.url, true);
	assert.match(missing.stderr, /Missing dist\/index.html/);
});
test("the terms/access gate is reached only when a download is needed", async (t) => {
	const f = data(t),
		remote = await server(t, (_q: any, r: any) => r.end("not needed"));
	const r = await install(f, remote.url, false, {
		env: { HF_TOKEN: undefined, HF_HOME: join(f.dir, "no-token") },
	});
	assert.equal(r.status, 1);
	assert.match(r.stderr, /Accept the PersonaPlex model terms/);
	assert.equal(remote.requests.length, 0);
	noPartial(f.destination);
});
test("valid TLS downloads install atomically, extract both archives and repeat offline without credentials", async (t) => {
	const f = data(t),
		remote = await server(t, (q: any, r: any) =>
			r.end(f.blobs[q.url.slice(1)]),
		);
	const r = await install(f, remote.url);
	assert.equal(r.status, 0, r.stderr);
	assert.equal(remote.requests.length, 4);
	assert.ok(remote.requests.every((q) => q.auth === `Bearer ${token}`));
	assert.equal(
		statSync(join(f.destination, "model.safetensors")).mode & 0o777,
		0o600,
	);
	assert.equal(
		readFileSync(join(f.destination, "voices/NATF2.pt"), "utf8"),
		"voice-data",
	);
	assert.equal(
		readFileSync(join(f.destination, "dist/index.html"), "utf8"),
		"<head>test</head>",
	);
	noPartial(f.destination);
	const again = await install(f, remote.url, false, {
		env: { HF_TOKEN: undefined, HF_HOME: join(f.dir, "no-token") },
	});
	assert.equal(again.status, 0, again.stderr);
	assert.equal(remote.requests.length, 4);
});
test("HTTP errors, oversize data and bad hashes fail once and leave previous files untouched", async (t) => {
	for (const kind of ["http", "oversize", "hash"]) {
		const f = data(t);
		mkdirSync(f.destination);
		writeFileSync(join(f.destination, "model.safetensors"), "old");
		const remote = await server(t, (_q: any, r: any) => {
			if (kind === "http") {
				r.writeHead(403, { "Content-Length": 10000 });
				r.flushHeaders();
			} else r.end(kind === "oversize" ? "x".repeat(1000) : "wrong-size");
		});
		const result = await install(f, remote.url);
		assert.equal(result.status, 1);
		assert.equal(remote.requests.length, 1);
		assert.equal(
			readFileSync(join(f.destination, "model.safetensors"), "utf8"),
			"old",
		);
		noPartial(f.destination);
		assert.ok(!result.stderr.includes(token));
		if (kind === "http") assert.match(result.stderr, /HTTP 403/);
	}
});
test("pinned bytes override overstated Content-Length but incomplete chunk framing still fails", async (t) => {
	for (const framing of ["length", "chunked"]) {
		const f = data(t),
			remote = await server(t, (q: any, r: any) => {
				const body = f.blobs[q.url.slice(1)];
				if (framing === "length") {
					r.writeHead(200, { "Content-Length": body.length + 100 });
					r.end(body);
				} else {
					const socket = r.socket;
					socket.write(
						"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n" +
							body.length.toString(16) +
							"\r\n",
					);
					socket.write(body);
					socket.end("\r\n");
				}
			});
		const r = await install(f, remote.url);
		assert.equal(r.status, framing === "length" ? 0 : 1, r.stderr);
		assert.equal(remote.requests.length, framing === "length" ? 4 : 1);
		noPartial(f.destination);
	}
});
test("same-authority HTTPS redirects keep credentials and ignore intermediate bodies", async (t) => {
	const f = data(t),
		remote = await server(t, (q: any, r: any) => {
			if (q.url.startsWith("/final/")) r.end(f.blobs[q.url.slice(7)]);
			else {
				r.writeHead(302, { Location: "/final" + q.url });
				r.end("ignored body");
			}
		});
	const r = await install(f, remote.url);
	assert.equal(r.status, 0, r.stderr);
	assert.ok(remote.requests.every((q) => q.auth === `Bearer ${token}`));
});
test("cross-authority redirects drop credentials permanently even when returning to the original host", async (t) => {
	const f = data(t);
	let original: any;
	const other = await server(t, (q: any, r: any) => {
		r.writeHead(302, { Location: original.url + "/final" + q.url });
		r.end();
	});
	original = await server(t, (q: any, r: any) => {
		if (q.url.startsWith("/final/")) r.end(f.blobs[q.url.slice(7)]);
		else {
			r.writeHead(302, { Location: other.url + q.url });
			r.end();
		}
	});
	const r = await install(f, original.url);
	assert.equal(r.status, 0, r.stderr);
	assert.ok(other.requests.every((q) => q.auth === undefined));
	assert.ok(
		original.requests
			.filter((q) => q.path.startsWith("/final/"))
			.every((q) => q.auth === undefined),
	);
});
test("authority spelling changes also drop credentials, matching the original netloc comparison", async (t) => {
	const f = data(t);
	let remote: any;
	remote = await server(t, (q: any, r: any) => {
		if (q.url.startsWith("/final/")) r.end(f.blobs[q.url.slice(7)]);
		else {
			r.writeHead(302, {
				Location:
					remote.url.replace("127.0.0.1", "LOCALHOST") + "/final" + q.url,
			});
			r.end();
		}
	});
	const r = await install(f, remote.url.replace("127.0.0.1", "localhost"));
	assert.equal(r.status, 0, r.stderr);
	assert.ok(
		remote.requests
			.filter((q) => q.path.startsWith("/final/"))
			.every((q) => q.auth === undefined),
	);
});
test("progressing TLS streams can exceed the idle deadline without a whole-download timeout", async (t) => {
	const f = data(t),
		remote = await server(t, (q: any, r: any) => {
			const body = f.blobs[q.url.slice(1)];
			let offset = 0;
			const timer = setInterval(() => {
				if (offset === body.length) {
					clearInterval(timer);
					r.end();
				} else {
					const end = Math.min(offset + 8, body.length);
					r.write(body.subarray(offset, end));
					offset = end;
				}
			}, 40);
			r.on("close", () => clearInterval(timer));
		});
	const started = Date.now(),
		r = await install(f, remote.url, false, { idle: 300 });
	assert.equal(r.status, 0, r.stderr);
	assert.ok(Date.now() - started > 600);
});
test("non-HTTPS redirects and redirect loops fail without leaking credentials", async (t) => {
	for (const to of [
		"http://127.0.0.1:1/model",
		"file:///tmp/model",
		"/model.safetensors",
	]) {
		const f = data(t),
			remote = await server(t, (_q: any, r: any) => {
				r.writeHead(302, { Location: to });
				r.end();
			});
		const r = await install(f, remote.url);
		assert.equal(r.status, 1);
		assert.ok(!r.stderr.includes(token));
		assert.ok(remote.requests.length <= 5);
		noPartial(f.destination);
		if (to.startsWith("http:")) assert.match(r.stderr, /non-HTTPS/);
	}
});
test("TLS trust failures and stalled sockets do not retry or leave partial files", async (t) => {
	const f = data(t),
		remote = await server(t, (_q: any, r: any) => {
			r.writeHead(200);
			r.flushHeaders();
		});
	const stalled = await install(f, remote.url, false, { idle: 250 });
	assert.equal(stalled.status, 1);
	assert.equal(remote.requests.length, 1);
	noPartial(f.destination);
	const bad = await install(f, remote.url, false, {
		env: { SSL_CERT_FILE: join(f.dir, "missing.pem") },
	});
	assert.equal(bad.status, 1);
	assert.equal(remote.requests.length, 1);
	noPartial(f.destination);
});
test("a trusted certificate for the wrong hostname is rejected before sending credentials", async (t) => {
	const f = data(t),
		k = join(f.dir, "wrong-key.pem"),
		c = join(f.dir, "wrong-cert.pem");
	const generated = spawnSync(
		"openssl",
		[
			"req",
			"-x509",
			"-newkey",
			"rsa:2048",
			"-nodes",
			"-keyout",
			k,
			"-out",
			c,
			"-days",
			"1",
			"-subj",
			"/CN=wrong.example",
			"-addext",
			"subjectAltName=DNS:wrong.example",
		],
		{ encoding: "utf8" },
	);
	assert.equal(generated.status, 0, generated.stderr);
	const remote = await server(t, (_q: any, r: any) => r.end("never read"), {
		key: readFileSync(k),
		cert: readFileSync(c),
	});
	const result = await install(f, remote.url, false, {
		env: { SSL_CERT_FILE: c },
	});
	assert.equal(result.status, 1);
	assert.equal(remote.requests.length, 0);
	noPartial(f.destination);
});
test("safe tar entries preserve data-filter modes and timestamps and allow internal directory aliases", async (t) => {
	const dir = root(t);
	mkdirSync(join(dir, "real"));
	symlinkSync("real", join(dir, "voices"));
	writeFileSync(
		join(dir, "voices.tgz"),
		archive([
			{ name: "voices", type: "5", mode: 0o000 },
			{ name: "voices/./NATF2.pt", data: "voice", mode: 0o7777 },
			{ name: "voices/nonexec", data: "data", mode: 0o011 },
		]),
	);
	const r = await invoke(fixture!, ["extract", dir, "voices"]);
	assert.equal(r.status, 0, r.stderr);
	assert.equal(readlinkSync(join(dir, "voices")), "real");
	assert.equal(readFileSync(join(dir, "real/NATF2.pt"), "utf8"), "voice");
	assert.equal(statSync(join(dir, "real/NATF2.pt")).mode & 0o7777, 0o755);
	assert.equal(statSync(join(dir, "real/nonexec")).mode & 0o777, 0o600);
	assert.equal(statSync(join(dir, "real")).mtimeMs, 1234567890000);
});
test("all archive members are validated before extraction, rejecting traversal, wrong roots and special/link types", async (t) => {
	const entries = [
		{ name: "../escape" },
		{ name: "voices/../../escape" },
		{ name: "/voices/absolute" },
		{ name: "other/file" },
		{ name: "voices/link", type: "2", link: "/tmp/escape" },
		{ name: "voices/hard", type: "1", link: "voices/NATF2.pt" },
		{ name: "voices/fifo", type: "6" },
	];
	for (const bad of entries) {
		const dir = root(t);
		writeFileSync(
			join(dir, "voices.tgz"),
			archive([{ name: "voices/NATF2.pt", data: "would be written" }, bad]),
		);
		const r = await invoke(fixture!, ["extract", dir, "voices"]);
		assert.equal(r.status, 1);
		assert.match(r.stderr, /Unsafe member/);
		assert.equal(existsSync(join(dir, "voices/NATF2.pt")), false);
	}
});
test("pre-existing file or directory symlinks escaping the root are rejected", async (t) => {
	for (const directory of [false, true]) {
		const dir = root(t),
			outside = root(t);
		mkdirSync(join(dir, "voices"));
		if (directory) {
			rmSync(join(dir, "voices"), { recursive: true });
			symlinkSync(outside, join(dir, "voices"));
		} else {
			writeFileSync(join(outside, "value"), "outside");
			symlinkSync(join(outside, "value"), join(dir, "voices/NATF2.pt"));
		}
		writeFileSync(
			join(dir, "voices.tgz"),
			archive([{ name: "voices/NATF2.pt", data: "bad" }]),
		);
		const r = await invoke(fixture!, ["extract", dir, "voices"]);
		assert.equal(r.status, 1);
		assert.match(r.stderr, /Unsafe member/);
		if (!directory)
			assert.equal(readFileSync(join(outside, "value"), "utf8"), "outside");
	}
});
test("data-filter confinement is applied per entry after full name/type validation", async (t) => {
	const dir = root(t),
		outside = root(t);
	mkdirSync(join(dir, "voices"));
	writeFileSync(join(outside, "value"), "outside");
	symlinkSync(join(outside, "value"), join(dir, "voices/escape"));
	writeFileSync(
		join(dir, "voices.tgz"),
		archive([
			{ name: "voices/first", data: "first" },
			{ name: "voices/escape", data: "bad" },
		]),
	);
	const r = await invoke(fixture!, ["extract", dir, "voices"]);
	assert.equal(r.status, 1);
	assert.equal(readFileSync(join(dir, "voices/first"), "utf8"), "first");
	assert.equal(readFileSync(join(outside, "value"), "utf8"), "outside");
});
test("server patch guards origin, voice allowlist, prompt length and busy state before websocket creation", async () => {
	const r = await invoke(fixture!, ["patch"], {
		input: handler + '\nint(request["seed"])\nint(request["seed"])',
	});
	assert.equal(r.status, 0, r.stderr);
	for (const value of [
		"http://127.0.0.1:8998",
		"http://localhost:8998",
		"if voice not in allowed_voices:",
		"> 8000",
		"if self.lock.locked():",
	])
		assert.ok(r.stdout.includes(value));
	assert.ok(
		r.stdout.indexOf("if self.lock.locked():") <
			r.stdout.indexOf("ws = web.WebSocketResponse()"),
	);
	assert.equal(r.stdout.split('int(request.query["seed"])').length - 1, 2);
});
test("server/root drift and duplicates fail closed without clobbering the source file", async (t) => {
	for (const source of [
		"changed",
		handler + handler,
		handler,
		handler + "\n" + webRoot + "\n" + webRoot,
	]) {
		const dir = root(t),
			file = join(dir, "server.py");
		writeFileSync(file, source);
		const r = await invoke(patcher!, [file, "/guard.js"]);
		assert.equal(r.status, 1);
		assert.equal(readFileSync(file, "utf8"), source);
		assert.match(r.stderr, /changed/);
	}
});
test("browser injection quotes Unicode, spaces, backslashes and quote characters, preserving source symlinks and modes", async (t) => {
	const dir = root(t),
		file = join(dir, "server.py"),
		link = join(dir, "link.py");
	writeFileSync(file, handler + "\n" + webRoot, { mode: 0o640 });
	symlinkSync(file, link);
	const guard = '/path with spaces/雪\\".js';
	const r = await invoke(patcher!, [link, guard]);
	assert.equal(r.status, 0, r.stderr);
	assert.equal(readlinkSync(link), file);
	assert.equal(statSync(file).mode & 0o777, 0o640);
	const out = readFileSync(file, "utf8");
	const quoted = out.match(/\+ Path\(("(?:\\.|[^"\\])*")\)\.read_text/);
	assert.ok(quoted);
	assert.equal(JSON.parse(quoted![1]), guard);
	assert.ok(out.includes('content_type="text/html"'));
});
