# 리뷰 하네스 서브에이전트 (2026-10-08)

`~/.claude/agents/<name>.md` 로 링크된다(`tools/agent_link.sh`, 새 세션부터 `Agent(subagent_type=…)` 로 호출). 등급·추론 강도는 [`harness/모델-선택.md`](../harness/모델-선택.md) — 수집은 낮추고 판정·종합·반증은 유지, 애매하면 `needs_judgment` 로 올려보낸다. **메인 루프(사용자와 대화·승인·게시)는 에이전트에 맡기지 않는다.**

| 에이전트 | 등급 | 하는 일 | 입력 → 출력(파일) | 쓰는 스킬 |
|---|---|---|---|---|
| `harness-pack-reader` | 수집(low) | 배치 그룹의 사실만(함수·관문 짝·불변조건 적중·미지 심볼·질문) | `review_request.batch*.md` → `observations.<g>.json` | — |
| `harness-reviewer` | 판정 | finding 스키마의 지적(why·proposal·verification·evidence·rule_ids) | preamble + 배치 (+observations) → `findings.<g>.json` | code-review · design-review |
| `harness-refuter` | 반증(high) | 지적을 무너뜨린다 — anchor·심볼·규칙 ID·확인 결과·더 단순한 설명 | findings → `refute.<g>.json` | review-response(반증 비평) |
| `harness-synthesizer` | 종합(high) | 합치기·중복 제거·중요도 정렬 → `findings.json`·코멘트 초안·보고서 §0 | findings+refute → `findings.json`, `comments.draft.md`, `report.head.md` | review-response · 리뷰보고서-요청자관점 |
| `harness-repro` | 판정 | 재현 스크립트를 develop/PR 빌드에 SA 로 돌려 회귀·스펙 변경·develop 결함 수정을 가른다 | SQL/모양 → `result.md` + 세트·출력 | sql-difftest · review-testing |
| `harness-threads` | 수집(low) | 스레드 수집·분류, 작성자 주장 인용 | PR → `threads.md/json` | review-response §1 |
| **`harness-gatekeeper`** | 감시(high) | **게시 전** 산출물 하나를 규칙 원문과 대조 — 차단급이면 게시 보류 | 초안/답글/findings → `gate.*.md` | review-response · code-review · sql-difftest · CLAUDE.md 규칙 |
| **`harness-auditor`** | 감시 | **작업 끝** 절차 점검 — 기록·배운것·보드·git·환경 복원·미응답 스레드 | 프로젝트 디렉터리+PR → `audit.*.md` | record · pre-clear · review-response §6·§7 |

## 흐름

```
harness-review   : run full → pack-reader(그룹별, 4개씩) → reviewer(그룹별) → refuter → synthesizer → run full --findings
                   → gatekeeper(comments.draft) → [메인: 사용자 승인] → 게시 → auditor
review-response  : threads → (결함 주장이면) repro → [메인: 답글 초안] → gatekeeper(답글) → [승인] → 게시 → auditor
sql-difftest     : repro(세트별) → [메인: 분류 확인] → gatekeeper(지적 초안) → 게시 → auditor
harness-implement: (구현은 메인) → reviewer(자기 리뷰) → refuter → gatekeeper(PR 본문) → auditor
```

## 왜 싱글 세션보다 나은가 — 2026-10-07~08 PR#8022 의 네 가지 실수가 근거
재현 절차에서 ALTER 단계를 뺀 채 게시(→ 작성자 "재현 안 됨"), 테스트 반복 루프에서 건마다 확인 퀴즈, 영어 응답, 2,000바인드·10,000항 스트레스로 컨테이너 메모리 56GiB. 넷 다 **이미 적혀 있던 규칙**을 산출물을 쓴 세션이 자기 산출물에 적용하지 못한 것이다. 규칙과 산출물만 보는 감시자는 결론에 지분이 없어 이런 것을 잡는다. 단, 에이전트는 이 대화를 보지 못하므로 **입력은 파일 경로로** 넘기고, 결과도 파일로 받는다.

## 한계
- 에이전트 정의의 `effort:` 는 모델-선택 등급을 적은 것이다. 호스트가 그 필드를 무시하면 세션 기본값으로 돈다.
- 동시 4개 이하(모델-선택 「한도」). 배치 ↔ 그룹은 `batch_index.md` 의 제안대로.
- 감시자는 규칙 위반을 잡지, 놓친 지적(recall)은 못 잡는다 — 그건 META-REVIEW 의 「놓친 것」 표.
