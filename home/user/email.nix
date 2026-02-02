{
  accounts.email.accounts.gmail = {
    primary = true;
    address = "william.michael.fish@gmail.com";
    realName = "William Fish";
    userName = "william.michael.fish@gmail.com";
    passwordCommand = "cat /home/william/.secrets/gmail-app-password";

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
        folder.alias = {
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
