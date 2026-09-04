#!/usr/bin/env bats

setup() {
  export TOTP_TEST_DIR="$BATS_TEST_TMPDIR"
  export TOTP_TEST_SEED="JBSWY3DPEHPK3PXP"
  export DOTFILES_SECRETS_ENV="$TOTP_TEST_DIR/env.yaml"
  export SOPS_NIX_SECRETS_DIR="$TOTP_TEST_DIR/secrets"
  export PATH="$TOTP_TEST_DIR/bin:$PATH"
  mkdir -p "$TOTP_TEST_DIR/bin" "$SOPS_NIX_SECRETS_DIR"
  printf '%s\n' "$TOTP_TEST_SEED" > "$SOPS_NIX_SECRETS_DIR/TOTP_FIXTURE_SECRET"
  printf '{}\n' > "$DOTFILES_SECRETS_ENV"

  # Extract exactly the production helpers, decoding only their Nix escapes.
  python3 - "$BATS_TEST_DIRNAME/../home/user/packages.nix" <<'PY'
import os
from pathlib import Path
import re
import sys
import textwrap

source = Path(sys.argv[1]).read_text()
root = Path(os.environ["TOTP_TEST_DIR"])
for binding, name in (("totpFromSops", "totp-from-sops"), ("totpSopsImport", "totp-sops-import")):
    pattern = (
        rf"  {binding} = pkgs\.writeShellApplication \{{\n"
        rf".*?    text = ''\n(.*?)\n    '';\n  \}};"
    )
    matches = re.findall(pattern, source, re.S)
    assert len(matches) == 1, f"expected exactly one {binding} helper"
    (root / name).write_text(textwrap.dedent(matches[0]).replace("''${", "${") + "\n")
PY

  cat > "$TOTP_TEST_DIR/bin/fake-tool" <<'PY'
#!/usr/bin/env python3
import json
import os
from pathlib import Path
import sys

name = Path(sys.argv[0]).name
root = Path(os.environ["TOTP_TEST_DIR"])
content = sys.stdin.read()
(root / f"{name}.argv").write_text(json.dumps(sys.argv[1:]))
(root / f"{name}.stdin").write_text(content)
if name == "oathtool":
    if os.environ.get("TOTP_TEST_REJECT") == "1":
        print("invalid base32 input", file=sys.stderr)
        sys.exit(1)
    print("123456")
elif name == "jq":
    # Support the old argv interface too, so regressions fail on the leak.
    value = sys.argv[sys.argv.index("--arg") + 2] if "--arg" in sys.argv else content
    print(json.dumps(value))
PY
  chmod +x "$TOTP_TEST_DIR/bin/fake-tool"
  for tool in oathtool jq sops; do
    ln -s fake-tool "$TOTP_TEST_DIR/bin/$tool"
  done
}

assert_no_seed_in_arguments() {
  python3 - <<'PY'
import os
from pathlib import Path

root = Path(os.environ["TOTP_TEST_DIR"])
for path in root.glob("*.argv"):
    assert os.environ["TOTP_TEST_SEED"] not in path.read_text(), path.name
PY
}

@test "generation sends seed through stdin and retains digit and period options" {
  run bash "$TOTP_TEST_DIR/totp-from-sops" FIXTURE --digits 8 --period 60
  [ "$status" -eq 0 ]
  [ "$output" = "123456" ]
  assert_no_seed_in_arguments
  [ "$(cat "$TOTP_TEST_DIR/oathtool.stdin")" = "$TOTP_TEST_SEED" ]
  [ "$(cat "$TOTP_TEST_DIR/oathtool.argv")" = '["--totp", "-b", "-d", "8", "-s", "60", "-"]' ]
}

@test "import keeps seed out of oathtool jq and sops arguments" {
  run bash "$TOTP_TEST_DIR/totp-sops-import" FIXTURE "$SOPS_NIX_SECRETS_DIR/TOTP_FIXTURE_SECRET"
  [ "$status" -eq 0 ]
  [[ "$output" != *"$TOTP_TEST_SEED"* ]]
  assert_no_seed_in_arguments
  [ "$(cat "$TOTP_TEST_DIR/oathtool.stdin")" = "$TOTP_TEST_SEED" ]
  [ "$(cat "$TOTP_TEST_DIR/jq.stdin")" = "$TOTP_TEST_SEED" ]
  python3 - <<'PY'
import json
import os
from pathlib import Path

root = Path(os.environ["TOTP_TEST_DIR"])
assert json.loads((root / "sops.stdin").read_text()) == os.environ["TOTP_TEST_SEED"]
assert json.loads((root / "sops.argv").read_text()) == [
    "set", "--value-stdin", os.environ["DOTFILES_SECRETS_ENV"], '["TOTP_FIXTURE_SECRET"]'
]
PY
}

@test "rejected imports do not write secrets or disclose them" {
  run env TOTP_TEST_REJECT=1 bash "$TOTP_TEST_DIR/totp-sops-import" FIXTURE "$SOPS_NIX_SECRETS_DIR/TOTP_FIXTURE_SECRET"
  [ "$status" -eq 65 ]
  [[ "$output" == *"seed rejected by oathtool"* ]]
  [[ "$output" != *"$TOTP_TEST_SEED"* ]]
  [ ! -e "$TOTP_TEST_DIR/sops.argv" ]
  assert_no_seed_in_arguments
}

@test "generation failures remain nonzero without disclosing seeds" {
  run env TOTP_TEST_REJECT=1 bash "$TOTP_TEST_DIR/totp-from-sops" FIXTURE
  [ "$status" -ne 0 ]
  [[ "$output" != *"$TOTP_TEST_SEED"* ]]
  assert_no_seed_in_arguments
}
