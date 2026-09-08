{
  lib,
  bash,
  fetchFromGitHub,
  stdenvNoCC,
  python3,
}:
let
  version = "3.2.33";
  interpreter = python3.withPackages (ps: [
    # Upstream pyproject dependencies, plus the mcp[cli] extras
    # (typer, python-dotenv) that nixpkgs' mcp keeps optional.
    ps."python-dotenv"
    ps."python-json-logger"
    ps.httpx
    ps.mcp
    ps.pillow
    ps.qrcode
    ps.telethon
    ps.typer
  ]);
in
stdenvNoCC.mkDerivation {
  pname = "telegram-mcp";
  inherit version;

  # Never from PyPI: the `telegram-mcp` name there is squatted by an
  # unrelated project and upstream warns credentials passed to it are
  # exposed. Pin the exact tag instead.
  src = fetchFromGitHub {
    owner = "chigwell";
    repo = "telegram-mcp";
    tag = "v${version}";
    hash = "sha256-J04iB8YHVb4I+PGZhuHRlLByMDssj6/X/tjETfCCkUw=";
  };

  dontConfigure = true;
  dontBuild = true;

  # Runtime logs belong beside the private session, not in the Nix store.
  postPatch = ''
    substituteInPlace telegram_mcp/runtime.py \
      --replace-fail 'log_file_path = os.path.join(script_dir, "mcp_errors.log")' \
      'log_file_path = os.getenv("TELEGRAM_LOG_FILE", os.path.join(script_dir, "mcp_errors.log"))'
  '';

  # The withPackages interpreter, for companion scripts (login helper).
  passthru.interpreter = interpreter;

  # Upstream's install guard refuses to start unless the distribution
  # metadata proves an explicit source-checkout install (a defense against
  # the PyPI squat). A hash-pinned source tree satisfies that contract:
  # no package metadata is installed, so the guard passes, and there is no
  # PyPI path at all.
  installPhase = ''
    runHook preInstall

    src_root="$out/share/telegram-mcp"
    install -d -m 0755 "$out/bin" "$src_root"
    cp -r --no-preserve=ownership . "$src_root"/

    printf '%s\n' \
      "#!${bash}/bin/bash" \
      "exec ${interpreter}/bin/python3 $src_root/main.py \"\$@\"" \
      > "$out/bin/telegram-mcp"
    chmod 0755 "$out/bin/telegram-mcp"

    runHook postInstall
  '';

  meta = {
    description = "Full user-account Telegram MCP server (chigwell/telegram-mcp)";
    homepage = "https://github.com/chigwell/telegram-mcp";
    license = lib.licenses.asl20;
    platforms = lib.platforms.unix;
    mainProgram = "telegram-mcp";
  };
}
