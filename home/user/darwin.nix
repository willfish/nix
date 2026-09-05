{
  lib,
  pkgs,
  ...
}:
{
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
}
