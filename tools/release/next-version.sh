#!/bin/sh
#
# Work out the next release version from Conventional Commits.
#
#   tools/release/next-version.sh [rev]      (rev defaults to HEAD)
#
# Prints three lines:
#
#   previous=v1.2.3     the newest vX.Y.Z tag reachable from rev, or v0.0.0
#   next=v1.3.0         the version rev should be released as
#   bump=minor          major, minor, patch or none
#
# Every commit after the previous tag is read, merge commits included. A
# merge commit's subject is GitHub's "Merge pull request #N from ...", so
# for those the pull request title - the first line of the body - is what
# gets classified. That makes a conventional PR title enough even when the
# commits inside the branch are not.
#
#   type!: ... or a "BREAKING CHANGE:" footer   -> major
#   feat: ...                                   -> minor
#   anything else                               -> patch
#
# "Anything else" includes messages that are not conventional at all: a
# change that reached master is still a change, and silently skipping a
# release for it is worse than a patch bump. While the major version is 0,
# a breaking change bumps the minor version instead, as SemVer allows.
#
# bump=none only when there is nothing new since the previous tag.
#
# With no vX.Y.Z tag at all the answer is v0.1.0 (bump=initial) without
# reading anything: the history below the first release is all of
# upstream Rockbox, and none of it is this fork's to version.

set -eu

rev=${1:-HEAD}

previous=$(git describe --tags --abbrev=0 --match 'v[0-9]*.[0-9]*.[0-9]*' \
           "$rev" 2>/dev/null || true)

if [ -z "$previous" ]; then
    echo "previous=v0.0.0"
    echo "next=v0.1.0"
    echo "bump=initial"
    exit 0
fi
range="$previous..$rev"

level=0   # 0 none, 1 patch, 2 minor, 3 major

for sha in $(git rev-list "$range"); do
    parents=$(git rev-list --parents -n 1 "$sha" | wc -w)
    subject=$(git log -1 --format=%s "$sha")
    body=$(git log -1 --format=%b "$sha")

    if [ "$parents" -gt 2 ]; then
        # Merge: classify the PR title, not "Merge pull request ...".
        title=$(printf '%s\n' "$body" | sed -n '/./{p;q;}')
        [ -n "$title" ] && subject=$title
    fi

    this=1
    if printf '%s\n' "$subject" |
       grep -Eq '^[a-zA-Z]+(\([^)]*\))?!:'; then
        this=3
    elif printf '%s\n' "$body" | grep -Eq '^BREAKING[ -]CHANGE:'; then
        this=3
    elif printf '%s\n' "$subject" |
         grep -Eiq '^feat(\([^)]*\))?:'; then
        this=2
    fi

    [ "$this" -gt "$level" ] && level=$this
    [ "$level" -eq 3 ] && break
done

ver=${previous#v}
major=${ver%%.*}; rest=${ver#*.}
minor=${rest%%.*}; patch=${rest#*.}

if [ "$level" -eq 3 ] && [ "$major" -eq 0 ]; then
    level=2
fi

case $level in
    3) bump=major; major=$((major + 1)); minor=0; patch=0 ;;
    2) bump=minor; minor=$((minor + 1)); patch=0 ;;
    1) bump=patch; patch=$((patch + 1)) ;;
    *) bump=none ;;
esac

echo "previous=$previous"
echo "next=v$major.$minor.$patch"
echo "bump=$bump"
