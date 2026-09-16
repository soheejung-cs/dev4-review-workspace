# imports/xmilex-git — 송일한(xmilex-git) 워크스페이스에서 들여온 리뷰 자료

출처: https://github.com/xmilex-git/workspace (public) @ `676531645` (2026-09-16) · 들여온 날 2026-09-16 (정소희 세션)
**원 작성자는 xmilex-git 이다.** 여기 파일은 원문 그대로이고(수정 없음), 우리 규약에 녹일 때는 해당 SKILL.md 에 "출처: imports/xmilex-git/…" 을 남긴다.
원문 갱신은 원 리포에서 일어난다 — 다시 들여올 때 이 README 의 리비전을 올린다.

| 디렉터리 | 무엇 | 우리 쪽 대응·활용 |
|---|---|---|
| `skills/cubrid-pr-review/` | CUBRID PR 리뷰 → **한국어 리뷰 보고서**(TL;DR / Summary / Findings Blocking·Non-blocking / 확인 필요) + voice-guide·reference·scripts | `skills/code-review` 의 보고서 형식 후보. `review-reports/PR-7766`, `PR-7673` 이 실제 산출 예 |
| `skills/cpp-perf-rules/` | **성능 규칙집의 영문 장별 분할판** — CHECKLIST(§18)·PRIORITY(§19)·COSTS·MEASUREMENT·MEMORY-COHERENCY·… ID 동일 | `rules/성능-리뷰-규칙.md`(요약 규칙) 의 1차 재료. `references/C-Cpp-성능규칙집.md`(한국어 원문)와 ID 로 교차 인용 |
| `skills/codebase-design/` | 깊은 모듈·seam·adapter 어휘 (DESIGN-IT-TWICE, DEEPENING) | `skills/design-review` 의 "책임이 맞는 자리에 있나" 판정 어휘 |
| `skills/diagnosing-bugs/` | 어려운 버그·성능 회귀 진단 루프(피드백 루프 먼저) | `code-review` §3 "재현이 먼저" 의 확장판 |
| `skills/ctp-run/` | **컨테이너(rootless podman) 안에서 CTP 전 스위트 실행·샤딩·CI 실패분 재현** — 호스트 CTP 는 `pkill cub` 로 이 사용자 cub_* 전부를 죽인다 | 리뷰 검증 규율(`skills/review-testing`)에서 CTP 실행 권장 방식. justfile·podman 필요(원 리포) |
| `skills/cubrid-pr-create/` (+ pr-corpus 78) | PR 본문 작성 스킬 + 실제 PR 본문 코퍼스 | PR브랜치-규칙 §3 보완 |
| `skills/cubrid-backport/`, `cubrid-jira-issue-write/`, `cubrid-build/`, `cubrid-deps-check/`, `code-review/`(2축 리뷰: Standards/Spec) | 부속 스킬 | 참고 |
| `review-reports/` | PR-7673·PR-7766 리뷰 보고서·코멘트 원문 | 보고서 형식 예시 |

주의: 원 스킬은 OpenAI/Codex·Herdr·justfile 전제가 섞여 있다(`agents/openai.yaml`, `just ctp`). 우리 환경(Claude Code, goto, .50/.51/.52)에 맞추려면 SKILL.md 를 복제해 고치되 원문은 여기 그대로 둔다.
