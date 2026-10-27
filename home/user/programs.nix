{
  pkgs,
  lib,
  isGraphicalLinux,
  ...
}:
{
  programs = {
    brave = lib.mkIf isGraphicalLinux {
      enable = true;

      commandLineArgs = [
        "--remote-debugging-port=9222"
        # The nixpkgs wrapper enables VA-API video decode/encode. On the RX 7600
        # (Navi 33) Brave's VA-API jobs hang the vcn_unified_0 ring; when the
        # ring reset fails amdgpu does a full mode1 reset, VRAM is lost and
        # Hyprland aborts. Keep video on the CPU until Mesa/amdgpu fix this.
        "--disable-accelerated-video-decode"
        "--disable-accelerated-video-encode"
      ];

      dictionaries = [
        pkgs.hunspellDictsChromium.en_GB
      ];
    };

    direnv = {
      enable = true;
      nix-direnv.enable = true;
    };

    # Disable man-db cache generation during activation. The openssl package
    # ships ~6200 man3 pages (including a cluster of OSSL_CMP_* ones that
    # mandb 2.13.1 frequently fails to index). This produces noisy but harmless
    # "failed to store entry" warnings in the man-cache step of `nh home switch`.
    # Direct `man` still works; we just skip the (slow, spammy) whatis indexing.
    man.generateCaches = false;
  };
}
