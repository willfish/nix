{ pkgs }:
pkgs.discord.overrideAttrs (old: {
  pname = "discord-themed";
  # Upstream's per-launch module staging uses rm, mkdir and ln from the caller's
  # PATH. Launchers with a minimal PATH skip it silently, leaving links into a
  # collected store path, so give it coreutils explicitly.
  stageModules = pkgs.writeShellScript "discord-stage-modules" ''
    PATH=${pkgs.coreutils}/bin''${PATH:+:$PATH} exec ${old.stageModules} "$@"
  '';
  nativeBuildInputs = (old.nativeBuildInputs or [ ]) ++ [
    pkgs.nodejs
    pkgs.asar
  ];
  doCheck = true;
  checkPhase = ''
    runHook preCheck
    node --check ${./theme.cjs}
    node --check ${./patch.cjs}
    runHook postCheck
  '';
  postInstall = (old.postInstall or "") + ''
    resources="$out/opt/Discord/resources"
    asar extract "$resources/app.asar" discord-app
    node ${./patch.cjs} discord-app ${./theme.cjs}
    asar pack discord-app "$resources/app.asar"
  '';
})
