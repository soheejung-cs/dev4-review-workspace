# dev4-review-workspace — 리뷰 스킬·참고 문서 정본

> **팀원 안내(사용법 + 하네스 제작 과정): [팀-안내.md](팀-안내.md)**

`claude-workspace` 에서 **리뷰에 관한 것만** 떼어낸 리포다(2026-09-16). 프로젝트 문서·메모리·호스트 규칙은 그대로
`claude-workspace` 에 있고, 소스 지식은 `dev4-ai-source` 에 있다. 세 리포의 역할:

| 리포 | 무엇 |
|---|---|
| `claude-workspace` | 일하는 방식(메모리 rules/howto), 프로젝트 문서(`projects/CBRD-*`), 호스트별 CLAUDE.md 조각 |
| **`dev4-review-workspace`** (여기) | **리뷰할 때 여는 스킬과 참고 문서** |
| `dev4-ai-source` | CUBRID 소스 분석 지식(모듈별 §1~§4) + 소스 학습·기록 규약 |

## 이 리포에 든 스킬 — 무엇을 언제 쓰나

스킬 **11개**(각각 `skills/<이름>/SKILL.md`)와, 다른 리포로 옮겨 **포인터만 남은 것 2개**. 스킬을 등록하면 Claude Code 세션에서 `/<이름>` 으로 부를 수 있고, 그중 **인자를 받는 4개**는 아래 「명령」 칸에 인자 형태를 적었다. 나머지는 자연어로 열거나(`"리뷰해줘"`) 인자 없이 `/<이름>` 으로 연다.

한 PR 을 처음부터 끝까지 어떤 순서로 도는지는 **[REVIEW-절차.md](REVIEW-절차.md)** 의 0~9단계 표를 본다. 아래는 그 표에서 쓰이는 도구의 목록이다.

### 리뷰 — PR 을 볼 때

| 스킬 | 무엇을 하나 | 언제 여나 | 명령 |
|---|---|---|---|
| **code-review** | 구현 리뷰: 정확한가 · 핫패스에서 낭비하나 · 이미 아는 함정인가. 재현 → 원인 → 검증 순서, 규칙은 ID 로 인용 | C/C++ 변경 PR 을 받았을 때. 코드를 쓰기 전 체크리스트를 훑을 때 | `/code-review` |
| **design-review** | 설계 리뷰: 변경이 **있어야 할 자리에** 있나 — 불변조건·컴포넌트 책임·의존 방향·공유/격리 경계. 설계 덱 작성 포함 | 모듈 경계·락·MVCC·새 데몬을 건드릴 때. `rules/설계-리뷰-규칙.md` §0 신호가 하나라도 걸릴 때 | `/design-review` |
| **review-testing** | 이 PR 에 **무엇을 돌릴지** 결정: CTP sql/medium/shell · 동시성 재현 · JOB · TPC-H, 그리고 어느 계약으로 재고 무엇으로 판정하나 | "테스트 뭐 돌려야 해?" 일 때. 검증 없이 승인하려 할 때 | `/review-testing` |
| **review-response** | 리뷰 코멘트 대응: 스레드 수집 → 결함이면 재현·수정·재검증 → 답글 초안 → **사용자 검토 후 게시**, 봇만 resolve | 내 PR 에 코멘트가 달렸을 때. 타인 PR 에 지적을 게시할 때 | `/review-response` |
| **oracle-reference** | Oracle 스펙·동작 대조 — 목차(`toc.md`)로 해당 절만 찍어 읽고 CUBRID 와 비교 | **요청이 있을 때만.** 표준/Oracle 호환 여부가 쟁점일 때 | `/oracle-reference` |

### 하네스 — 도구가 컨텍스트를 고르고 판정한다

| 스킬 | 무엇을 하나 | 언제 여나 | 명령 |
|---|---|---|---|
| **harness-review** | 통합 리뷰 하네스: `review_request*.md`(코드 그래프·도달성·불변조건·성능 규칙) 생성 → LLM 이 `findings.json` → 스키마·앵커·근거·규칙 ID 를 기계가 판정 → `report.md`·episodic | PR 리뷰의 **기본 진입점**. 사람이 하는 건 게시 승인뿐 | `/harness-review <PR번호>` |
| **harness-implement** | 구현 하네스: 후보 함수 팩 → `plan.json`(**승인 지점**) → 구현 → 게이트(범위·codestyle·`-fsyntax-only`·관문 짝) → 자기 리뷰 → draft PR(**승인 지점**) | 이슈를 직접 고칠 때. 작은 버그는 설계문서 없이 바로 plan 으로 | `/harness-implement <CBRD-n>` |
| **design-doc** | 증상 인터뷰(닫힌 질문 2~3개씩)로 사용자와 **함께** 설계문서를 쓴다 — 영역 판정 4단계 → 재현·기대 동작 → 대안표 → 결정 요청 Q | 고치기 전에 "어디를 고쳐야 하나" 부터 정해야 할 때 | `/design-doc <CBRD-n \| 증상 한 줄>` |

하네스의 층별 구현과 실행 명령은 **[harness/README.md](harness/README.md)**, 만든 과정과 채점은 **[팀-안내.md](팀-안내.md) §2** · `harness/META-REVIEW.md`.

### 보드·추천 — 무엇을 먼저 볼지 정할 때

| 스킬 | 무엇을 하나 | 언제 여나 | 명령 |
|---|---|---|---|
| **review-board** | review-to-do 웹 보드 재생성: 추적 GitHub ID 가 assignee 인 open·non-draft PR 의 미해결 스레드 수, 리뷰어 응답 상태, CI | 오늘 무엇을 리뷰할지 고를 때. 추적 ID 를 추가할 때 | `/review-board` ※1 |
| **review-recommend** | 리뷰어 추천: 최근 머지 PR 로 관심도, 변경 규모·영역으로 난이도(1/2/3명), 보드로 부하를 합쳐 표로 낸다(**추천만** — 지정은 사람이) | "누가 리뷰하면 좋을까", 리뷰 배정 균형을 볼 때 | `/review-recommend <PR>` |

### 기록 — 끝내기 전에

| 스킬 | 무엇을 하나 | 언제 여나 | 명령 |
|---|---|---|---|
| **record** | 세션에서 나온 것을 **한 발견은 한 곳에** 남기는 판단표 — 리뷰 산출물 / 소스 사실 / 일하는 방식 / 보드 | 작업이 끝났을 때, 컨텍스트가 길어져 넘겨야 할 때 | `/record` |

### 다른 리포로 간 것 (여기엔 포인터만)

`skills/` 아래에 디렉터리는 있지만 **`SKILL.md` 가 없다** — 본문은 private `dev4-tc-workspace` 에 있고, 등록 스크립트도 이 둘은 건너뛴다. 이 리포만 clone 한 사람에게는 해당 슬래시 명령이 없다.

| 스킬 | 본문 위치 | 무엇 |
|---|---|---|
| `skills/tc-analysis/` → 포인터 | `dev4-tc-workspace/skills/tc-analysis` | CI 실패 TC 3분류(출력차·코어·미상), 승인 후 출력차만 답안 정렬 |
| `skills/gha-ci/` → 포인터 | `dev4-tc-workspace/skills/gha-ci` | gha-ci 실패 분석 — collect 요약·failed.list·샤드 XML, 코어는 같은 SHA debug 빌드로 해석 |

### 등록 (안 하면 슬래시 명령이 없다)

스킬은 `~/.claude/skills/<이름>` 심링크가 있어야 세션에 보인다. **이 링크를 만들지 않으면 `/harness-review` 를 포함해 위 명령이 하나도 없다.**

```bash
mkdir -p ~/.claude/skills
for d in ~/dev/docs/dev4-review-workspace/skills/*/; do
  [ -f "$d/SKILL.md" ] && ln -sfn "${d%/}" ~/.claude/skills/"$(basename "$d")"
done
ls ~/.claude/skills            # 이 리포 것 11개가 들어 있어야 한다(다른 리포 스킬이 섞여 더 많을 수 있다)
```

### ※ 리포 밖 전제가 있는 스킬 (clone 만으로는 안 도는 것)

- **※1 review-board** — 보드 호스팅은 `.51` 의 `~/bin/review_board_publish.sh`·`~/bin/httpd_threaded.py` 로 도는데 **이 스크립트들은 어느 리포에도 없다.** 생성기(`tools/review_board_gen.py`)도 문서 링크 수집 경로가 `.51` 배치에 맞춰져 있다. 다른 컨테이너에서는 보드를 **읽기만** 한다(http://192.168.6.51:8827/).
- **harness-review / harness-implement** — 판정 단계 일부가 아직 owner 의 프라이빗 문서 리포 경로를 가정한다. 없으면 그 단계만 비고, 나머지는 돈다. tree-sitter `.so` 가 없으면 ctags 폴백(호출 그래프 없음).
- **oracle-reference** — 원문은 private `dev4-oracle-manual`(재배포 금지). 초대받지 않았으면 열 수 없다.
- **review-testing** — JOB·TPC-H 실행 하네스는 벤치 데이터를 가진 컨테이너에만 있다(JOB `.51`, TPC-H `.50`). 매트릭스로 **무엇을 돌릴지 판단**하는 데까지는 어디서나 쓸 수 있다.

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
  gha-ci/            → dev4-tc-workspace 로 분리(포인터)
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

