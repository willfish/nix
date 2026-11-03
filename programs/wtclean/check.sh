#!/usr/bin/env bash
# Offline acceptance check with real Git worktrees and disposable PostgreSQL.
set -euo pipefail
source_dir=$(cd "$(dirname "$0")" && pwd)
scratch=$(mktemp -d /tmp/opencode/wtclean-check.XXXXXX)
export REAL_GIT
REAL_GIT="$(git --exec-path)/../../bin/git"
export HOME="$scratch/home" GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1
export GIT_AUTHOR_NAME=Example GIT_COMMITTER_NAME=Example
export GIT_AUTHOR_EMAIL=example@example.invalid GIT_COMMITTER_EMAIL=example@example.invalid
export FIXTURES="$scratch/responses"
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE
mkdir -p "$HOME/Repositories/hmrc" "$scratch/bin" "$FIXTURES"
repo="$HOME/Repositories/hmrc/identity"
"$REAL_GIT" init -qb main "$repo"
cd "$repo"
printf 'original\n' >tracked
printf 'generated\n' >.gitignore
cat >flake.nix <<'FLAKE'
git rev-parse --show-toplevel | md5sum | cut -c1-8
$HOME/.local/share/postgres/worktrees/$WT_ID
$HOME/.local/share/gem/worktrees/$WT_ID
$HOME/.local/share/yarn/worktrees/$WT_ID
/tmp/pg-$WT_ID
FLAKE
"$REAL_GIT" add .
"$REAL_GIT" commit -qm initial
"$REAL_GIT" remote add origin git@github.com:trade-tariff/identity.git
for name in merged detached closed dirty untracked locked unpublished hidden symlink; do
  wt="$scratch/$name"
  "$REAL_GIT" worktree add -qb "$name" "$wt"
  "$REAL_GIT" -C "$wt" commit --allow-empty -qm "$name"
  tip=$("$REAL_GIT" -C "$wt" rev-parse HEAD)
  jq -n --arg tip "$tip" '[{number:42,state:"closed",merged_at:"2026-11-03T00:00:00Z",
    head:{sha:$tip,repo:{full_name:"trade-tariff/identity"}},
    base:{ref:"main",repo:{full_name:"trade-tariff/identity"}}}]' >"$FIXTURES/$tip"
  case "$name" in
  detached) "$REAL_GIT" -C "$wt" switch -q --detach ;;
  closed)
    jq '.[0].merged_at=null' "$FIXTURES/$tip" >"$scratch/reply"
    mv "$scratch/reply" "$FIXTURES/$tip"
    ;;
  dirty) printf 'local change\n' >"$wt/tracked" ;;
  untracked) printf 'local file\n' >"$wt/local" ;;
  locked) "$REAL_GIT" worktree lock "$wt" ;;
  unpublished)
    "$REAL_GIT" -C "$wt" commit --allow-empty -qm unpublished
    tip=$("$REAL_GIT" -C "$wt" rev-parse HEAD)
    echo '[]' >"$FIXTURES/$tip"
    ;;
  hidden)
    "$REAL_GIT" -C "$wt" update-index --assume-unchanged tracked
    printf 'hidden change\n' >"$wt/tracked"
    ;;
  esac
done
printf 'disposable ignored output\n' >"$scratch/merged/generated"
ident=$(printf '%s\n' "$scratch/merged" | md5sum | cut -c1-8)
data="$HOME/.local/share/postgres/worktrees/$ident"
symlink_id=$(printf '%s\n' "$scratch/symlink" | md5sum | cut -c1-8)
mkdir -p "${data%/*}" "$HOME/precious" "$HOME/.local/share/yarn/worktrees/$ident"
ln -s "$HOME/precious" "${data%/*}/$symlink_id"
mkdir "${data%/*}/deadbeef"
pg_bin=$(dirname "$(readlink -f "$(command -v pg_ctl)")")
"$pg_bin/initdb" -D "$data" --auth=trust --no-locale >"$scratch/initdb.log"
mkdir "$scratch/sockets"
"$pg_bin/pg_ctl" start -D "$data" -l "$scratch/postgres.log" -o "-k $scratch/sockets -c listen_addresses=''" -w
cleanup() {
  if [[ -f "$data/postmaster.pid" ]]; then "$pg_bin/pg_ctl" stop -D "$data" -m fast -w; fi
  rm -rf -- "$scratch"
}
trap cleanup EXIT
cat >"$scratch/bin/gh" <<'GH'
#!/usr/bin/env bash
set -euo pipefail
[[ "$1" == api ]]
if [[ "$2" == repos/trade-tariff/identity ]]; then echo main; exit; fi
revision=${2#*/commits/}; revision=${revision%%/*}
cat "$FIXTURES/$revision"
GH
ln -s "$REAL_GIT" "$scratch/bin/git"
chmod +x "$scratch/bin/gh"
export PATH="$scratch/bin:$PATH"
bash "$source_dir/wtclean.sh" --check-repo "$repo"
bash "$source_dir/wtclean.sh" --dry-run --repo "$repo" >"$scratch/preview"
cat "$scratch/preview"
[[ -d "$scratch/merged" && -d $data ]]
grep -q '2 worktree(s) eligible' "$scratch/preview"
bash "$source_dir/wtclean.sh" --yes --repo "$repo"
[[ ! -e "$scratch/merged" && ! -e "$scratch/detached" && ! -e $data ]]
[[ ! -e "$HOME/.local/share/yarn/worktrees/$ident" ]]
for name in closed dirty untracked locked unpublished hidden symlink; do [[ -d "$scratch/$name" ]]; done
[[ -d "$HOME/precious" && -d "${data%/*}/deadbeef" ]]
"$REAL_GIT" show-ref --verify --quiet refs/heads/merged
[[ $("$REAL_GIT" branch --show-current) == main ]]

# An unknown PostgreSQL executable must preserve both the checkout and database.
"$REAL_GIT" worktree add -qb faildb "$scratch/faildb"
"$REAL_GIT" -C "$scratch/faildb" commit --allow-empty -qm faildb
tip=$("$REAL_GIT" -C "$scratch/faildb" rev-parse HEAD)
jq --arg tip "$tip" '.[0].head.sha=$tip' "$FIXTURES/$("$REAL_GIT" rev-parse merged)" >"$FIXTURES/$tip"
fail_id=$(printf '%s\n' "$scratch/faildb" | md5sum | cut -c1-8)
mkdir "${data%/*}/$fail_id"
printf '123456789\n' >"${data%/*}/$fail_id/postmaster.pid"
if bash "$source_dir/wtclean.sh" --yes --repo "$repo"; then
  echo 'Unexpected success with an unknown PostgreSQL executable' >&2
  exit 1
fi
[[ -d "$scratch/faildb" && -f "${data%/*}/$fail_id/postmaster.pid" ]]
"$REAL_GIT" remote set-url origin git@github.com:other/identity.git
if bash "$source_dir/wtclean.sh" --check-repo "$repo"; then
  echo 'Accepted out-of-scope origin' >&2
  exit 1
fi
echo 'PASS: merged tips and live database removed; unsafe checkouts, failed-stop state, unrelated state and out-of-scope repos protected'
