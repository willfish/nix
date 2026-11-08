import { afterEach, expect, test } from "bun:test";
import { posixHost, type StartOptions } from "./remote-host.ts";
import type { SshConnection } from "./ssh.ts";

const original = process.env["OVIS_SSH_RUNTIME"];
afterEach(() => {
  if (original === undefined) delete process.env["OVIS_SSH_RUNTIME"];
  else process.env["OVIS_SSH_RUNTIME"] = original;
});

function connection(socket: string, registered = true) {
  const commands: string[] = [];
  return {
    commands,
    ssh: {
      async run(command: string) {
        commands.push(command);
        if (commands.length === 2) return socket;
        return ["Linux", "x86_64", "/home/operator", "/home/operator/.config", "/nix/store/herdr/bin/herdr",
          ...(registered ? [JSON.stringify({ socket_path: socket, managed_remote: false })] : [])].join("\n");
      },
    } as unknown as SshConnection,
  };
}

test("selects the registered desktop-app bridge without installing a TUI runtime", async () => {
  process.env["OVIS_SSH_RUNTIME"] = "herdr-core";
  const socket = "/home/operator/.local/state/ovis/herdr-core.sock";
  const fixture = connection(socket);
  const found = await posixHost.inspect(fixture.ssh);
  expect(found.expectedSocket).toBe(socket);
  expect(found.descriptors).toHaveLength(1);
  expect(fixture.commands[1]).toContain(socket);
  expect(fixture.commands[1]).toContain("${socket##*/}");
});

test("refuses a missing desktop-app bridge rather than offering a TUI install", async () => {
  process.env["OVIS_SSH_RUNTIME"] = "herdr-core";
  const fixture = connection("/home/operator/.local/state/ovis/herdr-core.sock", false);
  await expect(posixHost.inspect(fixture.ssh)).rejects.toThrow("automatic bridge installation is disabled");
});

test("preserves the ordinary TUI socket when the override is absent", async () => {
  delete process.env["OVIS_SSH_RUNTIME"];
  const socket = "/home/operator/.config/herdr/herdr.sock";
  const fixture = connection(socket, false);
  expect((await posixHost.inspect(fixture.ssh)).expectedSocket).toBe(socket);
  expect(fixture.commands[1]).toContain(socket);
});

test("never downloads or starts a TUI bundle when a desktop bridge disappears", async () => {
  process.env["OVIS_SSH_RUNTIME"] = "herdr-core";
  const fixture = connection("/home/operator/.local/state/ovis/herdr-core.sock", false);
  await expect(posixHost.installBundle(fixture.ssh, "linux-x64", new AbortController().signal)).rejects.toThrow("TUI bundle installation is disabled");
  await expect(posixHost.start(fixture.ssh, {} as StartOptions)).rejects.toThrow("TUI startup is disabled");
  expect(fixture.commands).toHaveLength(0);
});
