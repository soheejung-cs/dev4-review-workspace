#!/bin/bash
# headless_review.sh — 리뷰 하네스를 대화 없이 돌린다 (claude -p). 산출물만 만들고 게시하지 않는다.
#   headless_review.sh <PR> [--model <m>] [--effort <e>] [--max-turns N] [--dry]
#   산출: $OUT/<PR>-<ts>.json (claude 결과 JSON: result·usage·total_cost_usd·modelUsage)
#         $OUT/<PR>-<ts>.log  (stderr)
#         $OUT/cost.jsonl      (1줄 append: ts·pr·cost·turns·duration·모델별 토큰) — 보드 8827 이 읽는다
#   하네스 자체의 산출물(review_request.md·findings.json·report.md)은 /harness-review 스킬이 정한 자리에 그대로 생긴다.
#   게시 금지는 두 겹: (1) 스킬이 "사용자 승인 후 게시" 구조 (2) --disallowedTools 로 review_reply.py·gh pr comment/review·gh api -X 차단.
set -u
PR=${1:-}; shift || { sed -n '2,9p' "$0"; exit 1; }
[[ "$PR" =~ ^[0-9]+$ ]] || { echo "PR 번호가 아니다: $PR" >&2; exit 1; }
MODEL=""; EFFORT=""; MAXT=200; DRY=0
while [ $# -gt 0 ]; do case $1 in
    --model) MODEL=$2; shift 2;; --effort) EFFORT=$2; shift 2;; --max-turns) MAXT=$2; shift 2;; --dry) DRY=1; shift;;
    *) echo "모르는 옵션 $1" >&2; exit 1;; esac; done
HERE=$(cd "$(dirname "$0")/.." && pwd)                      # dev4-review-workspace
OUT=${HEADLESS_OUT:-$HOME/dev/utils/review-board/site/headless}; mkdir -p "$OUT"
ts=$(date +%Y%m%d-%H%M%S); base="$OUT/$PR-$ts"
SYS="헤드리스 실행이다(사용자가 대화에 없다). /harness-review 를 끝까지 돌려 findings.json·report.md·comments.draft.md 까지 만들되 \
GitHub 에 아무것도 게시하지 않는다(review_reply.py·gh pr comment/review·gh api POST/PATCH 금지). 사용자에게 묻는 대신 보고서의 '결정 Q' 로 남긴다. \
마지막 메시지는 산출물 경로 목록 + 지적 수(🔴/🟡/🟢) 한 줄로 끝낸다."
args=(-p "/harness-review $PR" --output-format json --max-turns "$MAXT"
      --append-system-prompt "$SYS"
      --allowedTools "Read" "Grep" "Glob" "Write" "Edit" "Agent" "Skill" "Bash(python3 *)" "Bash(gh pr view*)" "Bash(gh pr diff*)" "Bash(gh pr checks*)" "Bash(gh api graphql*)" "Bash(gh api repos/*)" "Bash(git *)" "Bash(ls *)" "Bash(cat *)" "Bash(grep *)" "Bash(mkdir *)" "Bash(diff *)"
      --disallowedTools "Bash(python3 *review_reply.py*)" "Bash(gh pr comment*)" "Bash(gh pr review*)" "Bash(gh pr ready*)" "Bash(gh pr merge*)" "Bash(gh api * -X *)" "Bash(gh api * --method *)" "Bash(gh api * -f *)" "Bash(gh api * -F *)" "Bash(git push*)")
[ -n "$MODEL" ] && args+=(--model "$MODEL"); [ -n "$EFFORT" ] && args+=(--effort "$EFFORT")
if [ $DRY = 1 ]; then printf 'cd %s && claude' "$HERE"; printf ' %q' "${args[@]}"; echo; exit 0; fi
cd "$HERE" || exit 1
echo "$(date '+%F %T') headless review PR#$PR start → $base.json" | tee -a "$OUT/.run.log"
claude "${args[@]}" > "$base.json" 2> "$base.log"; rc=$?
python3 - "$base.json" "$PR" "$OUT/cost.jsonl" "$rc" <<'EOF'
import json, sys, datetime
f, pr, costf, rc = sys.argv[1], int(sys.argv[2]), sys.argv[3], int(sys.argv[4])
try: d = json.load(open(f))
except Exception as e: d = {'is_error': True, 'result': 'no json: %s' % e}
mu = d.get('modelUsage') or {}
row = {'ts': datetime.datetime.now().strftime('%Y-%m-%d %H:%M'), 'pr': pr, 'rc': rc, 'error': bool(d.get('is_error')),
       'cost_usd': round(d.get('total_cost_usd') or 0, 4), 'turns': d.get('num_turns'), 'duration_s': round((d.get('duration_ms') or 0) / 1000),
       'models': {m: {k: v for k, v in u.items() if k in ('inputTokens', 'outputTokens', 'cacheReadInputTokens', 'cacheCreationInputTokens', 'costUSD')} for m, u in mu.items()},
       'session_id': d.get('session_id'), 'json': f.split('/')[-1], 'result_tail': (d.get('result') or '')[-300:]}
open(costf, 'a', encoding='utf-8').write(json.dumps(row, ensure_ascii=False) + '\n')
print('PR#%d rc=%d error=%s cost=$%.4f turns=%s %ss' % (pr, rc, row['error'], row['cost_usd'], row['turns'], row['duration_s']))
print((d.get('result') or '')[-600:])
EOF
echo "$(date '+%F %T') headless review PR#$PR end rc=$rc" >> "$OUT/.run.log"
exit $rc
