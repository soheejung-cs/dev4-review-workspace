# dev4-review-workspace — 리뷰 스킬·참고 문서 정본

> **팀원 안내(사용법 + 하네스 제작 과정): [팀-안내.md](팀-안내.md)**

`claude-workspace` 에서 **리뷰에 관한 것만** 떼어낸 리포다(2026-09-16). 프로젝트 문서·메모리·호스트 규칙은 그대로
`claude-workspace` 에 있고, 소스 지식은 `dev4-ai-source` 에 있다. 세 리포의 역할:

| 리포 | 무엇 |
|---|---|
| `claude-workspace` | 일하는 방식(메모리 rules/howto), 프로젝트 문서(`projects/CBRD-*`), 호스트별 CLAUDE.md 조각 |
| **`dev4-review-workspace`** (여기) | **리뷰할 때 여는 스킬과 참고 문서** |
| `dev4-ai-source` | CUBRID 소스 분석 지식(모듈별 §1~§4) + 소스 학습·기록 규약 |

```
harness/             설계용·리뷰용 하네스(pipelines/*.yaml, schemas/finding.json, ARCHITECTURE.md, META-REVIEW.md); 구현 tools/harness/
REVIEW-절차.md       접수→분류→읽기→구현/설계 리뷰→돌릴 것→CI/TC→판정→답글→기록 (스킬을 순서로 잇는 표)
rules/
  성능-리뷰-규칙.md   성능을 볼 때 여는 한 문서 — 레퍼런스 요약(ID 보존), 벤치 선택표, 측정 계약
  설계-리뷰-규칙.md   설계를 볼 때 여는 한 문서 — 불변조건 5·컴포넌트·의존·동시성 등급·산출물 형식
skills/
  code-review/       구현 리뷰 — 성능규칙집 18장 진입, 소스 지식 대조, 재현→수정→재검증
  design-review/     설계 리뷰 — 영역 판정 4단계, 계약 3요소, 대안표, 덱 구조, 결정 요청
  review-response/   리뷰 대응 — 스레드 수집, 재현, 답글 초안(사용자 검토→게시), 봇 resolve, TC PR 답글
  tc-analysis/       → dev4-tc-workspace 로 분리(포인터)
  (구)       CI 실패 TC 3분류(출력차·코어·미상) — 코드 안 읽고 보고, 승인 후 출력차만 정렬
  gha-ci/            → dev4-tc-workspace 로 분리(포인터)
  (구)            gha-ci 실패 분석 — collect 요약·failed.list·샤드 test-shell.xml, 코어는 아티팩트 서버(192.168.1.48:30080)+같은 SHA debug 빌드로 해석
  record/            기록 규약 — 무엇을 어디에 어떤 형식으로 (판단표)
  review-testing/    리뷰 검증 규율 — 변경 유형별 CTP/동시성/JOB/TPC-H 선택, 컨테이너, 측정 계약
  oracle-reference/  Oracle 스펙 비교 — 요청 시에만, toc.md 매핑 후 해당 절만 읽기
  harness-review/    **/harness-review <PR>** — 통합 하네스 슬래시 명령(run full → findings → 판정 → report)
  harness-implement/ **/harness-implement <CBRD-n>** — 구현 하네스(implement_pack → plan.json 승인 → 구현 → implement_gate → 자기 리뷰 → PR)
  design-doc/        **/design-doc <CBRD-n|증상>** — 증상 인터뷰 5라운드로 설계문서를 함께 쓴다(영역 판정·대안표·결정 Q)
  review-board/      리뷰 보드 갱신 — review-to-do 웹 보드(8827) 생성·호스팅, 추적 ID 관리
  review-recommend/  리뷰어 추천 — 머지 PR 모듈 관심도 × 난이도(1/2/3명, 학습 슬롯) × 보드 부하 (tools/review_recommend.py)
references/
  oracle-manual/     → private 리포 dev4-oracle-manual 포인터(Oracle 문서는 재배포 제한). 읽는 규약 skills/oracle-reference

  C-Cpp-성능규칙집.md        2568줄, 규칙 ID 로 인용. ~/.claude/성능규칙집.md 는 여기로의 심링크
  cubrid-사전설계문서.md      설계 리뷰 근거 — 불변조건 5·컴포넌트 [A]~[I]·의존·동시성 도메인
  대규모Cpp-물리설계-Lakos.md  PHYS 축 배경
  저지연패턴-HFT논문.md / -서적.md, 서버사이드-성능검토.md
members/             팀 명단(GitHub 기준)·가입 템플릿·사람별 공간(컨테이너 사실은 여기에만)
tools/               roster.json(추적 GitHub ID) · review_board_gen.py(review-to-do 보드)
imports/xmilex-git/    송일한 워크스페이스에서 들여온 원문(PR 리뷰 스킬·영문 성능 규칙집·설계 어휘·ctp-run·PR 코퍼스) — README 에 대응표
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
- **`compile_commands.json` 을 만들어 둔다(권장).** 없으면 하네스의 `-fsyntax-only` 게이트가 **조용히 건너뛴다**
  (`self_check: ran=False`, `implement_gate` 의 `syntax: skipped`). 즉 "게이트 OK" 가 컴파일을 확인했다는 뜻이 아니게 된다.
  CMake 가 Ninja 제너레이터면 **재빌드 없이 0.1초**에 만들 수 있다(2026-09-21 실측: 1,233 엔트리 3.7MB, 0.066초):
  ```bash
  cd <빌드트리>   # 예: ~/dev/build/build_x86_64_release
  RULES=$(grep '^rule ' CMakeFiles/rules.ninja | awk '{print $2}' | grep -E '^(C|CXX)_COMPILER__')
  ninja -t compdb $RULES > compile_commands.json
  ```
  다른 경로에 두려면 `export HARNESS_COMPILE_COMMANDS=<경로>/compile_commands.json`.
  (`CMAKE_EXPORT_COMPILE_COMMANDS=ON` 으로 다시 구성해도 되지만 그쪽은 재구성 비용이 든다.)
- **산출물 디렉터리를 알려준다(권장).** 리뷰 산출물(`projects/<JIRA>/repro/` 등)은 사람마다 다른 리포에 둔다.
  `export DEV4_RECORDS_ROOT=<내 워크스페이스>` 또는 `tools/roster.json` 의 `records_root` 에 적는다.
  **설정하지 않으면** 하네스는 repro 의무를 *평가하지 않고* blocking 을 그대로 둔다(로그에 한 줄 남는다).
  예전에는 이 경로가 없으면 모든 blocking 이 조용히 non-blocking 으로 강등됐다 — 그래서 고쳤다(2026-09-21).

## 규칙 변경
이 리포에 커밋 → 다른 컨테이너 pull. 리뷰 방식에 대한 사용자 지시는 해당 SKILL.md 에 **날짜와 함께** 적는다.

- `tests/shell-tc-index/` shell TC 분류·인덱스 — 생성 `tools/tc_index_build.py`, 판정 `tools/tc_relevance.py --pr <n> --failed <케이스…>`
