_: {
  # Keep credentials and user settings writable and outside Home Manager.
  # qwen-pi has a separate local profile and does not use these models.
  home.file.".pi/agent/models.json".source = ../config/pi/models.json;
}
