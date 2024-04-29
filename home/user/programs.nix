{ inputs, ... }:
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
}
