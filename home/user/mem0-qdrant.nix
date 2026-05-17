{ config, lib, pkgs, ... }:
# Docker Compose service for Qdrant (Mem0 vector DB backend)
{
  home.activationScripts.mem0-qdrant = {
    text = ''
      if pkgs.stdenv.isDarwin; then
        # macOS: manage via regular docker-compose
        : # Handled by launchd/brew services
      else
        # Linux: systemd user service
        ${pkgs.docker-compose}/bin/docker-compose -f ~/.dotfiles/home/config/docker-compose/mem0-qdrant/docker-compose.yaml up -d
      fi
    '';
    deps = [ "darwinSetup" ];
  };
}