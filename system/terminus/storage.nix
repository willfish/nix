_: {
  boot.zfs.forceImportRoot = false;

  services.zfs.autoScrub = {
    enable = true;
    pools = [ "tank" ];
  };

  services.smartd = {
    enable = true;
    autodetect = true;
    notifications.wall.enable = true;
  };

  fileSystems."/srv/media" = {
    device = "tank/media";
    fsType = "zfs";
  };

  systemd.tmpfiles.rules = [
    "d /srv 0755 root root -"
    "d /srv/media 0755 william users -"
    "d /srv/media/immich 0700 immich immich -"
    "d /srv/media/audiobooks 0755 william users -"
    "d /srv/media/audiobooks-celine 0755 william users -"
    "d /srv/media/audiobooks-children 0755 william users -"
    "d /srv/media/audiobooks-john 0755 william users -"
    "d /srv/media/phone-audiobooks 0755 william users -"
    "d /srv/media/imports 0755 william users -"
    "d /srv/media/photos 0755 william users -"
    "d /srv/media/videos 0755 william users -"
    "d /srv/media/phone-backups 0755 william users -"
  ];
}
