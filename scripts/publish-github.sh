#!/usr/bin/env bash
# publish-github.sh — publish a clean snapshot of the current repo to the PUBLIC GitHub mirror.
#
# The private origin (Gitea) is NEVER touched: it keeps the full history, the packet captures, and
# the compiled Bigscreen API key. GitHub instead gets a snapshot of the CURRENT tracked tree with:
#   - packet captures / logs excluded (they're already gitignored, this is belt-and-suspenders), and
#   - the Bigscreen client API key BLANKED OUT (it's Bigscreen's property — see the README).
#
# History model:
#   (default) rolling snapshots — one commit appended to github/main per run (a coarse public history)
#   --squash              — GitHub kept at a single, force-pushed "current release" commit (no history)
#
# The GitHub history is intentionally UNRELATED to origin's (it "starts from now"), so this never
# force-pushes origin and the two remotes never need to share commits.
#
# Copyright (C) 2026 Stefy Lanza. GNU GPL v3 or later.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GH_URL="${GH_URL:-git@github.com:nextime/bsdrX.git}"
WORK="$ROOT/.gh-publish"                       # throwaway github-tracking checkout (gitignored)
SQUASH=0; [ "${1:-}" = "--squash" ] && SQUASH=1
SRC="$(git -C "$ROOT" rev-parse --short HEAD)"
DATE="$(date +%Y-%m-%d)"
NAME="$(git -C "$ROOT" config user.name  || echo bsdrX)"
MAIL="$(git -C "$ROOT" config user.email || echo noreply@nexlab.net)"

echo ">> publishing origin @ $SRC to $GH_URL ($([ "$SQUASH" = 1 ] && echo single-commit || echo rolling))"

# 1. fresh, stateless checkout that tracks ONLY github (origin's object store is never pushed)
rm -rf "$WORK"; mkdir -p "$WORK"
git -C "$WORK" init -q -b main
git -C "$WORK" remote add github "$GH_URL"
if [ "$SQUASH" != 1 ]; then                    # rolling: start from github's current main, if any
  if git -C "$WORK" fetch -q github main 2>/dev/null; then git -C "$WORK" reset -q --hard FETCH_HEAD; fi
fi

# 2. lay down the current tracked tree (HEAD), minus captures/logs
find "$WORK" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +
git -C "$ROOT" archive HEAD | tar -x -C "$WORK" --exclude='*.pcap' --exclude='*.pcapng' --exclude='*.log'

# 3. sanitize: blank the Bigscreen API key default so it never reaches the public mirror
CLOUD_H="$WORK/include/bsdr/cloud.h"
if [ -f "$CLOUD_H" ]; then
  perl -0pi -e 's/(BSDR_CLOUD_API_KEY_DEFAULT\s*\\?\s*)"[^"]*"/$1""/s' "$CLOUD_H"
  leftover="$(perl -0ne 'print $1 if /BSDR_CLOUD_API_KEY_DEFAULT\s*\\?\s*"([^"]*)"/' "$CLOUD_H")"
  [ -z "$leftover" ] || { echo "!! ABORT: API key not blanked (len ${#leftover})" >&2; exit 1; }
fi

# 4. commit + push
git -C "$WORK" add -A
if git -C "$WORK" diff --cached --quiet; then echo ">> nothing new to publish"; exit 0; fi
if [ "$SQUASH" = 1 ]; then
  git -C "$WORK" -c user.name="$NAME" -c user.email="$MAIL" commit -q -m "bsdrX — public snapshot ($DATE, origin $SRC)"
  git -C "$WORK" push -f github main
else
  git -C "$WORK" -c user.name="$NAME" -c user.email="$MAIL" commit -q -m "snapshot from origin @ $SRC ($DATE)"
  git -C "$WORK" push github main
fi
echo ">> done."
