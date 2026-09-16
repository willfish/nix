("Credential-free source-wrapper tests; "
 "candidate artifacts are read, never run.")

import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "home/user/pi.nix"


def wrapper_text(
    *, secret_helper, pi, capture, enabled=True, bash=None, python=None
):
    ("Render only the owned shell fragment "
     "with explicitly synthetic helpers.")
    source = SOURCE.read_text().split('home.file.".local/bin/pi" = {', 1)[1]
    shell = source.split("text = ''\n", 1)[1].split("\n    '';", 1)[0]
    replacements = {
        "${pkgs.bash}/bin/bash": bash or shutil.which("bash"),
        '${if config.programs.pi-agent-bus.enable then "1" else "0"}': (
            "1" if enabled else "0"
        ),
        "${readSopsSecret}/bin/read-sops-secret": shlex.quote(
            str(secret_helper)
        ),
        "${lib.escapeShellArg config.sops.secrets.PI_AGENT_BUS_TOKEN.path}": (
            "'/synthetic/secret'"
        ),
        "${pkgs.python3}/bin/python3": shlex.quote(
            str(python or sys.executable)
        ),
        "${promptCapture}/bin/prompt-capture": shlex.quote(str(capture)),
        "${pkgs.pi-coding-agent}/bin/pi": shlex.quote(str(pi)),
        "${piThemeArgs}": (
            "--theme fixture-light --theme fixture-dark --use-theme"
            " fixture-light/fixture-dark"
        ),
    }
    for old, new in replacements.items():
        if old not in shell:
            raise AssertionError(f"wrapper interpolation disappeared: {old}")
        shell = shell.replace(old, new)
    if re.search(
        r"(?<!'')\$\{(?:pkgs|config|lib|readSopsSecret|"
        r"promptCapture|piThemeArgs)",
        shell,
    ):
        raise AssertionError("unhandled Nix interpolation")
    return textwrap.dedent(shell).replace("''${", "${") + "\n"


class WrapperTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="bus-wiring-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.secret = self.script(
            "secret",
            """#!/usr/bin/env bash
printf 'read\\n' >> "$HOME/reads"
printf 'synthetic-helper-token'
printf 'synthetic-helper-error' >&2
exit "${SECRET_STATUS:-0}"
""",
        )
        self.pi = self.script(
            "pi",
            f"""#!{sys.executable}
import json, os, sys
keys = ('PI_AGENT_BUS_URL', 'PI_AGENT_BUS_TOKEN', \
'PI_AGENT_BUS_ENABLED', 'NO_PROXY', 'no_proxy', 'CAPTURE_BRANCH', \
'PI_AGENT_BUS_OPERATOR_NOTICES', 'PI_AGENT_BUS_OPERATOR_READ', \
'PI_AGENT_BUS_OPERATOR_HISTORY')
print(json.dumps({{'args': sys.argv[1:], \
'env': {{key: os.environ.get(key) for key in keys}}}}))
""",
        )
        self.capture = self.script(
            "capture",
            """#!/usr/bin/env bash
set -euo pipefail
[ "$1" = pi ] && [ "$2" = -- ]
shift 2
export CAPTURE_BRANCH=1
exec "$@"
""",
        )

    def script(self, name, text):
        path = self.root / name
        path.write_text(text)
        path.chmod(0o700)
        return path

    def launch(self, args=(), env=None, enabled=True, parser=None):
        script = self.script(
            "wrapper",
            wrapper_text(
                secret_helper=self.secret,
                pi=self.pi,
                capture=self.capture,
                enabled=enabled,
                python=parser,
            ),
        )
        result = subprocess.run(
            [str(script), *args],
            cwd=self.root,
            env={
                "HOME": str(self.root),
                "PATH": os.environ["PATH"],
                **(env or {}),
            },
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        reads = self.root / "reads"
        count = len(reads.read_text().splitlines()) if reads.exists() else 0
        reads.unlink(missing_ok=True)
        return json.loads(result.stdout), count

    def test_operator_defaults_and_explicit_overrides(self):
        keys = (
            'PI_AGENT_BUS_OPERATOR_NOTICES',
            'PI_AGENT_BUS_OPERATOR_READ',
            'PI_AGENT_BUS_OPERATOR_HISTORY',
        )
        for capture in ('0', '1'):
            result, _ = self.launch(env={'CAPTURE_PROMPTS': capture})
            self.assertTrue(all(result['env'][key] == '1' for key in keys))
            for value in ('0', ''):
                overrides = {key: value for key in keys}
                result, _ = self.launch(env={
                    'CAPTURE_PROMPTS': capture, **overrides,
                })
                self.assertEqual(
                    {key: result['env'][key] for key in keys}, overrides
                )

    def test_normal_and_capture_load_once_before_exec_and_forward_arguments(
        self,
    ):
        for capture in ("0", "1"):
            with self.subTest(capture=capture):
                result, reads = self.launch(
                    ["--model", "qwen-fixture", "text with spaces", ""],
                    {"CAPTURE_PROMPTS": capture},
                )
                self.assertEqual(reads, 1)
                self.assertEqual(
                    result["env"]["PI_AGENT_BUS_TOKEN"],
                    "synthetic-helper-token",
                )
                self.assertEqual(
                    result["env"]["PI_AGENT_BUS_URL"], "http://terminus:7420"
                )
                self.assertEqual(
                    result["args"][-4:],
                    ["--model", "qwen-fixture", "text with spaces", ""],
                )
                self.assertEqual(
                    result["args"][:6],
                    [
                        "--theme",
                        "fixture-light",
                        "--theme",
                        "fixture-dark",
                        "--use-theme",
                        "fixture-light/fixture-dark",
                    ],
                )
                self.assertEqual(
                    result["env"]["CAPTURE_BRANCH"],
                    "1" if capture == "1" else None,
                )

    def test_explicit_overrides_including_empty_token_are_not_replaced(self):
        for capture in ("0", "1"):
            for token in ("synthetic-override", ""):
                result, reads = self.launch(
                    env={
                        "CAPTURE_PROMPTS": capture,
                        "PI_AGENT_BUS_TOKEN": token,
                        "PI_AGENT_BUS_URL": "http://bus.example:9999",
                    }
                )
                self.assertEqual(reads, 0)
                self.assertEqual(result["env"]["PI_AGENT_BUS_TOKEN"], token)
                self.assertEqual(
                    result["env"]["PI_AGENT_BUS_URL"], "http://bus.example:9999"
                )

    def test_disabled_offline_cli_and_environment_never_read_secret(self):
        cases = [(["--offline"], {}), ([], {"PI_AGENT_BUS_ENABLED": "0"})]
        cases += [
            ([], {"PI_OFFLINE": value})
            for value in ("1", "true", "YES", " true ", "yes ")
        ]
        for capture in ("0", "1"):
            for args, env in cases:
                with self.subTest(capture=capture, args=args, env=env):
                    result, reads = self.launch(
                        args, {"CAPTURE_PROMPTS": capture, **env}
                    )
                    self.assertEqual(reads, 0)
                    self.assertIsNone(result["env"]["PI_AGENT_BUS_TOKEN"])
            _, reads = self.launch(
                env={"CAPTURE_PROMPTS": capture}, enabled=False
            )
            self.assertEqual(reads, 0)

    def test_false_offline_values_remain_enabled(self):
        for value in ("0", "false", "no", "", "t rue"):
            _, reads = self.launch(env={"PI_OFFLINE": value})
            self.assertEqual(reads, 1)

    def test_missing_secret_does_not_exit_or_export_partial_helper_output(self):
        for capture in ("0", "1"):
            result, reads = self.launch(
                env={"CAPTURE_PROMPTS": capture, "SECRET_STATUS": "1"}
            )
            self.assertEqual(reads, 1)
            self.assertIsNone(result["env"]["PI_AGENT_BUS_TOKEN"])

    def test_capture_bypasses_only_hostname_and_preserves_both_proxy_overrides(
        self,
    ):
        for url, host in (
            ("http://terminus:7420", "terminus"),
            ("http://bus.example:7420/base", "bus.example"),
        ):
            result, _ = self.launch(
                env={
                    "CAPTURE_PROMPTS": "1",
                    "PI_AGENT_BUS_URL": url,
                    "NO_PROXY": "upper.example",
                    "no_proxy": "lower.example",
                }
            )
            expected = f"upper.example,lower.example,{host}"
            self.assertEqual(result["env"]["NO_PROXY"], expected)
            self.assertEqual(result["env"]["no_proxy"], expected)
        result, _ = self.launch(env={"NO_PROXY": "upper", "no_proxy": "lower"})
        self.assertEqual(result["env"]["NO_PROXY"], "upper")
        self.assertEqual(result["env"]["no_proxy"], "lower")

    def test_capture_parser_failure_disables_bus_without_aborting_pi(self):
        parser = self.script("failed-parser", "#!/usr/bin/env bash\nexit 1\n")
        result, _ = self.launch(env={"CAPTURE_PROMPTS": "1"}, parser=parser)
        self.assertEqual(result["env"]["PI_AGENT_BUS_ENABLED"], "0")
        self.assertIsNone(result["env"]["NO_PROXY"])
        self.assertEqual(result["env"]["CAPTURE_BRANCH"], "1")

    def test_empty_url_uses_client_default_for_bypass_without_changing_override(
        self,
    ):
        result, _ = self.launch(
            env={"CAPTURE_PROMPTS": "1", "PI_AGENT_BUS_URL": ""}
        )
        self.assertEqual(result["env"]["PI_AGENT_BUS_URL"], "")
        self.assertEqual(result["env"]["NO_PROXY"], "terminus")
        self.assertIsNone(result["env"]["PI_AGENT_BUS_ENABLED"])

    def test_ambiguous_urls_disable_only_capture_participation(self):
        for url in (
            "http://[bad",
            "http://bad,host:7420",
            "http://bücher.example:7420",
            "http://127.2:7420",
            "http://2130706434:7420",
            "http://0177.0.0.2:7420",
            "http://0x7f000002:7420",
            "http://bus.0xabc:7420",
            "http://[::1]:7420",
            "http://user@bus.example:7420",
            "http://bus.example\\\\@127.2:7420",
        ):
            for capture in ("0", "1"):
                with self.subTest(url=url, capture=capture):
                    result, _ = self.launch(
                        env={
                            "CAPTURE_PROMPTS": capture,
                            "PI_AGENT_BUS_URL": url,
                        }
                    )
                    self.assertEqual(result["env"]["PI_AGENT_BUS_URL"], url)
                    self.assertIsNone(result["env"]["NO_PROXY"])
                    self.assertEqual(
                        result["env"]["PI_AGENT_BUS_ENABLED"],
                        "0" if capture == "1" else None,
                    )


class WiringTests(unittest.TestCase):
    def test_runbook_uses_real_newline_formats(self):
        source = (ROOT / "docs/nixos-host-operations.md").read_text()
        self.assertIn("--write-out '%{http_code}\\n'", source)
        self.assertIn("printf 'Authorization: Bearer %s\\n'", source)
        self.assertNotIn("--write-out '%{http_code}\\\\n'", source)
        self.assertNotIn("printf 'Authorization: Bearer %s\\\\n'", source)

    def test_single_external_module_installation_and_no_global_token(self):
        source = SOURCE.read_text()
        self.assertIn(
            "programs.pi-agent-bus.enable = lib.mkDefault true;", source
        )
        self.assertEqual(source.count('home.file.".local/bin/pi"'), 1)
        self.assertNotIn('home.file.".pi/agent/extensions/agent-bus"', source)
        self.assertNotIn("home.sessionVariables", source)
        self.assertNotIn("pi install", source)
        self.assertEqual(source.count("${pkgs.pi-coding-agent}/bin/pi"), 2)
        self.assertNotIn("PI_CODING_AGENT_DIR=", source)

    def test_terminus_service_uses_private_path_and_conditional_secret_ordering(
        self,
    ):
        source = (ROOT / "system/terminus/configuration.nix").read_text()
        service = source.split("services.pi-agent-bus = {", 1)[1].split(
            "};", 1
        )[0]
        for declaration in (
            "enable = true;",
            'listenAddress = "0.0.0.0";',
            "port = 7420;",
            "tokenFile = config.sops.secrets.PI_AGENT_BUS_TOKEN.path;",
        ):
            self.assertIn(declaration, service)
        self.assertIn(
            "systemd.services.pi-agent-bus = lib.mkIf"
            " config.sops.useSystemdActivation",
            source,
        )
        self.assertIn('after = [ "sops-install-secrets.service" ];', source)
        self.assertIn('requires = [ "sops-install-secrets.service" ];', source)
        self.assertNotIn("openFirewall", service)
        # Effective firewall isolation still requires Nix evaluation and
        # a LAN probe.
        self.assertNotRegex(source, r"allowedTCPPorts\s*=\s*\[[^\]]*7420")

    def test_qwen_keeps_raw_pi_profile_and_exact_explicit_extension_list(self):
        source = (ROOT / "home/user/local-llm.nix").read_text()
        qwen = source.split('name = "qwen-pi";', 1)[1].split(
            "\n  toolsPython", 1
        )[0]
        self.assertIn("exec ${pkgs.pi-coding-agent}/bin/pi", qwen)
        self.assertIn(
            "export PI_CODING_AGENT_DIR=${lib.escapeShellArg piAgentDir}", qwen
        )
        for flag in (
            "--offline",
            "--no-context-files",
            "--no-skills",
            "--no-extensions",
            "--no-prompt-templates",
            "--no-themes",
        ):
            self.assertIn(flag, qwen)
        extensions = re.findall(r"--extension ([^\s\"}]+(?:\}[^\s\"]*)?)", qwen)
        self.assertEqual(
            extensions,
            [
                "${../config/local-llm/pi-qwen.ts}",
                *[
                    "${config.home.homeDirectory}/.pi/agent/extensions/" + name
                    for name in (
                        "mcp/index.ts",
                        "todo.ts",
                        "question.ts",
                        "herdr-agent-state.ts",
                        "herdr-ui.ts",
                        "herdr-model.ts",
                        "prompt-history/index.ts",
                        "pi-voice.ts",
                    )
                ],
            ],
        )
        self.assertNotIn("agent-bus", qwen)
        self.assertNotIn("switchboard", qwen)
        self.assertIn("${piThemeArgs}", qwen)
        self.assertIn('"$@"', qwen)


# The existing behavioral gate supplies this immutable generation. Do not
# evaluate or build a fallback, and never execute its credential-loading
# home-files wrapper.
if os.environ.get("MCP_TEST_HOME"):

    class CandidateWiringTests(unittest.TestCase):
        def test_candidate_external_artifact_and_selected_raw_pi(self):
            home = Path(os.environ["MCP_TEST_HOME"]).resolve(strict=True)
            self.assertTrue(str(home).startswith("/nix/store/"))
            files = home / "home-files"
            extension = (files / ".pi/agent/extensions/agent-bus").resolve(
                strict=True
            )
            self.assertTrue(str(extension).startswith("/nix/store/"))
            manifest = json.loads((extension / "package.json").read_text())
            self.assertEqual(manifest["name"], "pi-switchboard")
            self.assertEqual(manifest["pi"]["extensions"], ["./index.ts"])
            self.assertTrue((extension / "index.ts").is_file())
            raw_package = (
                (home / "home-path/bin/pi").resolve(strict=True).parents[1]
            )
            raw = raw_package / "libexec/pi/pi"
            with raw.open("rb") as handle:
                self.assertIn(
                    handle.read(4).hex(),
                    ("7f454c46", "cffaedfe", "feedfacf", "cafebabe"),
                )
            wrapper = (files / ".local/bin/pi").read_text()
            self.assertEqual(wrapper.count(str(raw_package / "bin/pi")), 2)
            self.assertIn("read-sops-secret", wrapper)
            self.assertIn("PI_AGENT_BUS_TOKEN", wrapper)
            self.assertNotIn("pi install", wrapper)


if __name__ == "__main__":
    unittest.main()
