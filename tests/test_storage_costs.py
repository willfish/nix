"""Validate storage report selection without switching or building anything."""

import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[1]


class StorageCostsTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="storage-cost-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.flake = self.root / "selected flake"
        self.flake.mkdir()
        self.home = self.root / "home"
        script = self.home / ".dotfiles/scripts/nix-storage-costs"
        script.parent.mkdir(parents=True)
        shutil.copyfile(ROOT / "scripts/nix-storage-costs", script)
        script.chmod(0o755)
        self.script = script
        self.env = os.environ.copy()
        for key in list(self.env):
            if key.startswith("NH_"):
                del self.env[key]
        self.env.update(
            HOME=str(self.home),
            XDG_STATE_HOME=str(self.root / "state"),
            PATH=str(self.bin) + os.pathsep + self.env["PATH"],
            STORAGE_CALLS=str(self.root / "calls.jsonl"),
        )
        self.tool("uname", 'printf "%s\\n" "${STORAGE_PLATFORM:-Linux}"')
        self.tool("hostname", 'printf "%s\\n" "${STORAGE_HOST:-andromeda}"')
        self.tool("whoami", 'printf "william\\n"')
        self.tool("nh", '''
            python3 -c 'import json,os,sys; open(os.environ["STORAGE_CALLS"],"a").write(json.dumps({"tool":"nh","args":sys.argv[1:]})+"\\n")' "$@"
            exit "${NH_FIXTURE_STATUS:-0}"
        ''')
        self.tool("nix", '''
            python3 - "$@" <<'PY'
import json, os, sys
args = sys.argv[1:]
if os.environ.get("STORAGE_NIX_STATUS"):
    raise SystemExit(int(os.environ["STORAGE_NIX_STATUS"]))
with open(os.environ["STORAGE_CALLS"], "a") as log:
    log.write(json.dumps({"tool": "nix", "args": args, "flake": os.environ.get("NIX_STORAGE_COST_FLAKE"), "scope": os.environ.get("NIX_STORAGE_COST_SCOPE")}) + "\\n")
if args[0] == "build":
    print("/nix/store/fixture-home" if any("homeConfigurations" in a for a in args) else "/nix/store/fixture-system")
elif args[0] == "eval":
    print("[]" if "--json" in args else os.environ.get("STORAGE_SELECTED_HOME", "william-linux"))
elif args[0] == "path-info":
    print(json.dumps({a: {"narSize": 5, "closureSize": 7} for a in args if a.startswith("/nix/store/")}))
else:
    raise SystemExit("unexpected nix invocation")
PY
        ''')

    def tool(self, name, body):
        path = self.bin / name
        path.write_text("#!/usr/bin/env bash\nset -eu\n" + textwrap.dedent(body))
        path.chmod(0o755)

    def calls(self, tool="nix"):
        path = self.root / "calls.jsonl"
        if not path.exists():
            return []
        return [item for line in path.read_text().splitlines()
                if (item := json.loads(line))["tool"] == tool]

    def run_report(self, *args):
        return subprocess.run(["bash", str(self.script), *args], cwd=self.root,
                              env=self.env, text=True, capture_output=True, timeout=15)

    def builds(self):
        return [call for call in self.calls() if call["args"][0] == "build"]

    def assert_one_build(self, fragment):
        builds = self.builds()
        self.assertEqual(len(builds), 1, builds)
        self.assertIn(str(self.flake) + fragment, builds[0]["args"])

    def test_home_scope_never_builds_nixos(self):
        result = self.run_report("--scope", "home", "--flake", str(self.flake),
                                 "--home", "william-darwin")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_one_build('#homeConfigurations."william-darwin".activationPackage')
        self.assertNotIn("NixOS system:", next((self.root / "state").rglob("*.md")).read_text())

    def test_system_scope_never_builds_home(self):
        result = self.run_report("--scope", "system", "--flake", str(self.flake),
                                 "--host", "foundation")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_one_build('#nixosConfigurations."foundation".config.system.build.toplevel')

    def test_darwin_direct_report_defaults_to_home_only(self):
        self.env.update(STORAGE_PLATFORM="Darwin", STORAGE_HOST="relay", STORAGE_SELECTED_HOME="william-darwin")
        result = self.run_report("--flake", str(self.flake))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_one_build('#homeConfigurations."william-darwin".activationPackage')

    def test_nh_home_flags_select_flake_and_configuration(self):
        result = self.run_report("--after-nh-switch", "home", "switch", "--diff", "always",
                                 "--configuration=william-darwin", str(self.flake), "--cores", "2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_one_build('#homeConfigurations."william-darwin".activationPackage')

    def test_nh_system_hostname_flag_and_environment_flake(self):
        self.env["NH_OS_FLAKE"] = str(self.flake)
        result = self.run_report("--after-nh-switch", "os", "switch", "--hostname=foundation")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_one_build('#nixosConfigurations."foundation".config.system.build.toplevel')

    def test_nh_home_environment_flake_has_precedence(self):
        self.env.update(NH_HOME_FLAKE=str(self.flake), NH_FLAKE="/wrong/flake")
        result = self.run_report("--after-nh-switch", "home", "switch", "-c", "william-linux")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_one_build('#homeConfigurations."william-linux".activationPackage')

    def test_dry_and_help_callbacks_never_call_nix(self):
        for flag in ("--dry", "-n", "--help", "-h"):
            with self.subTest(flag=flag):
                result = self.run_report("--after-nh-switch", "os", "switch", str(self.flake), flag)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.calls(), [])

    def test_ambiguous_nonflake_callback_is_skipped(self):
        result = self.run_report("--after-nh-switch", "home", "switch", "--file", "/unrelated/home.nix")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls(), [])

    def test_store_result_symlink_callback_is_skipped_after_resolution(self):
        output = self.root / "realized-output"
        output.mkdir()
        result_link = self.root / "result"
        result_link.symlink_to(output, target_is_directory=True)
        # Model a Nix store target without writing into the real store.
        self.tool("realpath", 'printf "/nix/store/fixture-system\\n"')

        result = self.run_report("--after-nh-switch", "os", "switch", str(result_link))

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Storage-cost callback skipped", result.stderr)
        self.assertEqual(self.calls(), [])

    def test_direct_linux_report_keeps_both_scopes(self):
        result = self.run_report("--flake", str(self.flake), "--home", "william-linux")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(self.builds()), 2)
        self.assertTrue(all(call["flake"] == str(self.flake) for call in self.builds()))

    def test_missing_flake_environment_skips_callback(self):
        result = self.run_report("--after-nh-switch", "home", "switch", "-c", "william-linux")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls(), [])

    def test_input_override_callback_is_not_rebuilt_without_its_override(self):
        result = self.run_report("--after-nh-switch", "home", "switch", str(self.flake),
                                 "-c", "william-linux", "--override-input", "nixpkgs", "/other/flake")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls(), [])

    def test_empty_nix_argument_separator_is_allowed(self):
        result = self.run_report("--after-nh-switch", "home", "switch", str(self.flake),
                                 "-c", "william-linux", "--")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_one_build('#homeConfigurations."william-linux".activationPackage')

    def test_extra_nix_arguments_are_never_reinterpreted_as_a_flake(self):
        result = self.run_report("--after-nh-switch", "home", "switch", str(self.flake),
                                 "-c", "william-linux", "--", "--override-input", "name", "/other")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls(), [])

    def test_inventory_does_not_evaluate_the_unselected_configuration(self):
        source = (ROOT / "scripts/nix-storage-costs").read_text()
        expressions = re.findall(r"--expr '(.*?)'", source, re.S)
        self.assertEqual(len(expressions), 2)
        inventory = expressions[1].replace(
            'builtins.getFlake (builtins.getEnv "NIX_STORAGE_COST_FLAKE")',
            "fixture",
        )
        library = '''rec {
          optionals = condition: values: if condition then values else [];
          concatStringsSep = builtins.concatStringsSep;
          attrByPath = path: fallback: attrs:
            if path == [] then attrs
            else if builtins.hasAttr (builtins.head path) attrs
            then attrByPath (builtins.tail path) fallback attrs.${builtins.head path}
            else fallback;
        }'''
        home = '''{ fixture.config = {
          home.packages = [];
          programs = {
            git.package = null; fish.package = null; tmux.package = null;
            zoxide.package = null; direnv.package = null;
          };
        }; }'''
        system = '''{ fixture.config = {
          environment = { systemPackages = []; shells = []; };
          users.users.william.packages = [];
          users.defaultUserShell = null;
          programs.fish.package = null;
          virtualisation.docker.package = null;
          boot.kernelPackages.kernel = null;
          fonts.packages = [];
        }; }'''
        for scope in ("home", "system"):
            with self.subTest(scope=scope):
                home_config = home if scope == "home" else 'throw "evaluated unselected home"'
                system_config = system if scope == "system" else 'throw "evaluated unselected system"'
                expression = (
                    "let fixture = { inputs.nixpkgs.lib = " + library + ";"
                    " homeConfigurations = " + home_config + ";"
                    " nixosConfigurations = " + system_config + "; }; in " + inventory
                )
                env = self.env | {
                    "NIX_STORAGE_COST_SCOPE": scope,
                    "NIX_STORAGE_COST_HOST": "fixture",
                    "NIX_STORAGE_COST_HOME": "fixture",
                }
                result = subprocess.run(
                    ["nix-instantiate", "--eval", "--strict", "--json", "--expr", expression],
                    env=env, text=True, capture_output=True, timeout=15,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(json.loads(result.stdout), [])

    def shell_code(self, shell):
        source = (ROOT / "home/user/shells.nix").read_text()
        if shell == "bash":
            matches = re.findall(r"initExtra = ''\n(.*?)\n    '';", source, re.S)
            self.assertEqual(len(matches), 1)
            return textwrap.dedent(matches[0]).replace("''${", "${")
        functions = []
        for name in ("__storage_costs_after_nh_switch", "nh"):
            matches = re.findall(r"      " + name + r" = ''\n(.*?)\n      '';", source, re.S)
            self.assertEqual(len(matches), 1)
            functions.append("function " + name + "\n" + textwrap.dedent(matches[0]) + "\nend\n")
        return "".join(functions)

    def run_shell(self, shell, args):
        (self.root / "calls.jsonl").unlink(missing_ok=True)
        source = self.root / ("source." + shell)
        source.write_text(self.shell_code(shell))
        command = 'source "$1"; shift; nh "$@"' if shell == "bash" else 'source $argv[1]; nh $argv[2..-1]'
        cmd = [shell, "--noprofile", "--norc"] if shell == "bash" else [shell, "--no-config"]
        cmd += ["-c", command]
        if shell == "bash":
            cmd += ["fixture"]
        return subprocess.run(cmd + [str(source), *args], cwd=self.root,
                              env=self.env, text=True, capture_output=True, timeout=15)

    def test_bash_and_fish_preserve_nh_arguments_and_report_selected_flake(self):
        args = ["home", "switch", "--diff", "always", str(self.flake), "-c", "william-darwin"]
        for shell in ("bash", "fish"):
            with self.subTest(shell=shell):
                result = self.run_shell(shell, args)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.calls("nh")[0]["args"], args)
                self.assert_one_build('#homeConfigurations."william-darwin".activationPackage')

    def test_failed_nh_status_is_preserved_without_reporting(self):
        self.env["NH_FIXTURE_STATUS"] = "7"
        for shell in ("bash", "fish"):
            with self.subTest(shell=shell):
                result = self.run_shell(shell, ["home", "switch", str(self.flake)])
                self.assertEqual(result.returncode, 7, result.stderr)
                self.assertEqual(self.calls(), [])

    def test_report_failure_does_not_change_successful_nh_status(self):
        self.env["STORAGE_NIX_STATUS"] = "19"
        for shell in ("bash", "fish"):
            with self.subTest(shell=shell):
                result = self.run_shell(shell, ["home", "switch", str(self.flake), "-c", "william-linux"])
                self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
