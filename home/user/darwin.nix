{
  config,
  lib,
  pkgs,
  ...
}:
lib.mkIf (!config.dotfiles.darwinSystemServices) {
  # Legacy Darwin homes only: retain their Aqua-dependent LaunchAgents and
  # desktop trimming. Headless nodes use nix-darwin system jobs instead.
  home.file.".local/share/dotfiles-system/limit.maxfiles.plist" = lib.mkIf pkgs.stdenv.isDarwin {
    text = ''
      <?xml version="1.0" encoding="UTF-8"?>
      <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
      <plist version="1.0">
      <dict>
        <key>Label</key>
        <string>limit.maxfiles</string>
        <key>ProgramArguments</key>
        <array>
          <string>/bin/launchctl</string>
          <string>limit</string>
          <string>maxfiles</string>
          <string>65536</string>
          <string>245760</string>
        </array>
        <key>RunAtLoad</key>
        <true/>
      </dict>
      </plist>
    '';
  };

  home.file.".local/share/dotfiles-system/io.tailscale.tailscaled.plist" =
    lib.mkIf pkgs.stdenv.isDarwin
      {
        text = ''
          <?xml version="1.0" encoding="UTF-8"?>
          <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
          <plist version="1.0">
          <dict>
            <key>Label</key>
            <string>io.tailscale.tailscaled</string>
            <key>ProgramArguments</key>
            <array>
              <string>${pkgs.tailscale}/bin/tailscaled</string>
              <string>--state=/var/lib/tailscale/tailscaled.state</string>
              <string>--socket=/var/run/tailscaled.socket</string>
            </array>
            <key>KeepAlive</key>
            <true/>
            <key>RunAtLoad</key>
            <true/>
            <key>ProcessType</key>
            <string>Background</string>
            <key>StandardOutPath</key>
            <string>/var/log/tailscaled.log</string>
            <key>StandardErrorPath</key>
            <string>/var/log/tailscaled.log</string>
          </dict>
          </plist>
        '';
      };

  home.activation.relayServerTrim = lib.mkIf pkgs.stdenv.isDarwin (
    lib.hm.dag.entryAfter [ "writeBoundary" ] ''
      defaults=/usr/bin/defaults
      launchctl=/bin/launchctl
      killall=/usr/bin/killall

      "$defaults" write com.apple.assistant.support "Assistant Enabled" -bool false
      "$defaults" write com.apple.Siri StatusMenuVisible -bool false
      "$defaults" write com.apple.Siri VoiceTriggerUserEnabled -bool false
      "$defaults" write com.apple.Siri SiriPrefStashedStatusMenuVisible -bool false
      "$defaults" write com.apple.CloudSubscriptionFeatures.gm GMSignedUp -bool false
      "$defaults" write com.apple.universalaccess reduceMotion -bool true
      "$defaults" write com.apple.universalaccess reduceTransparency -bool true
      "$defaults" write com.apple.WindowManager StandardHideWidgets -bool true
      "$defaults" write com.apple.WindowManager StageManagerHideWidgets -bool true
      "$defaults" write com.apple.WindowManager EnableStandardClickToShowDesktop -bool false
      "$defaults" write com.apple.loginwindow TALLogoutSavesState -bool false
      "$defaults" write com.apple.SubmitDiagInfo AutoSubmit -bool false
      "$defaults" write com.apple.CrashReporter DialogType none
      "$defaults" write com.apple.NetworkBrowser DisableAirDrop -bool true
      "$defaults" -currentHost write com.apple.coreservices.useractivityd ActivityAdvertisingAllowed -bool false
      "$defaults" -currentHost write com.apple.coreservices.useractivityd ActivityReceivingAllowed -bool false
      "$defaults" write com.apple.photoanalysisd EnableAutomaticAnalysis -bool false

      uid="$(id -u)"
      domain="gui/$uid"
      for label in \
        com.apple.photoanalysisd \
        com.apple.mediaanalysisd \
        com.apple.knowledgeconstructiond \
        com.apple.spotlightknowledged \
        com.apple.spotlightknowledged.updater \
        com.apple.spotlightknowledged.importer \
        com.apple.intelligenceplatformd \
        com.apple.intelligencecontextd \
        com.apple.intelligencetasksd \
        com.apple.intelligenceflowd \
        com.apple.Siri.agent \
        com.apple.SiriTTSTrainingAgent \
        com.apple.GameController.gamecontrolleragentd \
        com.apple.wallpaper.agent \
        com.apple.homed \
        homebrew.mxcl.ollama
      do
        "$launchctl" disable "$domain/$label" >/dev/null 2>&1 || true
      done

      for app in "Brave Browser" "System Settings" Safari Photos Music Mail Messages Calendar Notes Preview "Activity Monitor"
      do
        "$killall" -TERM "$app" >/dev/null 2>&1 || true
      done

      /usr/bin/mdutil -a -i off >/dev/null 2>&1 || true
    ''
  );
}
