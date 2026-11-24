# Contributing

Start with `README.md` and `CLAUDE.md`; the rules there apply to people too.

## Issues

Pick the bug or feature form. The triage workflow adds the remaining labels
from `.github/labels.yml`. Once work starts, the issue holds the goal and one
`<!-- state -->` comment holds what has been tried (see CLAUDE.md, "Device
test loop").

## Branches

Branch from `master`, one branch per issue or batch:

* People: `<type>/<issue>-<slug>`, e.g. `fix/179-audiobook-drag-back`, using
  the Conventional Commits types.
* Agents: `claude/<slug>` - the prefix Claude Code and the Claude workflow
  create.

## Pull requests

The template asks for what changed, how it was tested and the issue it
closes. The PR title is checked by `pr-title.yml` and becomes the release
note, so make it read well on its own. Every CI check must be green before
review.
