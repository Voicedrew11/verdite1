#!/usr/bin/env bash
# Start a three-way merge from upstream RecompOne into this fork.
#
# Run it in a working clone of the fork, not in a game repository. The merge base
# is real: this fork's history descends from upstream, and UPSTREAM names the
# upstream commit last merged. Once the merge is resolved and committed, each
# game takes it with `bash scripts/setup_tools.sh --pull-fork`.
#
# Upstream rejects AI-authored pull requests. Nothing goes upstream from here:
# no pull requests and no issues.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"
UPSTREAM_URL="https://github.com/BlackLabelHQ/RecompOne.git"
UPSTREAM_BRANCH="master"

if ! git remote get-url upstream >/dev/null 2>&1; then
    git remote add upstream "$UPSTREAM_URL"
fi
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    echo "the working tree has changes; commit or stash them first" >&2
    exit 1
fi
git fetch --quiet upstream

base="$(cat UPSTREAM)"
head="$(git rev-parse "upstream/$UPSTREAM_BRANCH")"
echo "==> fork is at upstream $base"
echo "==> upstream $UPSTREAM_BRANCH is at $head"
if [ "$base" = "$head" ]; then
    echo "    already current; nothing to harvest."
    exit 0
fi
echo "    $(git rev-list --count "$base..$head") commit(s) to consider:"
git log --oneline "$base..$head" | sed 's/^/      /'
echo

echo "==> merging upstream into the fork"
set +e
git merge --no-commit "upstream/$UPSTREAM_BRANCH"
rc=$?
set -e
cat <<EOF

The merge is in the working tree, with conflicts left in the files.
psyq.json and NotoSansCJK-Regular.otf are left out of this fork on purpose; if
upstream changed either, resolve with \`git rm\` on it.

  resolve:  \$EDITOR the conflicted files, then git add <file>
  finish:   echo $head > UPSTREAM
            git commit
            # in a game, on a scratch branch:
            #   VERDITE_FORK_URL=$(pwd) bash scripts/setup_tools.sh --pull-fork <branch>
            #   bash scripts/setup_tools.sh          # build it
            # then run the game and check: 144 fps drawn at 20 ticks/s, the
            # agent beacon reaching an fdat overlay with a real position
  abandon:  git merge --abort

Take one upstream commit rather than all of them with:
  git cherry-pick -n <sha>
EOF
exit $rc
