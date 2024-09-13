{ pkgs-unstable, inputs, ... }:
{
  programs.firefox = {
    enable = true;

    profiles.william = {
        extensions = with inputs.firefox-addons.packages."x86_64-linux"; [
          bitwarden
          vimium
          i-dont-care-about-cookies
          to-google-translate
          view-image
          ublock-origin
          youtube-shorts-block
          private-relay
          elasticvue
        ];
    };
  };

  programs.brave = {
    enable = true;

    dictionaries = [
      pkgs-unstable.hunspellDictsChromium.en_GB
    ];
  };

  programs.google-chrome = {
    enable = true;
  };
}
