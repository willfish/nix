{ pkgs }:
pkgs.slack.overrideAttrs (old: {
  pname = "slack-themed";
  nativeBuildInputs = (old.nativeBuildInputs or [ ]) ++ [ pkgs.nodejs ];
  postInstall = (old.postInstall or "") + ''
    resources="$out/lib/slack/resources"
    asar extract "$resources/app.asar" slack-app
    node ${./patch.cjs} slack-app ${./theme.cjs} ${./keys.cjs}
    # Keep native modules outside ASAR, as required by Electron's native loader.
    asar pack slack-app "$resources/app.asar" --unpack-dir node_modules
  '';
})
