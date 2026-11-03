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
for name in merged detached snapshot closed stopped open open-detached open-review dirty untracked locked unpublished hidden symlink missing; do
  wt="$scratch/$name"
  "$REAL_GIT" worktree add -qb "$name" "$wt"
  "$REAL_GIT" -C "$wt" commit --allow-empty -qm "$name"
  tip=$("$REAL_GIT" -C "$wt" rev-parse HEAD)
  jq -n --arg tip "$tip" --arg name "$name" '[{number:42,state:"closed",merged_at:"2026-11-03T00:00:00Z",
    head:{ref:$name,sha:$tip,repo:{full_name:"trade-tariff/identity"}},
    base:{ref:"main",repo:{full_name:"trade-tariff-archive/identity"}}}]' >"$FIXTURES/$tip"
  case "$name" in
  detached)
    "$REAL_GIT" -C "$wt" switch -q --detach
    ;;
  snapshot)
    "$REAL_GIT" -C "$wt" switch -q --detach
    printf 'missing\n' >"$FIXTURES/$tip"
    ;;
  closed)
    jq '.[0].merged_at=null | .[0].base.ref="stacked-parent"' "$FIXTURES/$tip" >"$scratch/reply"
    mv "$scratch/reply" "$FIXTURES/$tip"
    ;;
  open)
    jq '.[0].merged_at=null | .[0].state="open" | .[0].head.repo.full_name="other/identity"' "$FIXTURES/$tip" >"$scratch/reply"
    mv "$scratch/reply" "$FIXTURES/$tip"
    cp "$FIXTURES/$tip" "$FIXTURES/open.json"
    "$REAL_GIT" -C "$wt" commit --allow-empty -qm 'local work ahead of open PR'
    printf 'uncommitted work on open PR\n' >"$wt/tracked"
    ;;
  open-detached | open-review)
    remote_tip=$tip
    number=43
    if [[ $name == open-review ]]; then
      remote_tip=0000000000000000000000000000000000000000
      number=44
    fi
    jq --arg ref "origin-$name" --arg sha "$remote_tip" --argjson number "$number" \
      '.[0].state="open" | .[0].merged_at=null | .[0].head.ref=$ref | .[0].head.sha=$sha | .[0].number=$number' \
      "$FIXTURES/$tip" >"$scratch/reply"
    mv "$scratch/reply" "$FIXTURES/$tip"
    jq -s '.[0]+.[1]' "$FIXTURES/open.json" "$FIXTURES/$tip" >"$scratch/reply"
    mv "$scratch/reply" "$FIXTURES/open.json"
    "$REAL_GIT" -C "$wt" switch -q --detach
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
  missing) rm -rf -- "$wt" ;;
  esac
done
printf 'disposable ignored output\n' >"$scratch/merged/generated"
ident=$(printf '%s\n' "$scratch/merged" | md5sum | cut -c1-8)
data="$HOME/.local/share/postgres/worktrees/$ident"
symlink_id=$(printf '%s\n' "$scratch/symlink" | md5sum | cut -c1-8)
mkdir -p "${data%/*}" "$HOME/precious" "$HOME/.local/share/yarn/worktrees/$ident"
ln -s "$HOME/precious" "${data%/*}/$symlink_id"
mkdir "${data%/*}/deadbeef"
missing_id=$(printf '%s\n' "$scratch/missing" | md5sum | cut -c1-8)
mkdir "${data%/*}/$missing_id"
stopped_id=$(printf '%s\n' "$scratch/stopped" | md5sum | cut -c1-8)
mkdir "${data%/*}/$stopped_id"
printf '"/nix/store/00000000000000000000000000000000-gone-postgresql/bin/postgres"\n' >"${data%/*}/$stopped_id/postmaster.opts"
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
if [[ "$2" == repos/trade-tariff/identity ]]; then echo '{"full_name":"trade-tariff-archive/identity"}'; exit; fi
if [[ "$2" == 'repos/trade-tariff-archive/identity/pulls?state=open&per_page=100' ]]; then
  cat "$FIXTURES/open.json"
  exit
fi
revision=${2#*/commits/}; revision=${revision%%/*}
if [[ $(<"$FIXTURES/$revision") == missing ]]; then
  echo "gh: No commit found for SHA: $revision (HTTP 422)" >&2
  exit 1
fi
if [[ $(<"$FIXTURES/$revision") == api-error ]]; then
  echo 'gh: Service Unavailable (HTTP 503)' >&2
  exit 1
fi
cat "$FIXTURES/$revision"
GH
ln -s "$REAL_GIT" "$scratch/bin/git"
chmod +x "$scratch/bin/gh"
export PATH="$scratch/bin:$PATH"
bash "$source_dir/wtclean.sh" --check-repo "$repo"
bash "$source_dir/wtclean.sh" --dry-run --repo "$repo" >"$scratch/preview"
cat "$scratch/preview"
[[ -d "$scratch/merged" && -d $data ]]
grep -q 'Would remove 9 worktrees; would prune 1 stale registrations.' "$scratch/preview"
if grep -q '^KEEP ' "$scratch/preview"; then
  echo 'Default preview is too noisy' >&2
  exit 1
fi
bash "$source_dir/wtclean.sh" >"$scratch/applied" 2>&1
cat "$scratch/applied"
grep -q 'Removed 9 worktrees; pruned 1 stale registrations.' "$scratch/applied"
[[ ! -e "$scratch/merged" && ! -e "$scratch/detached" && ! -e "$scratch/closed" && ! -e $data ]]
[[ ! -e "$HOME/.local/share/yarn/worktrees/$ident" ]]
for name in open open-detached open-review locked symlink; do [[ -d "$scratch/$name" ]]; done
for name in merged detached snapshot closed stopped dirty untracked unpublished hidden; do [[ ! -e "$scratch/$name" ]]; done
[[ ! -e "${data%/*}/$stopped_id" ]]
[[ $(<"$scratch/open/tracked") == 'uncommitted work on open PR' ]]
[[ -d "$HOME/precious" && -d "${data%/*}/deadbeef" && -d "${data%/*}/$missing_id" ]]
if "$REAL_GIT" worktree list --porcelain | grep -q "^worktree $scratch/missing$"; then
  echo 'Missing registration was not pruned' >&2
  exit 1
fi
"$REAL_GIT" show-ref --verify --quiet refs/heads/merged
"$REAL_GIT" show-ref --verify --quiet refs/heads/closed
[[ $("$REAL_GIT" branch --show-current) == main ]]
bash "$source_dir/wtclean.sh" --dry-run >"$scratch/nothing" 2>&1
[[ $(wc -l <"$scratch/nothing") == 2 ]]
grep -q 'Would remove 0 worktrees' "$scratch/nothing"
bash "$source_dir/wtclean.sh" --dry-run --verbose >"$scratch/verbose" 2>&1
grep -q '^KEEP ' "$scratch/verbose"

# An unknown PostgreSQL executable must preserve both the checkout and database.
"$REAL_GIT" worktree add -qb faildb "$scratch/faildb"
"$REAL_GIT" -C "$scratch/faildb" commit --allow-empty -qm faildb
tip=$("$REAL_GIT" -C "$scratch/faildb" rev-parse HEAD)
jq --arg tip "$tip" '.[0].head.sha=$tip' "$FIXTURES/$("$REAL_GIT" rev-parse merged)" >"$FIXTURES/$tip"
fail_id=$(printf '%s\n' "$scratch/faildb" | md5sum | cut -c1-8)
mkdir "${data%/*}/$fail_id"
printf '123456789\n' >"${data%/*}/$fail_id/postmaster.pid"
"$REAL_GIT" worktree add -qb api-error "$scratch/api-error"
"$REAL_GIT" -C "$scratch/api-error" commit --allow-empty -qm api-error
tip=$("$REAL_GIT" -C "$scratch/api-error" rev-parse HEAD)
printf 'api-error\n' >"$FIXTURES/$tip"
status=0
bash "$source_dir/wtclean.sh" --repo "$repo" >"$scratch/failure" 2>&1 || status=$?
[[ $status == 1 ]]
cat "$scratch/failure"
grep -q 'Incomplete:' "$scratch/failure"
grep -q 'trade-tariff-archive/identity.*Service Unavailable (HTTP 503)' "$scratch/failure"
if grep -q '^gh:' "$scratch/failure"; then
  echo 'Leaked raw GitHub diagnostics' >&2
  exit 1
fi
[[ -d "$scratch/faildb" && -f "${data%/*}/$fail_id/postmaster.pid" ]]
"$REAL_GIT" remote set-url origin git@github.com:other/identity.git
if bash "$source_dir/wtclean.sh" --check-repo "$repo"; then
  echo 'Accepted out-of-scope origin' >&2
  exit 1
fi
echo 'PASS: unbacked dirty/unpublished worktrees removed; open PR work with a changed tip preserved; live database stopped; uncertain state and lookups protected'
