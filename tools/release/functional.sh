#!/bin/sh
#
# List the files a change touches that make a difference on the player.
#
#   tools/release/functional.sh <git diff arguments>
#
#   tools/release/functional.sh origin/master...HEAD
#   tools/release/functional.sh abc123^ abc123
#
# Prints one path per line. Nothing printed means the change is
# non-functional: documentation and the machinery around the build, which
# neither the device build (build.yml) nor a release (next-version.sh)
# has any reason to run for. A change with both kinds of file prints the
# functional ones, so it builds and releases as before.
#
# Non-functional:
#
#   *.md anywhere, docs/     documentation, and the configs that live in it
#   .github/, .claude/       pipelines and agent settings
#   tools/release/           these scripts
#   tools/stick_test/        the stick tests, which build.yml runs anyway
#   .gitignore, .gitattributes, LICENSE

set -eu

git diff --name-only "$@" |
    grep -Ev '(\.md$|^docs/|^\.github/|^\.claude/|^tools/release/|^tools/stick_test/|^\.gitignore$|^\.gitattributes$|^LICENSE)' ||
    true
