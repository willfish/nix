# Static HTTP access to the radio archive.
#
# A player resolves the generated M3U playlists over HTTP and then plays the
# MP3s they reference, so this host only has to serve files: no transcoding,
# no library index, no authentication. The tree is read-only here.
# Rebuild scripts are deployed to /etc/radio-archive from ./radio-archive.
_:

let
  port = 8085;
in
{
  services.nginx = {
    enable = true;

    virtualHosts.radio-archive = {
      default = true;
      listen = [
        {
          addr = "0.0.0.0";
          inherit port;
        }
        {
          addr = "[::]";
          inherit port;
        }
      ];
      root = "/srv/media/radio-archive";

      # The archive is browsed by hand as well as by players.
      locations."/" = {
        extraConfig = ''
          autoindex on;
          autoindex_exact_size off;
          autoindex_format html;
          charset utf-8;
          charset_types text/plain text/html;
        '';
      };

      # M3U is not in nginx's mime table; players and cliamp want plain text.
      # Double the backslash: a Nix double-quoted string eats one.
      locations."~* \\.m3u8?$" = {
        extraConfig = ''
          default_type text/plain;
          charset utf-8;
        '';
      };

      # audio/<id>.mp3 is content-addressed. Playlists are regenerated in place.
      locations."~* \\.mp3$" = {
        extraConfig = ''
          expires 30d;
        '';
      };
    };
  };

  # The scripts that rebuild the tree and playlists live here so the pool can be
  # regenerated from the repository after a disk rebuild.
  environment.etc."radio-archive" = {
    source = ./radio-archive;
  };

  # The media pool is group-readable by users rather than world-readable by
  # design, so the web server user needs the same membership as Audiobookshelf.
  users.users.nginx.extraGroups = [ "users" ];

  # Tailnet only, same pattern as the binary cache. tailscale0 is trusted,
  # so this port stays closed on the LAN. Players use http://terminus:8085.
}
