---
name: cubrid-backport
description: Backport a merged develop fix to CUBRID release branches — verify applicability per branch, cherry-pick, open one PR per branch, and optionally mark a Notion tracking page done. Use when the user says "backport"/"백포트" or a JIRA ticket's Planned Versions lists patch versions.
---

# CUBRID Backport

Backport a merged develop fix to CUBRID release branches: verify applicability per branch, cherry-pick, open one PR per branch, and optionally mark a Notion tracking page done.

## When to Use

- User says "backport", "백포트", or points at a JIRA ticket whose Planned Version(s) lists patch versions
- A fix is already merged to `develop` and needs to land on `release/*` branches

## Arguments

- `/cubrid-backport CBRD-XXXXX` — derive everything from the JIRA ticket
- `/cubrid-backport CBRD-XXXXX 7346` — ticket + original develop PR number
- Optional: Notion page URL → enables the Notion completion step

## Prerequisites

- Local clone at `~/dev/cubrid` with remotes `origin` (CUBRID/cubrid, fetch-only) and `xmilex` (xmilex-git/cubrid, push)
- `gh` authenticated as xmilex-git
- `ntn` CLI authenticated (only if Notion step requested)

## Execution Steps

### Step 1: Determine targets

1. Fetch the JIRA ticket (`read` on `http://jira.cubrid.org/browse/CBRD-XXXXX`).
2. Read **Planned Version(s)**. Map to branches: `11.4 Patch N` → `release/11.4`, etc. `guava` = develop (already merged — exclude).
3. If the user supplied explicit branches, they win; note any mismatch with JIRA.

### Step 2: Identify the fix commit

```bash
cd ~/dev/cubrid && git fetch origin
git log origin/develop --grep=CBRD-XXXXX --format='%H %s'
```

Confirm the commit's file list matches the original PR (`gh pr view <N> --repo CUBRID/cubrid`).

### Step 3: Verify applicability per branch (throwaway worktrees)

```bash
C=<fix-commit>
for v in 11.4 11.3 11.0 10.2; do
  wt=/tmp/bk-CBRD-XXXXX-$v
  git worktree add -q --detach $wt origin/release/$v
  if git -C $wt cherry-pick --no-commit $C >/dev/null 2>&1; then echo "$v: CLEAN"
  else echo "$v: CONFLICT"; git -C $wt status --porcelain | grep -E '^(UU|AA|DU|UD)'; git -C $wt cherry-pick --abort
  fi
done
```

**Semantic check (mandatory even on clean picks)** — release branches drift from develop:

1. Extract the touched function(s) from each branch and diff against the develop pre-fix version:
   ```bash
   git show origin/release/$v:path/file.c | awk '/^func_name/{f=1} f{print} f&&/^}/{exit}'
   ```
2. Any difference must be explained (dead code leftovers are fine; behavioral differences are not).
3. Diff the post-cherry-pick function against the develop post-fix version — expect identity modulo explained drift.
4. Confirm the bug actually exists on the branch (the broken sequence is present), so the backport is meaningful.

On CONFLICT: report hunks to the user, resolve manually, and document the resolution in the PR body.

### Step 4: Commit and push

Skip local builds — CircleCI builds every PR.

```bash
for v in ...; do
  wt=/tmp/bk-CBRD-XXXXX-$v
  git -C $wt reset --hard origin/release/$v
  git -C $wt checkout -B CBRD-XXXXX-release-$v
  git -C $wt cherry-pick -x $C        # -x records the origin commit
  git -C $wt push xmilex CBRD-XXXXX-release-$v -u
done
```

Branch naming: `CBRD-XXXXX-release-<ver>`. One commit per branch. Squash-merge later replaces the message with the PR title, so do not polish commit messages.

### Step 5: Open PRs (not draft)

Title: `[CBRD-XXXXX] [Backport] <original JIRA title verbatim>` — identical across all PRs; the base branch carries the target.

Body style: follow [cubrid-pr-create tone_guide](../cubrid-pr-create/tone_guide.md). Structure:

- JIRA link at top
- `### Purpose` — 2–3 sentence Korean summary of root cause + fix (condensed from the original PR)
- Last line: `develop \`<hash>\` (#<PR>) 의 clean cherry-pick 입니다.` (or describe conflict resolutions)

```bash
gh pr create --repo CUBRID/cubrid \
  --base release/$v \
  --head xmilex-git:CBRD-XXXXX-release-$v \
  --assignee xmilex-git \
  --title "..." --body "$(cat <<'EOF' ... EOF)"
```

### Step 6 (optional): Notion completion

Only when a Notion page URL was given. Page ID = trailing 32-hex of the URL.

```bash
ntn api /v1/pages/<PAGE_ID> -X PATCH 'properties[상태][select][name]=완료'
ntn api /v1/blocks/<PAGE_ID>/children -X PATCH -d '{"children":[
  {"heading_3":{"rich_text":[{"text":{"content":"Backport PRs (YYYY-MM-DD)"}}]}},
  {"bulleted_list_item":{"rich_text":[{"text":{"content":"release/11.4 — PR #NNNN","link":{"url":"https://github.com/CUBRID/cubrid/pull/NNNN"}}}]}}
]}'
```

Timing: flip to 완료 as soon as all PRs are open (submission done); merge tracking stays on GitHub.

### Step 7: Cleanup and report

```bash
for v in ...; do git worktree remove --force /tmp/bk-CBRD-XXXXX-$v; done
```

Report: PR URLs per branch, clean/conflict status per branch, semantic-check summary, Notion result.

## Tips

- Never push to `origin` — it is fetch-only by convention (`DO_NOT_PUSH` push URL).
- If Planned Version(s) and the user's branch list disagree, ask before proceeding.
- TC merge-gate and CircleCI bots attach automatically; merge order is TC PR first, then engine PR.
