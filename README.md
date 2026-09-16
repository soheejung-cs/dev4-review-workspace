# dev4-review-workspace — 리뷰 스킬·참고 문서 정본

`claude-workspace` 에서 **리뷰에 관한 것만** 떼어낸 리포다(2026-09-16). 프로젝트 문서·메모리·호스트 규칙은 그대로
`claude-workspace` 에 있고, 소스 지식은 `dev4-ai-source` 에 있다. 세 리포의 역할:

| 리포 | 무엇 |
|---|---|
| `claude-workspace` | 일하는 방식(메모리 rules/howto), 프로젝트 문서(`projects/CBRD-*`), 호스트별 CLAUDE.md 조각 |
| **`dev4-review-workspace`** (여기) | **리뷰할 때 여는 스킬과 참고 문서** |
| `dev4-ai-source` | CUBRID 소스 분석 지식(모듈별 §1~§4) + 소스 학습·기록 규약 |

```
skills/
  code-review/       구현 리뷰 — 성능규칙집 18장 진입, 소스 지식 대조, 재현→수정→재검증
  design-review/     설계 리뷰 — 영역 판정 4단계, 계약 3요소, 대안표, 덱 구조, 결정 요청
  review-response/   리뷰 대응 — 스레드 수집, 재현, 답글 초안(사용자 검토→게시), 봇 resolve, TC PR 답글
  tc-analysis/       CI 실패 TC 3분류(출력차·코어·미상) — 코드 안 읽고 보고, 승인 후 출력차만 정렬
  gha-ci/            gha-ci 실패 분석 — collect 요약·failed.list·샤드 test-shell.xml, 코어는 아티팩트 서버(192.168.1.48:30080)+같은 SHA debug 빌드로 해석
  record/            기록 규약 — 무엇을 어디에 어떤 형식으로 (판단표)
  review-board/      리뷰 보드 갱신 — review-to-do 웹 보드(8827) 생성·호스팅, 추적 ID 관리
references/
  C-Cpp-성능규칙집.md        2568줄, 규칙 ID 로 인용. ~/.claude/성능규칙집.md 는 여기로의 심링크
  cubrid-사전설계문서.md      설계 리뷰 근거 — 불변조건 5·컴포넌트 [A]~[I]·의존·동시성 도메인
  대규모Cpp-물리설계-Lakos.md  PHYS 축 배경
  저지연패턴-HFT논문.md / -서적.md, 서버사이드-성능검토.md
members/             팀 명단(GitHub 기준)·가입 템플릿·사람별 공간(컨테이너 사실은 여기에만)
tools/               roster.json(추적 GitHub ID) · review_board_gen.py(review-to-do 보드)
examples/
  설계리뷰-덱-CBRD-27369.html   13장 덱(비유·락 전후 표·라이브락 그림·결정 요청) — 다음 덱의 틀
  리뷰답글-예시-PR7900.md      엔진 스레드 5 + TC PR 답글 2 — 문체·구조 예시
```

## 사용
- 리뷰 요청이 오면 `skills/*/SKILL.md` 중 해당 것을 **먼저 읽고** 그 순서대로 한다. 스킬은 서로 가리킨다
  (code-review → design-review 신호, → review-response 게시, → record 기록).
- 각 컨테이너: `git clone https://github.com/soheejung-cs/dev4-review-workspace ~/dev/docs/dev4-review-workspace`
  후 `ln -sfn ~/dev/docs/dev4-review-workspace/references/C-Cpp-성능규칙집.md ~/.claude/성능규칙집.md`.
- Claude Code 스킬로 등록하려면 `~/.claude/skills/<name>` → `skills/<name>` 심링크(선택).

## 규칙 변경
이 리포에 커밋 → 다른 컨테이너 pull. 리뷰 방식에 대한 사용자 지시는 해당 SKILL.md 에 **날짜와 함께** 적는다.
