# Skill catalogue audit

`audit-skills [--root CHECKOUT]` audits the public skill catalogue, deployment
parity, frontmatter, references, explicit-invocation metadata and freshness dates.
It returns nonzero for errors or warnings. Frontmatter handling is a text
convention, not a YAML parser.

Source references must resolve within the LLM root. Duplicate JSON, unsafe
mappings and local/mapped collisions fail before auditing. The command does not
run skill scripts, load private overlays or authorize changes.

Build with `nix build .#audit-skills`. Use an explicit checkout root when running
elsewhere. Manual fixtures live with the
[repository command collection](../collections/repo-tools).
