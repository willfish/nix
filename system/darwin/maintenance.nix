{
  system.defaults.CustomSystemPreferences."/Library/Preferences/com.apple.SoftwareUpdate" = {
    AutomaticCheckEnabled = true;
    ScheduleFrequency = 1;
    AutomaticDownload = false;
    AutomaticallyInstallAppUpdates = false;
    AutomaticallyInstallMacOSUpdates = false;
    restrict-software-update-require-admin-to-install = true;
  };
  # Preserve Apple's existing security/config-data policy. Do not add cron
  # reboots, cache deletion, store GC or optimisation behind Determinate's back.
}
