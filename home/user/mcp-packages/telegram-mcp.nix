{
  lib,
  bash,
  stdenvNoCC,
  python3,
  fetchPypi,
  source,
}:
let
  # flake.lock owns the revision, so the version follows the locked commit date.
  d = source.lastModifiedDate;
  version = "0-unstable-${builtins.substring 0 4 d}-${builtins.substring 4 2 d}-${builtins.substring 6 2 d}";
  interpreter = python3.withPackages (ps: [
    # Upstream pyproject dependencies, plus the mcp[cli] extras
    # (typer, python-dotenv) that nixpkgs' mcp keeps optional.
    ps."python-dotenv"
    ps."python-json-logger"
    ps.httpx
    ps.mcp
    ps.pillow
    ps.pypdf
    ps.qrcode
    # nixpkgs' 1.42 predates the layer-229 button types used by this server.
    # Match upstream's uv.lock, including its source archive hash.
    (ps.telethon.overridePythonAttrs {
      version = "1.45.0";
      src = fetchPypi {
        pname = "telethon";
        version = "1.45.0";
        hash = "sha256-TLeuJpuKudrZ/EkLhZ8LwviFEh6ZD1sgQHsfqg2yodk=";
      };
      # The sdist ships setup.py and generated types, but no Hatch hook or tests.
      pyproject = null;
      format = "setuptools";
      doCheck = false;
      pythonImportsCheck = [ "telethon" ];
    })
    ps.typer
  ]);
in
stdenvNoCC.mkDerivation {
  pname = "telegram-mcp";
  inherit version;

  # Never from PyPI: the `telegram-mcp` name there is squatted by an
  # unrelated project and upstream warns credentials passed to it are
  # exposed. Use the locked GitHub source instead.
  src = source;

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

  # Import every registered tool without credentials or a network connection.
  doInstallCheck = true;
  installCheckPhase = ''
    runHook preInstallCheck
    cd "$out/share/telegram-mcp"
    TELEGRAM_API_ID=1 TELEGRAM_API_HASH=build-check \
      TELEGRAM_SESSION_NAME="$TMPDIR/session" TELEGRAM_LOG_FILE="$TMPDIR/mcp_errors.log" \
      ${interpreter}/bin/python3 -c 'import telegram_mcp.runner'
    runHook postInstallCheck
  '';

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
