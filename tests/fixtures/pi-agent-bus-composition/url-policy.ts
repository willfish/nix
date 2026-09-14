// Parsing only: never instantiate the client or fetch the effective default.
import assert from "node:assert/strict";

export default async function () {
  assert.equal(typeof Bun.version, "string");
  const { DEFAULT_URL, parseHubUrl } = await import(process.env.COMPOSITION_EXTENSION_PROTOCOL!);
  assert.equal(DEFAULT_URL, "http://terminus:7420");
  const raw = process.env.PI_AGENT_BUS_URL || DEFAULT_URL;
  const parsed = parseHubUrl(raw);
  assert.ok("ok" in parsed);
  assert.equal(new URL(parsed.ok).hostname, process.env.COMPOSITION_EXPECT_HOST);
  if (process.env.COMPOSITION_EXPECT_DISABLED === "1") {
    assert.equal(process.env.PI_AGENT_BUS_ENABLED, "0");
  } else {
    assert.notEqual(process.env.PI_AGENT_BUS_ENABLED, "0");
    assert.ok(process.env.NO_PROXY?.split(",").includes(new URL(parsed.ok).hostname));
    assert.equal(process.env.no_proxy, process.env.NO_PROXY);
  }
  console.log("COMPOSITION_URL_POLICY_PASSED");
}
