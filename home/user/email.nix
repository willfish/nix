{
  config,
  lib,
  readSopsSecret,
  ...
}:
{
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
      };
    };
  };

  programs.himalaya.enable = true;
}
