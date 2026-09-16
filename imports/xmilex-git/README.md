# imports/xmilex-git — 송일한(xmilex-git) 워크스페이스에서 들여온 **방법론**

출처: https://github.com/xmilex-git/workspace (public) @ `676531645` (2026-09-16) · 들여온 날 2026-09-16 (정소희 세션). **원 작성자 xmilex-git, 원문 무수정.**
방침(사용자 지시 2026-09-16): **방법론만 들여온다.** PR 본문 코퍼스·특정 PR 리뷰 보고서·환경 전용 절차(빌드·deps·backport)·에이전트 설정(`agents/openai.yaml`)·측정 데이터는 들여오지 않는다(원 리포에서 본다). 성능 규칙집 영문 분할판(`cpp-perf-rules`)은 `references/C-Cpp-성능규칙집.md` 와 같은 내용이라 제외.

| 디렉터리 | 방법론 | 우리 쪽 대응 |
|---|---|---|
| `skills/cubrid-pr-review/` | 한국어 리뷰 보고서 형식(TL;DR / Summary / Findings Blocking·Non-blocking / 확인 필요) + voice-guide + reference | `skills/code-review` 보고서 형식, `skills/review-response` 어조 |
| `skills/code-review/` | 2축 리뷰(Standards / Spec) | `skills/code-review` §2 |
| `skills/codebase-design/` | 깊은 모듈·seam·adapter, DESIGN-IT-TWICE, DEEPENING | `rules/설계-리뷰-규칙.md` §3 어휘 |
| `skills/diagnosing-bugs/` | 어려운 버그·성능 회귀 진단 루프(피드백 루프 먼저) + hitl 템플릿 | `skills/code-review` §3 "재현이 먼저" |
| `skills/ctp-run/` | 컨테이너(rootless podman) 안 CTP 실행·샤딩·CI 실패분 재현 절차 + 스크립트 | `skills/review-testing` §2 |
| `skills/cubrid-pr-create/` | PR 본문 작성법 + tone_guide (코퍼스 제외) | PR브랜치-규칙 §3 |
| `skills/cubrid-jira-issue-write/` | JIRA 이슈 작성법·첨부 템플릿·어조 | `howto/JIRA-API-사용법` 보완 |

우리 규약에 녹일 때는 해당 SKILL.md 에 "출처: imports/xmilex-git/…" 을 남긴다. 다시 들여올 때 이 README 의 리비전을 올린다.
