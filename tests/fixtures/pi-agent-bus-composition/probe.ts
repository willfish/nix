// Synthetic HTTP only. Run inside the selected compiled Pi/Bun artifact.
import assert from "node:assert/strict";

export default async function () {
  assert.equal(typeof Bun.version, "string");
  const bus = process.env.PI_AGENT_BUS_URL!;
  const token = process.env.PI_AGENT_BUS_TOKEN!;
  for (const [method, path] of [["PUT", "/v1/agents/synthetic"], ["POST", "/v1/messages"]]) {
    const response = await fetch(bus + path, {
      method,
      headers: { authorization: `Bearer ${token}`, "content-type": "application/json" },
      body: JSON.stringify({ marker: "synthetic-bus-private-body" }),
      signal: AbortSignal.timeout(3000),
    });
    assert.equal(response.status, 200);
    assert.equal(await response.text(), "synthetic-direct-bus");
  }
  const provider = await fetch(process.env.COMPOSITION_PROVIDER_URL!, {
    signal: AbortSignal.timeout(3000),
  });
  assert.equal(await provider.text(), "synthetic-provider-response");
  console.log("COMPOSITION_BUN_CAPTURE_PASSED");
}
