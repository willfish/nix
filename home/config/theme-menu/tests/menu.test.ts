import assert from "node:assert/strict";
import { spawn, spawnSync } from "node:child_process";
import {
	existsSync,
	mkdirSync,
	mkdtempSync,
	readFileSync,
	writeFileSync,
	rmSync,
	symlinkSync,
	readlinkSync,
	statSync,
	readdirSync,
} from "node:fs";
import { join, basename } from "node:path";
import { tmpdir } from "node:os";
import { test } from "node:test";
const binary = process.env.THEME_MENU_BIN!,
	fixture = process.env.THEME_MENU_FIXTURE!;
assert.ok(binary && fixture, "Set THEME_MENU_BIN and THEME_MENU_FIXTURE");
const legacy = process.env.THEME_MENU_LEGACY;
const python =
	process.env.THEME_MENU_PYTHON ||
	process.env.PATH?.split(":")
		.map((p) => join(p, "python3"))
		.find((p) => existsSync(p));
const fake = `#!${process.execPath}
const fs=require('node:fs'),path=require('node:path');
const name=path.basename(process.argv[1]),args=process.argv.slice(2),table=JSON.parse(process.env.TM_COMMANDS||'{}');
const entry={name,args};
if(name==='fuzzel'){entry.input=fs.readFileSync(0,'utf8');entry.settings=fs.readFileSync(args[args.indexOf('--config')+1],'utf8');}
fs.appendFileSync(process.env.TM_LOG,JSON.stringify(entry)+'\\n');
const response=table[name+' '+args.join(' ')]??table[name]??{code:0,out:name==='gdbus'?'(false,)':''};
if(name==='nix'&&args.includes('--out-link')&&process.env.TM_WALLPAPER){const link=args[args.indexOf('--out-link')+1];try{fs.unlinkSync(link);}catch{}fs.symlinkSync(process.env.TM_WALLPAPER,link);}
if(response.writeMode)fs.writeFileSync(process.env.TM_STATE+'/mode',response.writeMode+'\\n');
setTimeout(()=>{if(response.err)process.stderr.write(response.err);if(response.out)process.stdout.write(response.out);process.exit(response.code??0);},response.delay??0);
`;
function setup(t: any) {
	const root = mkdtempSync(join(tmpdir(), "theme-menu-fixture-"));
	t.after(() => rmSync(root, { recursive: true, force: true }));
	const state = join(root, "state"),
		config = join(root, "config"),
		bin = join(root, "bin"),
		log = join(root, "commands.jsonl"),
		catalogue = join(root, "catalogue.json"),
		greeter = join(root, "greeter"),
		proc = join(root, "proc"),
		wallpaper = join(root, "wallpaper");
	for (const p of [bin, config, proc, wallpaper])
		mkdirSync(p, { recursive: true });
	writeFileSync(log, "");
	writeFileSync(join(wallpaper, "wallpaper.png"), Buffer.from([0, 1, 2, 255]));
	for (const name of [
		"fuzzel",
		"gdbus",
		"herdr",
		"notify-send",
		"nix",
		"gsettings",
		"hyprctl",
		"systemctl",
		"makoctl",
		"walker",
	])
		writeFileSync(join(bin, name), fake, { mode: 0o755 });
	const palettes: any = {};
	for (const [id, label] of [
		["tokyo-night", "Tokyo Night"],
		["rose-pine", "Rosé Pine"],
		["osaka-jade", "Osaka Jade"],
	]) {
		const source = join(root, id);
		mkdirSync(source);
		const files: any = {};
		for (const name of [
			"host-palettes.json",
			"herdr.toml",
			"btop.theme",
			"ghostty-dark",
			"ghostty-light",
			"host-dark.json",
			"host-light.json",
			"delta",
			"bat-dark",
			"bat-light",
		]) {
			const p = join(source, name);
			writeFileSync(p, `${id}:${name}\n`);
			files[name] = p;
		}
		const session: any = {};
		for (const mode of ["light", "dark"]) {
			session[mode] = {};
			for (const name of [
				"hyprland.conf",
				"gtk.css",
				"fuzzel.ini",
				"walker.css",
			]) {
				const p = join(source, mode + "-" + name);
				const content =
					name === "hyprland.conf"
						? `$theme_gtk = custom-${mode}\n$theme_font = Test Font\n$theme_mono_font = Test Mono\n$theme_font_size = 15\n$theme_active_border = rgb(abcdef)\n$theme_rounding = 9\n`
						: name === "gtk.css"
							? "/* Shared GTK and Brave colours and fonts */\n"
							: name === "fuzzel.ini"
								? "[main]\nfont=Test Font:size=14\n[border]\nwidth=3\nradius=7\n"
								: `${id}:${mode}`;
				writeFileSync(p, content);
				session[mode][name] = p;
			}
		}
		palettes[id] = {
			label,
			files,
			session,
			nvim: {
				dark: {
					base00: "#24273a",
					base02: "#494d64",
					base05: "#cad3f5",
					base07: "#f4dbd6",
					base0D: "#b7bdf8",
				},
				light: {
					base00: "#eeeeee",
					base02: "#bbbbbb",
					base05: "#222222",
					base07: "#111111",
					base0D: "#bbbb00",
				},
			},
		};
	}
	const data: any = {
		default: "tokyo-night",
		palettes,
		appearance: {
			monoFont: "Fallback Mono",
			fontSize: 13,
			borderSize: 2,
			rounding: 10,
		},
		launcher: {
			lines: 4,
			width: 45,
			anchor: "center",
			layer: "overlay",
			matchMode: "fzf",
		},
	};
	const save = () => writeFileSync(catalogue, JSON.stringify(data));
	save();
	return {
		root,
		state,
		config,
		bin,
		log,
		catalogue,
		greeter,
		proc,
		wallpaper,
		data,
		save,
	};
}
function logs(f: any) {
	return readFileSync(f.log, "utf8")
		.trim()
		.split("\n")
		.filter(Boolean)
		.map((s) => JSON.parse(s));
}
function execute(cmd: string, args: string[], env: any = {}, input?: string) {
	return new Promise<any>((resolve, reject) => {
		const p = spawn(cmd, args, {
			env: { ...process.env, ...env },
			stdio: ["pipe", "pipe", "pipe"],
		});
		let stdout = "",
			stderr = "";
		const timer = setTimeout(() => {
			p.kill("SIGKILL");
			reject(new Error("Fixture timed out"));
		}, 20000);
		p.stdout.on("data", (d) => (stdout += d));
		p.stderr.on("data", (d) => (stderr += d));
		p.on("error", reject);
		p.on("close", (status, signal) => {
			clearTimeout(timer);
			if (signal) reject(new Error(`${signal}: ${stderr}`));
			else resolve({ status, stdout, stderr });
		});
		p.stdin.end(input || "");
	});
}
function env(f: any, extra: any = {}) {
	return {
		PATH: f.bin,
		XDG_CONFIG_HOME: f.config,
		XDG_CURRENT_DESKTOP: "",
		HYPRLAND_INSTANCE_SIGNATURE: "",
		THEME_MENU_PUBLISH: undefined,
		TM_LOG: f.log,
		TM_STATE: f.state,
		TM_WALLPAPER: f.wallpaper,
		TM_COMMANDS: "{}",
		...extra,
	};
}
function args(f: any, extra: string[] = []) {
	return ["--catalogue", f.catalogue, "--state", f.state, ...extra];
}
function run(
	f: any,
	extra: string[] = ["--no-reload", "default"],
	e: any = {},
) {
	return execute(binary, args(f, extra), env(f, e));
}
function runFixture(f: any, extra: string[], e: any = {}, failure = -1) {
	return execute(
		fixture,
		["run", f.greeter, f.proc, String(failure), ...args(f, extra)],
		env(f, e),
	);
}
function active(f: any, name: string) {
	return readFileSync(join(f.state, "active", name));
}
function content(f: any) {
	const result: any = {};
	if (existsSync(f.state))
		for (const file of readdirSync(f.state, { recursive: true })) {
			if (file === "lock") continue;
			const p = join(f.state, String(file));
			if (statSync(p).isFile())
				result[String(file)] = readFileSync(p).toString("base64");
		}
	return result;
}

test("default, override, reapply, reset and byte-for-byte legacy CLI parity", async (t) => {
	const f = setup(t);
	for (const selection of ["default", "rose-pine", "--reapply", "default"]) {
		const r = await run(f, ["--no-reload", selection]);
		assert.equal(r.status, 0, r.stderr);
		assert.equal(
			existsSync(join(f.state, "selection")),
			selection !== "default",
		);
		if (selection === "--reapply")
			assert.equal(
				readFileSync(join(f.state, "selection"), "utf8"),
				"rose-pine\n",
			);
	}
	if (legacy) {
		assert.ok(python);
		const old = setup(t);
		for (const id of ["rose-pine", "light", "--reapply", "default", "dark"]) {
			assert.equal((await run(f, ["--no-reload", id])).status, 0);
			const r = await execute(
				python!,
				[legacy, ...args(old, ["--no-reload", id])],
				env(old),
			);
			assert.equal(r.status, 0, r.stderr);
			assert.deepEqual(content(f), content(old));
		}
	}
});
test("all bundle bytes and session assets apply, native mode wins and unchanged files retain inode/mode", async (t) => {
	const f = setup(t);
	mkdirSync(f.state);
	writeFileSync(join(f.state, "mode"), "light\n");
	assert.equal((await run(f, ["--no-reload", "rose-pine"])).status, 0);
	for (const [name, path] of Object.entries({
		...f.data.palettes["rose-pine"].files,
		...f.data.palettes["rose-pine"].session.light,
	}))
		assert.deepEqual(active(f, name), readFileSync(path as string));
	const path = join(f.state, "active/delta"),
		inode = statSync(path).ino;
	assert.equal((await run(f, ["--no-reload", "--reapply"])).status, 0);
	assert.equal(statSync(path).ino, inode);
	f.data.palettes["rose-pine"].nativeMode = "dark";
	f.save();
	assert.equal((await run(f, ["--no-reload", "rose-pine"])).status, 0);
	assert.equal(readFileSync(join(f.state, "mode"), "utf8"), "dark\n");
	const r = await run(f, ["--no-reload", "light"]);
	assert.equal(r.status, 1);
	assert.match(r.stderr, /dark-only/);
});
test("mode actions change only projected assets and preserve palette/application overrides", async (t) => {
	const f = setup(t);
	await run(f, ["--no-reload", "rose-pine"]);
	writeFileSync(join(f.state, "active/delta"), "manual override");
	const r = await run(f, ["light"]);
	assert.equal(r.status, 0, r.stderr);
	assert.equal(readFileSync(join(f.state, "selection"), "utf8"), "rose-pine\n");
	assert.equal(active(f, "delta").toString(), "manual override");
	assert.equal(readFileSync(join(f.state, "mode"), "utf8"), "light\n");
	assert.deepEqual(
		logs(f).map((x) => [x.name, ...x.args]),
		[["notify-send", "Appearance mode", "Light mode"]],
	);
});
test("unknown palette, absent source and invalid catalogue fail before publishing active writes", async (t) => {
	const f = setup(t);
	for (const id of ["../outside", "unknown"]) {
		const r = await run(f, ["--no-reload", id]);
		assert.equal(r.status, 1);
		assert.equal(existsSync(join(f.state, "active")), false);
	}
	rmSync(f.data.palettes["rose-pine"].files.delta);
	assert.equal((await run(f, ["--no-reload", "rose-pine"])).status, 1);
	assert.equal(existsSync(join(f.state, "active")), false);
	writeFileSync(f.catalogue, "[]");
	assert.equal((await run(f)).status, 1);
});
test("transactions roll back every write and selection, including a failed final selection commit", async (t) => {
	const f = setup(t);
	await run(f);
	const original = content(f);
	await run(f, ["--no-reload", "rose-pine"]);
	const changed = Object.keys(content(f)).filter(
		(k) => original[k] !== content(f)[k],
	).length;
	await run(f);
	for (const step of [0, 4, changed - 2, changed - 1]) {
		const r = await runFixture(f, ["--no-reload", "rose-pine"], {}, step);
		assert.equal(r.status, 1, r.stderr);
		assert.deepEqual(content(f), original);
		assert.ok(
			!readdirSync(join(f.state, "active")).some((n) => n.startsWith(".")),
		);
	}
	await run(f, ["--no-reload", "rose-pine"]);
	const light = content(f);
	const r = await runFixture(f, ["--no-reload", "light"], {}, 1);
	assert.equal(r.status, 1);
	assert.deepEqual(content(f), light);
});
test("active symlinks are replaced without modifying targets; broken state links default safely", async (t) => {
	const f = setup(t);
	mkdirSync(join(f.state, "active"), { recursive: true });
	const target = join(f.root, "target");
	writeFileSync(target, "target");
	symlinkSync(target, join(f.state, "active/delta"));
	symlinkSync(join(f.root, "missing"), join(f.state, "selection"));
	symlinkSync(join(f.root, "missing2"), join(f.state, "mode"));
	const r = await run(f, ["--no-reload", "--reapply"]);
	assert.equal(r.status, 0, r.stderr);
	assert.equal(statSync(join(f.state, "active/delta")).isFile(), true);
	assert.equal(readFileSync(target, "utf8"), "target");
	assert.equal(existsSync(join(f.state, "selection")), false);
	assert.equal(readFileSync(join(f.state, "mode"), "utf8"), "dark\n");
});
test("persisted Solarized migrates to Osaka Jade; invalid state mode falls back to dark", async (t) => {
	const f = setup(t);
	mkdirSync(f.state);
	writeFileSync(join(f.state, "selection"), "\u0085solarized\u001c\n");
	writeFileSync(join(f.state, "mode"), "invalid\n");
	const r = await run(f, ["--no-reload", "--reapply"]);
	assert.equal(r.status, 0, r.stderr);
	assert.equal(
		readFileSync(join(f.state, "selection"), "utf8"),
		"osaka-jade\n",
	);
	assert.equal(readFileSync(join(f.state, "mode"), "utf8"), "dark\n");
});
test("popup sorting, current marker, INI styles, index mapping and legacy styling parity", async (t) => {
	const f = setup(t);
	await run(f, ["--no-reload", "rose-pine"]);
	const r = await run(f, ["--no-reload"], {
		TM_COMMANDS: JSON.stringify({ fuzzel: { out: "1\n" } }),
	});
	assert.equal(r.status, 0, r.stderr);
	const menu = logs(f).find((x) => x.name === "fuzzel");
	assert.equal(
		menu.input,
		"  Switch to light mode\n  Host default (Tokyo Night)\n  Osaka Jade\n* Rosé Pine\n  Tokyo Night\n",
	);
	assert.equal(menu.args.at(-1), "3");
	assert.ok(menu.settings.includes("font=Test Font:size=14"));
	assert.ok(menu.settings.includes("width=3\nradius=7"));
	assert.equal(existsSync(join(f.state, "selection")), false);
	if (legacy) {
		const old = setup(t);
		await execute(
			python!,
			[legacy, ...args(old, ["--no-reload", "rose-pine"])],
			env(old),
		);
		const result = await execute(
			python!,
			[legacy, ...args(old, ["--no-reload"])],
			env(old, { TM_COMMANDS: JSON.stringify({ fuzzel: { out: "1\n" } }) }),
		);
		assert.equal(result.status, 0, result.stderr);
		assert.equal(
			logs(old).find((x) => x.name === "fuzzel").settings,
			menu.settings,
		);
	}
});
test("popup cancellation has no writes; errors and malformed indices are not cancellation", async (t) => {
	for (const response of [
		{ code: 1, out: "" },
		{ code: 2, out: "" },
		{ code: 0, out: "999" },
		{ code: 0, out: "-1" },
		{ code: 0, out: "bad" },
		{ code: 0, out: "0\u0000bad" },
		{ code: 2, err: "invalid option" },
	]) {
		const f = setup(t),
			r = await run(f, ["--no-reload"], {
				TM_COMMANDS: JSON.stringify({ fuzzel: response }),
			});
		assert.equal(
			r.status,
			response.code === 1 || (response.code === 2 && !response.err) ? 0 : 1,
			r.stderr,
		);
		assert.equal(existsSync(join(f.state, "active")), false);
	}
});
test("Unicode decimal indices are accepted, and a popup mode action remains explicit if mode changes", async (t) => {
	const f = setup(t);
	await run(f);
	const r = await run(f, ["--no-reload"], {
		TM_COMMANDS: JSON.stringify({ fuzzel: { out: "٠", writeMode: "light" } }),
	});
	assert.equal(r.status, 0, r.stderr);
	assert.equal(readFileSync(join(f.state, "mode"), "utf8"), "light\n");
});
test("native themes omit the mode row and choose host default at index zero", async (t) => {
	const f = setup(t);
	f.data.palettes["rose-pine"].nativeMode = "light";
	f.save();
	await run(f, ["--no-reload", "rose-pine"]);
	const r = await run(f, ["--no-reload"], {
		TM_COMMANDS: JSON.stringify({ fuzzel: { out: "0" } }),
	});
	assert.equal(r.status, 0, r.stderr);
	assert.ok(
		logs(f)
			.find((x) => x.name === "fuzzel")
			.input.startsWith("  Host default"),
	);
	assert.equal(existsSync(join(f.state, "selection")), false);
});
test("the advisory lock stays held while the popup is open", async (t) => {
	const f = setup(t);
	const first = run(f, ["--no-reload"], {
		TM_COMMANDS: JSON.stringify({ fuzzel: { code: 2, delay: 800 } }),
	});
	const deadline = Date.now() + 3000;
	while (!logs(f).length && Date.now() < deadline)
		await new Promise((r) => setTimeout(r, 10));
	assert.ok(logs(f).length);
	const second = await run(f, ["--no-reload", "rose-pine"]);
	assert.equal(second.status, 1);
	assert.match(second.stderr, /Another theme menu is open/);
	assert.equal((await first).status, 0);
});
test("unpublished wallpaper references do not build, root or copy image packages", async (t) => {
	const f = setup(t);
	f.data.palettes["tokyo-night"].session.dark["wallpaper.png"] =
		"nix-theme:tokyo-night";
	f.save();
	assert.equal((await run(f)).status, 0);
	assert.equal(existsSync(join(f.state, "active/wallpaper.png")), false);
	assert.deepEqual(logs(f), []);
});
test("published wallpaper builds/copies before commit, roots after apply and resets only on theme change", async (t) => {
	const f = setup(t);
	for (const p of Object.values(f.data.palettes) as any[])
		p.session.dark["wallpaper.png"] = "nix-theme:wallpaper-id";
	f.save();
	const e = {
		THEME_MENU_PUBLISH: "1",
		THEME_WALLPAPER_FLAKE: "/flake with spaces",
		TM_COMMANDS: JSON.stringify({ nix: { out: f.wallpaper + "\n" } }),
	};
	const r = await runFixture(f, ["default"], e);
	assert.equal(r.status, 0, r.stderr);
	assert.deepEqual(active(f, "wallpaper.png"), Buffer.from([0, 1, 2, 255]));
	assert.equal(readlinkSync(join(f.state, "wallpaper-source")), f.wallpaper);
	const calls = logs(f).filter((x) => x.name === "nix");
	assert.deepEqual(calls[0].args, [
		"build",
		"--no-link",
		"--print-out-paths",
		"/flake with spaces#theme-wallpaper-id",
	]);
	assert.ok(calls[1].args.includes("--out-link"));
	writeFileSync(join(f.state, "wallpaper-current"), "rotated.png");
	writeFileSync(join(f.state, "active/wallpaper-live.png"), "rotated");
	assert.equal((await runFixture(f, ["--reapply"], e)).status, 0);
	assert.equal(
		readFileSync(join(f.state, "wallpaper-current"), "utf8"),
		"rotated.png",
	);
	f.data.palettes["tokyo-night"].session.dark["wallpaper.png"] =
		"nix-theme:other";
	f.save();
	assert.equal((await runFixture(f, ["--reapply"], e)).status, 0);
	assert.equal(existsSync(join(f.state, "wallpaper-current")), false);
});
test("wallpaper overrides drop rotation files/root; invalid IDs or failed builds leave active selection untouched", async (t) => {
	const f = setup(t);
	await run(f);
	const before = content(f);
	for (const source of [
		"nix-theme:../escape",
		"nix-theme:bad id",
		"nix-theme:",
	]) {
		f.data.palettes["rose-pine"].session.dark["wallpaper.png"] = source;
		f.save();
		const r = await runFixture(f, ["rose-pine"], { THEME_MENU_PUBLISH: "1" });
		assert.equal(r.status, 1);
		assert.deepEqual(content(f), before);
	}
	f.data.palettes["rose-pine"].session.dark["wallpaper.png"] = "nix-theme:good";
	f.save();
	assert.equal(
		(
			await runFixture(f, ["rose-pine"], {
				THEME_MENU_PUBLISH: "1",
				TM_COMMANDS: JSON.stringify({ nix: { code: 1 } }),
			})
		).status,
		1,
	);
	assert.deepEqual(content(f), before);
	for (const name of ["wallpaper-current", "wallpaper-theme"])
		writeFileSync(join(f.state, name), "old");
	writeFileSync(join(f.state, "active/wallpaper-live.png"), "old");
	symlinkSync(f.wallpaper, join(f.state, "wallpaper-source"));
	f.data.palettes["rose-pine"].session.dark["wallpaper.png"] = join(
		f.wallpaper,
		"wallpaper.png",
	);
	f.save();
	assert.equal(
		(await runFixture(f, ["rose-pine"], { THEME_MENU_PUBLISH: "1" })).status,
		0,
	);
	for (const p of [
		"wallpaper-current",
		"wallpaper-theme",
		"wallpaper-source",
		"active/wallpaper-live.png",
	])
		assert.equal(existsSync(join(f.state, p)), false);
});
test("greeter publication overwrites/truncates only existing regular files and ignores unsafe paths", async (t) => {
	for (const kind of ["regular", "missing", "symlink", "directory", "long"]) {
		const f = setup(t);
		if (kind === "regular") writeFileSync(f.greeter, "old id much longer");
		if (kind === "symlink") {
			writeFileSync(join(f.root, "target"), "untouched");
			symlinkSync(join(f.root, "target"), f.greeter);
		}
		if (kind === "directory") mkdirSync(f.greeter);
		if (kind === "long") {
			const id = "a".repeat(64);
			f.data.palettes[id] = f.data.palettes["rose-pine"];
			f.data.default = id;
			f.save();
			writeFileSync(f.greeter, "untouched");
		}
		const r = await runFixture(f, ["default"], { THEME_MENU_PUBLISH: "1" });
		assert.equal(r.status, 0, r.stderr);
		if (kind === "regular")
			assert.equal(readFileSync(f.greeter, "utf8"), "tokyo-night\n");
		if (kind === "missing") assert.equal(existsSync(f.greeter), false);
		if (kind === "symlink")
			assert.equal(readFileSync(join(f.root, "target"), "utf8"), "untouched");
		if (kind === "long")
			assert.equal(readFileSync(f.greeter, "utf8"), "untouched");
	}
});
test("Hyprland publication preserves GTK overrides, uses per-user Waybar unit and ordered reloads", async (t) => {
	const f = setup(t);
	mkdirSync(join(f.config, "gtk-3.0"));
	mkdirSync(join(f.config, "gtk-4.0"));
	writeFileSync(join(f.root, "custom"), "custom");
	symlinkSync(join(f.root, "custom"), join(f.config, "gtk-3.0/gtk.css"));
	writeFileSync(join(f.config, "gtk-4.0/gtk.css"), "custom");
	const r = await runFixture(f, ["default"], {
		THEME_MENU_PUBLISH: "1",
		XDG_CURRENT_DESKTOP: "GNOME: HYPRLAND ; other",
	});
	assert.equal(r.status, 0, r.stderr);
	assert.match(r.stderr, /Preserved custom gtk-4.0/);
	assert.equal(readFileSync(join(f.root, "custom"), "utf8"), "custom");
	const records = logs(f);
	assert.deepEqual(
		records.filter((x) => x.name === "gsettings").map((x) => x.args.at(-1)),
		["prefer-dark", "custom-dark", "Test Font 15", "Test Mono 15"],
	);
	assert.deepEqual(
		records.filter((x) => x.name === "hyprctl").map((x) => x.args),
		[
			["keyword", "general:col.active_border", "rgb(abcdef)"],
			["keyword", "decoration:rounding", "9"],
		],
	);
	const commands = records
		.filter((x) => x.name === "systemctl")
		.map((x) => x.args);
	assert.deepEqual(commands, [
		["--user", "kill", "--signal=SIGUSR2", "waybar.service"],
		["--user", "try-restart", "walker.service"],
		["--user", "reset-failed", "hypr-wallpaper.service"],
		["--user", "restart", "hypr-wallpaper.service"],
	]);
	assert.ok(!records.some((x) => x.name === "pkill"));
});
test("desktop command failures become warnings, omapager suppresses the Mako warning, and Ghostty is not started", async (t) => {
	const f = setup(t);
	const table: any = {
		gsettings: { code: 1, err: "schema missing" },
		hyprctl: { code: 1, err: "no socket" },
		"systemctl --user kill --signal=SIGUSR2 waybar.service": {
			code: 1,
			err: "not active",
		},
		makoctl: { code: 1 },
		herdr: { code: 1 },
	};
	const r = await runFixture(f, ["default"], {
		THEME_MENU_PUBLISH: "1",
		HYPRLAND_INSTANCE_SIGNATURE: "instance",
		TM_COMMANDS: JSON.stringify(table),
	});
	assert.equal(r.status, 0, r.stderr);
	for (const message of [
		"schema missing",
		"no socket",
		"not active",
		"Herdr was not reloaded",
	])
		assert.ok(r.stderr.includes(message));
	assert.ok(!r.stderr.includes("Notifications will use"));
	assert.equal(logs(f).filter((x) => x.name === "gdbus").length, 1);
	table.gdbus = { out: "(true,)" };
	assert.equal(
		(await runFixture(f, ["--reapply"], { TM_COMMANDS: JSON.stringify(table) }))
			.status,
		0,
	);
	assert.ok(logs(f).some((x) => x.args.includes("org.gtk.Actions.Activate")));
});
test("a GTK read error stops later desktop reloads but retains the committed palette", async (t) => {
	const f = setup(t);
	mkdirSync(join(f.config, "gtk-3.0/gtk.css"), { recursive: true });
	const result = await runFixture(f, ["rose-pine"], {
		THEME_MENU_PUBLISH: "1",
		HYPRLAND_INSTANCE_SIGNATURE: "test",
	});
	assert.equal(result.status, 1);
	assert.equal(readFileSync(join(f.state, "selection"), "utf8"), "rose-pine\n");
	assert.ok(
		!logs(f).some((x) => x.name === "hyprctl" || x.name === "systemctl"),
	);
});
test("equal labels retain catalogue order and binary GTK markers remain owned", async (t) => {
	const f = setup(t);
	for (const p of Object.values(f.data.palettes) as any[]) p.label = "Same";
	f.save();
	const r = await run(f, ["--no-reload"], {
		TM_COMMANDS: JSON.stringify({ fuzzel: { out: "2" } }),
	});
	assert.equal(r.status, 0, r.stderr);
	assert.equal(
		readFileSync(join(f.state, "selection"), "utf8"),
		"tokyo-night\n",
	);
	assert.equal(
		logs(f).find((x) => x.name === "fuzzel").input,
		"  Switch to light mode\n* Host default (Same)\n  Same\n  Same\n  Same\n",
	);
	mkdirSync(join(f.config, "gtk-3.0"));
	writeFileSync(
		join(f.config, "gtk-3.0/gtk.css"),
		Buffer.from("\u0000Shared GTK and Brave colours and fonts"),
	);
	const result = await runFixture(f, ["default"], {
		THEME_MENU_PUBLISH: "1",
		HYPRLAND_INSTANCE_SIGNATURE: "test",
	});
	assert.equal(result.status, 0, result.stderr);
	assert.equal(
		readFileSync(join(f.config, "gtk-3.0/gtk.css"), "utf8"),
		"/* Shared GTK and Brave colours and fonts */\n",
	);
});
test("btop reload signals only matching processes from the owned proc fixture", async (t) => {
	const f = setup(t),
		marker = join(f.root, "signal");
	const child = spawn(
		process.execPath,
		[
			"-e",
			`process.on('SIGUSR2',()=>require('node:fs').writeFileSync(${JSON.stringify(marker)},'reloaded'));process.stdout.write('ready');setInterval(()=>{},1000);`,
		],
		{ stdio: ["ignore", "pipe", "pipe"] },
	);
	t.after(() => child.kill("SIGTERM"));
	await new Promise<void>((resolve, reject) => {
		child.stdout!.once("data", () => resolve());
		child.once("error", reject);
	});
	mkdirSync(join(f.proc, String(child.pid)));
	writeFileSync(join(f.proc, String(child.pid), "comm"), "btop\n");
	const r = await runFixture(f, ["default"], { THEME_MENU_PUBLISH: "1" });
	assert.equal(r.status, 0, r.stderr);
	assert.equal(readFileSync(marker, "utf8"), "reloaded");
	rmSync(marker);
	writeFileSync(join(f.proc, String(child.pid), "comm"), "not-btop\n");
	assert.equal(
		(await runFixture(f, ["default"], { THEME_MENU_PUBLISH: "1" })).status,
		0,
	);
	assert.equal(existsSync(marker), false);
});
test("reload deadlines warn without preventing selection and command stdin/argv never use a shell", async (t) => {
	const f = setup(t);
	const start = Date.now();
	const result = await runFixture(f, ["default"], {
		TM_COMMANDS: JSON.stringify({ gdbus: { delay: 6000 } }),
	});
	assert.equal(result.status, 0, result.stderr);
	assert.ok(Date.now() - start >= 4900);
	assert.ok(Date.now() - start < 7500);
	assert.match(result.stderr, /Reload Ghostty configuration manually/);
});
test("Python whitespace/splitlines variables and selection contrast choices are preserved", async () => {
	const r = await execute(
		fixture,
		["variables"],
		{},
		"  $theme_font = Test Font\u0085$theme_x = one\n$theme_x=two\u2028ignored=three\n\u001c $theme_last = spaced \u001c",
	);
	assert.equal(r.status, 0);
	assert.deepEqual(JSON.parse(r.stdout), {
		theme_font: "Test Font",
		theme_x: "two",
		theme_last: "spaced",
	});
	for (const [bg, paper, ink, bright, expected] of [
		["000000", "eeeeee", "ffffff", "bbbbbb", "ffffff"],
		["777777", "000000", "888888", "ffffff", "ffffff"],
		["777777", "000000", "777777", "eeeeee", "000000"],
	])
		assert.equal(
			(
				await execute(fixture, ["selection-text", bg, paper, ink, bright])
			).stdout.trim(),
			expected,
		);
});
test("all generated bundles match their sources and every native-mode picker config validates with pinned Fuzzel", {
	skip: !process.env.THEME_MENU_CATALOGUE,
}, async (t) => {
	const f = setup(t);
	f.catalogue = process.env.THEME_MENU_CATALOGUE!;
	const catalogue = JSON.parse(readFileSync(f.catalogue, "utf8"));
	let count = 0;
	for (const [id, p] of Object.entries(catalogue.palettes) as any) {
		const r = await run(f, ["--no-reload", id]);
		assert.equal(r.status, 0, `${id}: ${r.stderr}`);
		for (const [name, source] of Object.entries({
			...p.files,
			...p.session[p.nativeMode || "dark"],
		}))
			if (!(source as string).startsWith("nix-theme:"))
				assert.deepEqual(
					active(f, name),
					readFileSync(source as string),
					id + ":" + name,
				);
		for (const mode of ["dark", "light"]) {
			writeFileSync(join(f.state, "mode"), mode + "\n");
			writeFileSync(f.log, "");
			const result = await run(f, ["--no-reload"], {
				TM_COMMANDS: JSON.stringify({ fuzzel: { code: 2 } }),
			});
			assert.equal(result.status, 0, `${id}: ${result.stderr}`);
			const config = join(f.root, "picker.ini");
			writeFileSync(config, logs(f).find((x) => x.name === "fuzzel").settings);
			if (process.env.THEME_MENU_FUZZEL) {
				const check = spawnSync(
					process.env.THEME_MENU_FUZZEL,
					["--config", config, "--check-config"],
					{ encoding: "utf8" },
				);
				assert.equal(check.status, 0, `${id}/${mode}: ${check.stderr}`);
			}
			count++;
		}
	}
	console.log(
		`Validated ${Object.keys(catalogue.palettes).length} bundles and ${count} picker configurations`,
	);
});
