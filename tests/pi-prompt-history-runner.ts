// Run upstream node:test-style callbacks inside Pi's extension loader so imports
// use the exact bundled host API. The packaged executable does not run node:test.
// Tests in this pinned revision use only test(name, callback), no runner hooks.
const cases: { name: string; run: () => unknown }[] = [];
export default function test(name: string, run: () => unknown): void {
  cases.push({ name, run });
}
export async function run(): Promise<void> {
  let failures = 0;
  for (const entry of cases) {
    try {
      await entry.run();
      console.log(`PASS ${entry.name}`);
    } catch (error) {
      failures++;
      console.error(`FAIL ${entry.name}`, error);
    }
  }
  if (!cases.length || failures) throw new Error(`${failures}/${cases.length} history tests failed`);
  console.log(`HISTORY_TESTS_PASSED=${cases.length}`);
}
