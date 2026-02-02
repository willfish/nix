# Tools

Available tools and capabilities.

## Himalaya (Email)

CLI email client connected to `william.michael.fish@gmail.com`.

### Listing & Searching Emails

- `himalaya envelope list` — list recent inbox envelopes
- `himalaya envelope list -s 20` — list 20 envelopes (default page size)
- `himalaya envelope list -f sent` — list sent emails
- `himalaya envelope list from alice` — filter by sender
- `himalaya envelope list subject meeting` — filter by subject
- `himalaya envelope list from alice and subject meeting` — combine filters
- `himalaya envelope list body "search term"` — search message bodies
- `himalaya envelope list after 2026-01-01` — filter by date
- `himalaya envelope list order by date desc` — sort by date descending

### Reading & Managing Messages

- `himalaya message read <id>` — read a message by envelope ID
- `himalaya message thread <id>` — read all messages in a thread
- `himalaya message write` — compose a new message (opens editor)
- `himalaya message reply <id>` — reply to a message
- `himalaya message reply -a <id>` — reply-all
- `himalaya message forward <id>` — forward a message
- `himalaya message copy <id> -f inbox sent` — copy to another folder
- `himalaya message move <id> -f inbox trash` — move to another folder
- `himalaya message delete <id>` — move to trash

### Flags

- `himalaya flag add <id> seen` — mark as read
- `himalaya flag add <id> flagged` — star/flag a message
- `himalaya flag remove <id> flagged` — remove star/flag
- `himalaya flag remove <id> seen` — mark as unread

### Attachments

- `himalaya attachment download <id>` — download attachments from a message

### Folders

- `himalaya folder list` — list all folders/labels

### Folder Aliases

These Gmail labels are configured as folder aliases:

| Alias  | Gmail Label       |
|--------|-------------------|
| inbox  | INBOX             |
| sent   | [Gmail]/Sent Mail |
| drafts | [Gmail]/Drafts    |
| trash  | [Gmail]/Trash     |
| spam   | [Gmail]/Spam      |
