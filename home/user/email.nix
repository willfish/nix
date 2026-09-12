{
  config,
  lib,
  readSopsSecret,
  ...
}:
lib.mkIf config.dotfiles.capabilities.email {
  accounts.email.accounts.gmail = {
    primary = true;
    address = "william.michael.fish@gmail.com";
    realName = "William Fish";
    userName = "william.michael.fish@gmail.com";
    passwordCommand = "${readSopsSecret}/bin/read-sops-secret ${lib.escapeShellArg config.sops.secrets.GMAIL_APP_PASSWORD.path}";

    imap = {
      host = "imap.gmail.com";
      port = 993;
      tls.enable = true;
    };

    smtp = {
      host = "smtp.gmail.com";
      port = 465;
      tls.enable = true;
    };

    himalaya = {
      enable = true;
      settings = {
        folder.aliases = {
          inbox = "INBOX";
          sent = "[Gmail]/Sent Mail";
          drafts = "[Gmail]/Drafts";
          trash = "[Gmail]/Trash";
          spam = "[Gmail]/Spam";
        };
        # Gmail SMTP already files Sent; Himalaya must not IMAP-append a second copy.
        message.send.save-copy = false;
      };
    };
  };

  programs.himalaya.enable = true;
}
