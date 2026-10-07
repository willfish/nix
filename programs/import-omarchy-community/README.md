# Omarchy community theme importer

`import-omarchy-community [--root CHECKOUT] PAGE.html SOURCE_URL` reads a downloaded,
reviewed page and updates the marked community-input block plus
`home/user/themes/community.json`. The checkout defaults to the working directory.
Review both changes before updating the flake lock.

HTML5 token parsing preserves explicit figure boundaries and last-duplicate
attributes. External resources are disabled. Slugs, GitHub URLs and labels are
validated before writing; duplicate or empty catalogues fail. Unavailable themes
remain in the catalogue without becoming inputs. Writes are ordered, not an
all-files transaction. The importer never fetches or executes theme code.

Build with `nix build .#import-omarchy-community`. Manual checks use disposable
checkouts in the [repository command collection](../collections/repo-tools).
