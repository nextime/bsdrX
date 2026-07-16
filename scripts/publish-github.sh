#!/usr/bin/env bash
# publish-github.sh — publish a clean snapshot of the current repo to the PUBLIC GitHub mirror.
#
# The private origin (Gitea) is NEVER touched: it keeps the full history, the packet captures, and
# the compiled Bigscreen API key. GitHub instead gets a snapshot of the CURRENT tracked tree with:
#   - packet captures / logs excluded (they're already gitignored, this is belt-and-suspenders),
#   - the plugins/ tree PRUNED to the 'open' (open-source) plugins only: 'closed' and 'private'
#     plugins stay on the private origin and never reach the mirror, and even the open ones ship
#     without their store.conf (no pricing/publish metadata leaks). The generic plugin *system* in
#     src/plugin.c + include/bsdr/plugin.h is public and always stays, and
#   - the private plugin-store uploader (scripts/push_plugin.py) and the private authoring doc
#     (plugins/PLUGIN-AUTHORING.md) excluded — build-plugins.sh only uses the uploader when present,
#     so its absence just disables the optional store-upload step, and
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
PREV_SRC=""                                    # origin SHA recorded in github's last snapshot
if [ "$SQUASH" != 1 ]; then                    # rolling: start from github's current main, if any
  if git -C "$WORK" fetch -q github main 2>/dev/null; then
    git -C "$WORK" reset -q --hard FETCH_HEAD
    # the previous snapshot's message ends with "origin <sha>"; pull it back out to diff against
    PREV_SRC="$(git -C "$WORK" log -1 --format=%B 2>/dev/null \
                | grep -oiE 'origin @?[[:space:]]*[0-9a-f]{7,40}' | grep -oiE '[0-9a-f]{7,40}' | head -1 || true)"
  fi
fi

# Build a changelog = the origin commit subjects introduced since the last published snapshot
# (falls back to the most recent commits when there's no prior snapshot to diff against).
CHANGELOG=""
if [ -n "$PREV_SRC" ] && git -C "$ROOT" cat-file -e "${PREV_SRC}^{commit}" 2>/dev/null; then
  CHANGELOG="$(git -C "$ROOT" log --no-merges --reverse --format='- %s' "${PREV_SRC}..HEAD" 2>/dev/null || true)"
fi
[ -n "$CHANGELOG" ] || CHANGELOG="$(git -C "$ROOT" log --no-merges -15 --format='- %s' HEAD 2>/dev/null || true)"
[ -n "$CHANGELOG" ] || CHANGELOG="- (no commit history available)"

# 2. lay down the current tracked tree (HEAD), minus captures/logs, the private uploader, and the
# private authoring doc. The plugins/ tree is laid down in full here and PRUNED to 'open' in 2b.
find "$WORK" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +
git -C "$ROOT" archive HEAD | tar -x -C "$WORK" \
    --exclude='*.pcap' --exclude='*.pcapng' --exclude='*.log' \
    --exclude='.plugstore.env' \
    --exclude='scripts/push_plugin.py' \
    --exclude='scripts/push-plugins.sh' \
    --exclude='plugins/PLUGIN-AUTHORING.md'

# 2b. prune plugins/: publish ONLY plugins marked 'open' (open-source). Strip 'closed' and 'private'
# ones entirely (they stay on the private origin), and strip store.conf from the survivors so no
# store/pricing metadata leaks. A plugin's marking is the MARKING= line in its plugins/<name>/store.conf.
if [ -d "$WORK/plugins" ]; then
  for d in "$WORK"/plugins/*/; do
    [ -d "$d" ] || continue
    marking=""
    [ -f "$d/store.conf" ] && marking="$(sh -c '. "$1" >/dev/null 2>&1; printf %s "${MARKING:-}"' _ "$d/store.conf" 2>/dev/null || true)"
    if [ "$marking" = open ]; then
      rm -f "$d/store.conf"          # keep the source, drop the publish/pricing metadata
      echo ">> github: publishing open plugin '$(basename "$d")'"
    else
      echo ">> github: stripping non-open plugin '$(basename "$d")' (marking='${marking:-unset}')"
      rm -rf "$d"
    fi
  done
  # remove any stray non-dir files under plugins/, then drop plugins/ itself if now empty
  find "$WORK/plugins" -mindepth 1 -maxdepth 1 ! -type d -exec rm -f {} + 2>/dev/null || true
  rmdir "$WORK/plugins" 2>/dev/null || true
fi

# 2c. belt-and-suspenders. store.conf is stripped from every published (open) plugin, so any surviving
# store.conf means a non-open plugin slipped through unprocessed -> abort. Also abort if the private
# uploader leaked.
if [ -d "$WORK/plugins" ] && find "$WORK/plugins" -name store.conf | grep -q .; then
  echo "!! ABORT: a store.conf survived under plugins/ — a non-open plugin may have leaked" >&2
  find "$WORK/plugins" -name store.conf -print >&2
  exit 1
fi
if [ -e "$WORK/scripts/push_plugin.py" ]; then
  echo "!! ABORT: scripts/push_plugin.py leaked into the GitHub snapshot" >&2
  exit 1
fi

# 3. sanitize: blank BOTH Bigscreen API keys (companion + client) so neither reaches the public mirror
CLOUD_H="$WORK/include/bsdr/cloud.h"
if [ -f "$CLOUD_H" ]; then
  for K in BSDR_CLOUD_API_KEY_DEFAULT BSDR_CLOUD_CLIENT_KEY_DEFAULT; do
    perl -0pi -e "s/(${K}\\s*\\\\?\\s*)\"[^\"]*\"/\${1}\"\"/s" "$CLOUD_H"
    leftover="$(perl -0ne "print \$1 if /${K}\\s*\\\\?\\s*\"([^\"]*)\"/" "$CLOUD_H")"
    [ -z "$leftover" ] || { echo "!! ABORT: ${K} not blanked (len ${#leftover})" >&2; exit 1; }
  done
fi

# 4. commit + push (with a meaningful message summarising the changes in this snapshot)
git -C "$WORK" add -A
if git -C "$WORK" diff --cached --quiet; then echo ">> nothing new to publish"; exit 0; fi

if [ "$SQUASH" = 1 ]; then
  SUBJECT="bsdrX — public snapshot ($DATE)"
  RANGE_NOTE="Recent changes:"
else
  SUBJECT="bsdrX snapshot $DATE — $(echo "$CHANGELOG" | grep -c '^- ') change(s)"
  RANGE_NOTE="$([ -n "$PREV_SRC" ] && echo "Changes since the last snapshot ($PREV_SRC):" || echo "Recent changes:")"
fi

# NB: the "origin <sha>" trailer is how the NEXT run finds this snapshot's baseline — keep it.
MSG="$(printf '%s\n\n%s\n%s\n\norigin %s\n' "$SUBJECT" "$RANGE_NOTE" "$CHANGELOG" "$SRC")"

git -C "$WORK" -c user.name="$NAME" -c user.email="$MAIL" commit -q -m "$MSG"
if [ "$SQUASH" = 1 ]; then git -C "$WORK" push -f github main; else git -C "$WORK" push github main; fi
echo ">> done."
