# CUBRID JIRA Issue Tone and Output Guide

This guide is authoritative for wording, detail level, and markup. The target is the practical depth of CBRD-27041 for improvement work and CBRD-26799 for errors, not a code-level implementation or verification document.

## Audience and purpose

Write for QA, decision-makers, and developers. The body must let them understand:

- what is wrong or being changed;
- when and whom it affects;
- what behavior or result marks completion.

The body is not a private analysis note, code review walkthrough, or exhaustive test plan. Prefer observable behavior and product scope over code internals.

## JIRA wiki markup

The saved file is pasted directly into jira.cubrid.org. Use JIRA wiki markup, not Markdown.

| Need | Use | Do not use |
|---|---|---|
| Section label | `*Description*` | `# Description`, `## Description` |
| SQL block | `{code:sql} ... {code}` | fenced Markdown code block |
| Shell block | `{code:bash} ... {code}` | fenced Markdown code block |
| Plain output | `{noformat} ... {noformat}` | fenced Markdown code block |
| Bullet | `* item` | Markdown checkbox |
| Numbered step | `# item` | Markdown heading |

Use plain text for function names, parameters, paths, and other identifiers. Do not use Markdown backticks or JIRA `{{...}}` inline monospace.

The JIRA Summary/title is a separate field. Do not place a Markdown title or `[TAG]` title in the body. A proposed Summary may be reported separately.

## Detail boundary

Include:

- the relevant background and impact;
- externally visible behavior and specification;
- the minimal complete reproduction for an error;
- the central implementation concept and affected scope;
- a small set of observable acceptance results.

Exclude from the body unless a fact is essential to understanding the issue:

- function-by-function flows and call graphs;
- data-structure and algorithm walkthroughs;
- source file lists, line numbers, and change counts;
- backtraces and root-cause essays;
- exhaustive edge-case, regression, or unit-test plans;
- implementation alternatives and speculative design;
- repeated summaries of the same fact.

Do not require an explanation for every internal term. Avoid the term when plain Korean is enough. When an identifier is necessary for reproduction, result recognition, or accuracy, keep it as-is and add one short explanation only if readers need it.

## Korean style

- Write concise Korean in 평어(한다체).
- Prefer short paragraphs and natural sentences. Avoid AI-style labels, self-narration, and repeated `한다.` sentence rhythm.
- Do not add `Issue Triage`, `AI-Generated Context`, `Summary`, TL;DR, rationale scaffolding, or a tutorial.
- Do not repeat one requirement across `Specification Changes`, `Acceptance Criteria`, and `Definition of done`.
- Do not use emoji or non-BMP characters. Prefer plain ASCII punctuation with Korean text.
- Do not invent facts, thresholds, build versions, performance targets, test results, or implementation choices. Use `N/A` only where the template requires a section but there is no applicable content.

## Portable content

JIRA readers do not share the author's local tooling.

- Do not put `just <recipe>`, personal aliases, private helper functions, or local-only paths in the body.
- Use commands available in the CUBRID/CTP environment, such as `ctp.sh`, `cubrid`, `csql`, `make`, `cmake`, `bash`, or `sh`.
- If a local wrapper was used, write the portable underlying command instead.

## Final check

Before saving, verify:

1. The chosen template matches the work, including for a Sub-task.
2. The body uses JIRA wiki markup and contains no title line.
3. Implementation and verification detail stay within the limits in `SKILL.md`.
4. `Repro` is minimally complete for a Correct Error.
5. Specification and performance claims come from supplied evidence.
6. Empty `Additional Information` is omitted; absent specification changes are `N/A`.
7. The same fact is not repeated in multiple sections.
