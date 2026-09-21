# 기여 규약 (dev4-review-workspace · dev4-ai-source 공통)

1. **이름을 남긴다.** 커밋 저자는 사람 이름(`git config user.name "<한글 이름>"`, 리포별 설정), 기록 항목·파일에는
   `이름(.컨테이너) 날짜` 서명. Claude 세션은 `Co-Authored-By:` 트레일러로만. 설정 절차: `members/_설정-템플릿.md`.
2. **한 발견은 한 곳에.** 어디에 둘지는 `skills/record/SKILL.md` 판단표. 소스 사실은 `dev4-ai-source/staging/<이름>-<이슈명>.md` 로.
3. **컨테이너·머신 고유 사실은 `members/<이름>/`** 에만. 공용 스킬·참고 문서에는 넣지 않는다.
   **`references/` 새 파일 규약**: ① 맨 앞에 **§0 진입표**(어떤 질문이 오면 어느 절) 필수 ② **250줄 상한** —
   넘으면 표를 `rules/` 로 옮긴다 ③ **인용 가능한 판정 행(`ID | 판정 문장`)은 `rules/` 에만** 둔다. 하네스가
   `references/` 를 읽지 않기 때문이고, 양쪽에 두면 갈라진다. 성능규칙집이 2568줄이 되어 "통째로 읽지 말 것"
   이라는 예외 규약을 세 곳에 반복해야 했던 것을 되풀이하지 않는다.
   ④ 저작권: 본문을 옮기지 않는다. 서지·공개된 목차·"어느 질문에 어느 절"·**우리 판단**만 쓴다.
   **코드도 마찬가지다** — `tools/` 의 스크립트에 개인 경로를 하드코딩하지 않는다. 밖에서 받아라
   (환경변수 또는 `tools/roster.json`). 2026-09-21 에 `pipeline.py` 가 `~/dev/docs/claude-workspace` 를
   박아 두고 있었고, 그 경로가 없는 환경에서는 **모든 blocking finding 이 아무 신호 없이 강등**됐다.
4. **규약을 바꾸면 그 줄에 이름과 지시 날짜.** 스킬(`skills/*/SKILL.md`)이 곧 규약이다 — 채팅에만 남긴 지시는 없는 것과 같다.
5. push 전에 `git pull --rebase`. 세 컨테이너·여러 사람이 같은 리포에 push 한다.
6. 프라이빗 리포다 — 초대는 `members/README.md` 명단 기준으로 owner(정소희)가 한다.
7. **번역하기 어려운 용어는 번역하지 않는다.** 락 모드(`SCH_S`, `IX`), MVCC/WAL/latch/vacuum/snapshot, 함수·구조체·에러 코드·파라미터 이름, PG/Oracle 고유 용어(`ShareUpdateExclusiveLock`, `recalc pool`, `DBMS_STATS`)는 **원문 그대로** 쓴다. 억지 번역(예: "공유 갱신 배타 잠금")은 검색도 안 되고 뜻도 흐려진다. 한국어 설명은 원문 뒤에 괄호로 붙인다. (사용자 지시 2026-09-16)
