// Language adapter for retained Python production APIs. All cases and assertions
// live in TypeScript; this process only invokes targets and serializes outcomes.
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('../../', import.meta.url));
const adapter = String.raw`
import asyncio, json, os, runpy, sys, types
from pathlib import Path
import urllib.request
sys.path.insert(0, str(Path(sys.argv[1]) / "home/config/voice"))
import personaplex_models as models
import personaplex_patch
request = json.load(sys.stdin)
op, args = request["operation"], request["args"]
token_reads = []
def no_credentials():
    token_reads.append(True)
    raise RuntimeError("credential access attempted")
models.token = no_credentials
try:
    if op == "matches":
        result = models.matches(Path(args["path"]), args["size"], args["digest"])
    elif op == "redirect":
        original = urllib.request.Request(args["url"], headers=args.get("headers", {}))
        redirected = models.PrivateRedirect().redirect_request(original, None, 302, "", {}, args["to"])
        result = {"authorization": redirected.get_header("Authorization"), "url": redirected.full_url}
    elif op == "install":
        if "assets" in args:
            models.ASSETS = args["assets"]
        result = models.install(Path(args["root"]), check_only=args.get("check_only", False))
    elif op == "patch":
        result = personaplex_patch.patch(args["source"], args.get("guard"))
        if args.get("compile"):
            compile("class State:\n" + result, "patched-server", "exec")
    elif op == "guards":
        calls = []
        class Denied(Exception):
            def __init__(self, *, text):
                super().__init__(text)
        web = types.SimpleNamespace(HTTPForbidden=Denied, HTTPBadRequest=Denied,
            HTTPServiceUnavailable=Denied, WebSocketResponse=lambda: calls.append(True))
        scope = {"web": web}
        exec("class State:\n" + personaplex_patch.patch(args["source"]), scope)
        state = scope["State"]()
        result = []
        for item in args["requests"]:
            state.lock = types.SimpleNamespace(locked=lambda: item["busy"])
            target = types.SimpleNamespace(headers={"Origin": item["origin"]},
                query={"voice_prompt": item["voice"], "text_prompt": item["prompt"]})
            before = len(calls)
            try:
                asyncio.run(state.handle_chat(target))
                result.append({"denied": False, "websockets": len(calls) - before})
            except Denied:
                result.append({"denied": True, "websockets": len(calls) - before})
    elif op == "locate":
        gate = runpy.run_path(str(Path(sys.argv[1]) / "scripts/check-behavior.py"))
        found = gate["native_voice_fixture"](Path(args["launcher"]))
        result = str(found) if found is not None else None
    elif op == "configure":
        gate = runpy.run_path(str(Path(sys.argv[1]) / "scripts/check-behavior.py"))
        function = gate["configure"]
        Path.resolve = lambda self, **kwargs: Path(args["home"])
        Path.is_file = lambda self: True
        function.__globals__["native_voice_fixture"] = lambda _: Path(args["fixture"])
        os.environ.clear()
        function(Path(args["home"]))
        result = {key: os.environ[key] for key in ("PI_THEME_TEST_BIN", "PI_VOICE_CONTROLLER_FIXTURE")}
    else:
        raise ValueError("unknown target operation")
    print(json.dumps({"value": result, "token_reads": len(token_reads)}))
except Exception as error:
    print(json.dumps({"error": str(error), "type": type(error).__name__, "token_reads": len(token_reads)}))
`;

export function invoke(operation, args = {}) {
  const child = spawnSync('python3', ['-c', adapter, root], {
    input: JSON.stringify({ operation, args }), encoding: 'utf8', timeout: 10000,
    env: { PATH: process.env.PATH, HOME: '/nonexistent', LANG: 'C.UTF-8', PYTHONDONTWRITEBYTECODE: '1' },
  });
  if (child.error) throw child.error;
  if (child.status !== 0) throw new Error(`Python target failed: ${child.stderr}`);
  return JSON.parse(child.stdout);
}
