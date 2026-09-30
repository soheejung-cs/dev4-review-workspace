#!/bin/bash
# develop 기반 goto 캐시 후보 — A/B 의 REF 로 쓸 만한 설치본을 근거와 함께 보여 준다.
# 판정: BUILD_REV 의 리비전이 upstream/develop 의 조상이면 "develop 그대로"; 아니면 머지베이스 이후 바뀐 src/ 디렉터리를 보여 준다
# (src/optimizer 만이면 실행 의미가 develop 과 같은 후보 — 플랜만 다를 수 있으니 결과 정합 비교엔 쓸 수 있다).
SRC=${SRC:-$HOME/dev/sources/cubrid}
git -C "$SRC" fetch -q upstream develop 2>/dev/null
printf '%-28s %-10s %-10s %s\n' 설치본 rev develop조상 "머지베이스 이후 바뀐 디렉터리(src/)"
for d in $HOME/release/CUBRID-goto-*/; do
  n=$(basename "$d"); [ -x "$d/bin/cub_server" ] || { printf '%-28s %s\n' "$n" "(cub_server 없음 — 깨진 빌드)"; continue; }
  rev=$(sed -n 's/^빌드 리비전: \([0-9a-f]*\).*/\1/p' "$d/BUILD_REV" | head -1); [ -n "$rev" ] || rev=$(echo "$n" | sed 's/CUBRID-goto-//')
  if git -C "$SRC" merge-base --is-ancestor "$rev" upstream/develop 2>/dev/null; then printf '%-28s %-10s %-10s %s\n' "$n" "$rev" yes "-"
  else mb=$(git -C "$SRC" merge-base "$rev" upstream/develop 2>/dev/null)
    dirs=$(git -C "$SRC" diff --name-only "$mb" "$rev" 2>/dev/null | grep '^src/' | cut -d/ -f1-2 | sort -u | tr '\n' ' ')
    printf '%-28s %-10s %-10s %s\n' "$n" "$rev" no "${dirs:-(리비전 미보유 — fetch 필요)}"; fi
done
