#!/bin/bash
# review_watch.sh — 보드(board.json)에 새로 들어온 엔진 PR 에 헤드리스 리뷰를 건다. 기본은 꺼짐.
#   review_watch.sh            한 번 훑고 끝 (REVIEW_WATCH_ENABLE=1 이 아니면 계획만 출력)
#   review_watch.sh --daemon N N분마다 (nohup 으로)
#   REVIEW_WATCH_ENABLE=1      실제로 돌린다        REVIEW_WATCH_MAX=2   한 번에 최대 PR 수(토큰 보호)
#   REVIEW_WATCH_SKIP_AUTHORS  쉼표 목록 — 기본 roster.json agent_github(= 내 PR 은 자동 리뷰하지 않는다; .51 규칙은 "내 PR 도 타인 PR" 이지만 자동은 비용이라 제외)
#   상태: $OUT/.seen (PR 번호 한 줄씩). 이미 본 PR 은 헤드 커밋이 바뀌어도 다시 돌리지 않는다 — 재리뷰는 headless_review.sh <PR> 수동.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BOARD=${BOARD_JSON:-$HOME/dev/utils/review-board/site/board.json}
OUT=${HEADLESS_OUT:-$HOME/dev/utils/review-board/site/headless}; mkdir -p "$OUT"; touch "$OUT/.seen"
ENABLE=${REVIEW_WATCH_ENABLE:-0}; MAX=${REVIEW_WATCH_MAX:-2}
SKIP=${REVIEW_WATCH_SKIP_AUTHORS:-$(python3 -c 'import json;print(",".join(json.load(open("'"$HERE"'/roster.json")).get("agent_github",[])))')}
once () {
    [ -f "$BOARD" ] || { echo "board.json 없음: $BOARD"; return; }
    python3 - "$BOARD" "$OUT/.seen" "$SKIP" <<'EOF' | head -n "$MAX" > "$OUT/.todo"
import json, sys
b = json.load(open(sys.argv[1])); seen = set(l.strip() for l in open(sys.argv[2])); skip = set(filter(None, sys.argv[3].split(',')))
for r in sorted(b['prs'], key=lambda r: r['created']):
    if r['repo'] != 'CUBRID/cubrid' or str(r['number']) in seen or r['author'] in skip: continue
    if r.get('agent_n'): continue                     # 이 에이전트가 이미 리뷰를 남긴 PR
    print(r['number'], r['author'], r['title'][:70], sep='\t')
EOF
    if [ ! -s "$OUT/.todo" ]; then echo "$(date '+%F %T') watch: 새 PR 없음"; return; fi
    while IFS=$'\t' read -r n a t; do
        if [ "$ENABLE" != 1 ]; then echo "(계획) PR#$n $a — $t  → REVIEW_WATCH_ENABLE=1 이면 headless_review.sh $n"; continue; fi
        echo "$n" >> "$OUT/.seen"
        bash "$HERE/headless_review.sh" "$n" >> "$OUT/.watch.log" 2>&1
    done < "$OUT/.todo"
}
once
if [ "${1:-}" = "--daemon" ]; then while sleep "$(( ${2:-30} * 60 ))"; do once; done; fi
