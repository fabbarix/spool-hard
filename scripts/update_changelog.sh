#!/usr/bin/env bash
set -euo pipefail

# Update CHANGELOG.md with a new section for the current console/scale
# VERSION, drawn from `git log <previous-tag>..HEAD`. Designed to be run
# right before tagging a release.
#
# - Pulls the version from console/VERSION (canonical; both products are
#   bumped in lockstep).
# - Detects the previous tag automatically (last tag matching `v*`). If
#   there is none, treats every commit on the current history as part of
#   the release.
# - Drops any commit subject containing `[chore]` (case-insensitive).
# - Inserts the new section above the most recent existing one in
#   CHANGELOG.md, scaffolding the file if it doesn't exist yet.
#
# Typical workflow:
#
#   1. Bump  console/VERSION  and  scale/VERSION.
#   2. ./scripts/update_changelog.sh
#   3. Review the new section, edit if needed.
#   4. git add CHANGELOG.md console/VERSION scale/VERSION
#      git commit -m "[chore] release v$(cat console/VERSION)"
#   5. git tag "v$(cat console/VERSION)"
#   6. git push --follow-tags

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CHANGELOG="$REPO_ROOT/CHANGELOG.md"

VERSION_FILE="$REPO_ROOT/console/VERSION"
if [[ ! -f "$VERSION_FILE" ]]; then
  echo "Missing $VERSION_FILE — bump the version files before running this." >&2
  exit 1
fi
VERSION=$(tr -d '[:space:]' < "$VERSION_FILE")
DATE=$(date +%Y-%m-%d)

# Most recent v-prefixed tag, or empty if none.
PREV_TAG=$(git -C "$REPO_ROOT" describe --tags --abbrev=0 --match='v*' 2>/dev/null || true)

if [[ -z "$PREV_TAG" ]]; then
  RANGE=""                           # all commits on current history
  SCOPE_LABEL="(initial release)"
else
  RANGE="${PREV_TAG}..HEAD"
  SCOPE_LABEL="(since $PREV_TAG)"
fi

# Subject lines for the range, filtered. `--no-merges` drops "Merge branch …"
# noise; `[chore]` filter is case-insensitive. Both grep and sed get a
# `|| true` because `set -e` would otherwise abort on grep's "no match"
# exit (1), which is the legitimate "nothing to do" case.
COMMITS=$(git -C "$REPO_ROOT" log --no-merges --pretty=format:'%s' $RANGE 2>/dev/null \
  | { grep -viE '\[chore\]' || true; } \
  | sed 's/^/- /')

if [[ -z "$COMMITS" ]]; then
  echo "No non-chore commits to report $SCOPE_LABEL — nothing to do."
  exit 0
fi

# Build the new section.
read -r -d '' ENTRY <<EOF || true
## [$VERSION] - $DATE

$COMMITS
EOF

if [[ ! -f "$CHANGELOG" ]]; then
  cat > "$CHANGELOG" <<EOF
# Changelog

All notable changes to SpoolHard are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

$ENTRY
EOF
  echo "Created $CHANGELOG with [$VERSION] $SCOPE_LABEL"
  exit 0
fi

# Insert the new entry before the first existing release section. If the file
# has no release sections yet (only the header), append at the end.
TMP=$(mktemp)
awk -v entry="$ENTRY" '
  /^## \[/ && !inserted { print entry; print ""; inserted=1 }
  { print }
  END { if (!inserted) { print ""; print entry } }
' "$CHANGELOG" > "$TMP" && mv "$TMP" "$CHANGELOG"

echo "Updated $CHANGELOG with [$VERSION] $SCOPE_LABEL"
