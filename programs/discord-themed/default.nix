{ pkgs }:
pkgs.discord.overrideAttrs (old: {
  pname = "discord-themed";
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
