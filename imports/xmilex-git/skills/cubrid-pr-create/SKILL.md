---
name: cubrid-pr-create
description: Open a GitHub pull request for the CUBRID project. Use this when the user wants to create a PR for their CUBRID changes.
---

# CUBRID PR Creator

Create GitHub pull requests for the CUBRID project in the author's hand-written style: concise, factual, no filler.

## When to Use

- User says "create pr", "make pr", "PR 만들어", "PR 올려", "풀리퀘"
- User wants to push changes and open a PR against CUBRID/CUBRID or a fork

## Arguments

- `/cubrid-pr-create CBRD-26583` — Use this JIRA ticket number
- `/cubrid-pr-create CBRD-26583 feat/oos` — Ticket + base branch
- `/cubrid-pr-create` — Interactive: detect from branch name or ask

## Conventions

### Title

```
[CBRD-XXXXX] <JIRA ticket title, verbatim>
```

- JIRA ticket number **required**. Extract from branch name (e.g., `cbrd-26583-oos-compact` → `CBRD-26583`) or ask.
- Description = the **JIRA ticket title as-is**. Do not compose a new one. Get it via `/jira CBRD-XXXXX`; if the ticket cannot be fetched, ask the user for the ticket title.

### Body

**Read [tone_guide.md](./tone_guide.md) before drafting. It is the single source of truth for body style.** In short:

- JIRA link (`<http://jira.cubrid.org/browse/CBRD-XXXXX>`) at the very top.
- `### Purpose` — always. Korean prose, 1 sentence to 3 paragraphs depending on change size.
- `### Implementation` — only when the change spans multiple files/modules or adds new structures. Omit for one-or-two-function fixes.
- `### Remarks` — exceptional; only for constraints/follow-ups a reviewer must know.
- A 6-line body with a one-sentence Purpose is a normal, correct PR. Never pad.

## Execution Steps

### Step 1: Gather Context

Run in parallel:

1. `git status` — check for uncommitted changes
2. `git branch -vv` — current branch and tracking info
3. `git remote -v` — available remotes

If there are uncommitted changes, warn the user and ask whether to proceed or commit first.

### Step 2: Determine PR Parameters

1. **JIRA ticket**: from arguments, branch name (`cbrd-XXXXX`/`CBRD-XXXXX`), or ask.
2. **Base branch**: if not specified, detect:
   - `feat/oos*` branches → base `feat/oos`
   - `CBRD-*` branches → base `develop`
   - `cubvec/*` branches → base `cubvec/cubvec`
   - Otherwise ask.
3. **Target repo**: default `CUBRID/CUBRID`.
4. **Source**: user's fork remote (typically `xmilex` for `xmilex-git/cubrid`). Head ref format: `<github-user>:<branch>`.

### Step 3: Analyze Changes

1. `git fetch <upstream-remote> <base-branch>`
2. `git log --oneline <upstream>/<base>..HEAD`
3. `git diff <upstream>/<base>...HEAD --stat`, then read the full diff.
4. If a JIRA ticket was identified, fetch context with `/jira CBRD-XXXXX`.

### Step 4: Draft PR Content

1. Read [tone_guide.md](./tone_guide.md) and match the example closest in scale to this diff.
2. Draft title + body. Write only what the diff supports — nothing speculative.
3. Run the tone_guide self-check.
4. **Show the draft to the user and get confirmation before creating.** Single pass; revise only on user feedback. Run `/grill-with-docs` only if the user explicitly asks for it.

### Step 5: Push and Create PR

1. Push: `git push <fork-remote> <branch> -u` (skip if already pushed).
2. Create:
   ```bash
   gh pr create --repo CUBRID/CUBRID \
     --draft \
     --base <base-branch> \
     --head <user>:<branch> \
     --assignee xmilex-git \
     --title "[CBRD-XXXXX] Title" \
     --body "$(cat <<'EOF'
   <http://jira.cubrid.org/browse/CBRD-XXXXX>

   ### Purpose

   한국어 산문...
   EOF
   )"
   ```
3. Print the resulting PR URL.

## Tips

- If a PR already exists for the branch, show it instead of creating a duplicate.
- For multi-commit PRs, summarize the overall change rather than listing each commit message.
- Always use heredoc for the body to handle multi-line Korean text correctly.
- Style reference corpus: [`pr-corpus/`](./pr-corpus/) — 76 hand-written PRs (#4432–#6911), one file per PR with diff summary + verbatim body.
