#!/bin/bash
# 서브에이전트 등록 — ~/.claude/agents/<name>.md → dev4-review-workspace/agents/<name>.md 심링크. 새 세션부터 Agent 도구의 subagent_type 으로 보인다.
set -u
mkdir -p ~/.claude/agents
for f in ~/dev/docs/dev4-review-workspace/agents/*.md; do ln -sfn "$f" ~/.claude/agents/"$(basename "$f")"; done
echo "agents: $(ls ~/.claude/agents | sed 's/\.md$//' | paste -sd' ' -)"; find ~/.claude/agents -xtype l -print | sed 's/^/dangling: /'
