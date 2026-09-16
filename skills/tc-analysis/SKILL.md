---
name: tc-analysis
description: CI 실패 TC 를 코드베이스를 읽지 않고 3분류(출력차·코어·미상)로 나눠 보고하고, 승인 후 출력차만 답안 정렬한다. 사용자가 'CI 실패 TC 봐줘', '답안 정렬', 'NOK 분류' 라고 할 때.
---

사용자 지시 2026-09-11(.50). **PR 구현 워크플로와 별건으로 도는 독립 작업**이다 —
여기엔 코드 리뷰도, 엔진 코드 수정도 들어가지 않는다.

## 0. 범위 계약 (먼저 읽는다)

| 한다 | 하지 않는다 |
|---|---|
| CI 실패 TC 를 수집해 **3분류**(A 출력차 / B 코어 / C 미상)로 나눈다 | 엔진 코드를 읽는다 (`~/dev/sources/cubrid/src/**`) |
| 분류 결과를 **사용자에게 보고하고 멈춘다** | 코드 리뷰·성능 판정 (그건 구현/리뷰 워크플로 몫) |
| 승인 후 **A 만** "출력을 맞추는" 방식으로 답안 정렬 | B·C 를 고친다 — **사용자 요청 전까지 손대지 않는다** |
| 재현이 필요한 케이스만 로컬 단건 실행 | A 를 코드 수정으로 해결하러 간다 |

**코드베이스를 읽지 않는 것이 이 워크플로의 핵심 제약이다** (사용자 지시: 그곳에 토큰을 쓰지 않는다).
읽는 것은 ① CI 산출물 ② `cubrid-testcases`/`-private-ex` 의 케이스·답안 파일, 이 둘뿐이다.
원인이 엔진에 있다고 판단되면 **그 사실만 보고**하고 분석은 별도 작업(=[[작업-생애주기]] §1-(B))으로 넘긴다.

## 1. T1 — 대상 확정과 짝 맞추기 (여기서 틀리면 나머지가 전부 무효)

```bash
gh pr checks <PR번호> --repo CUBRID/cubrid
```

현재 배치(2026-09-11 실측):

| 스위트 | 체크 이름 | 어디 |
|---|---|---|
| sql | `ci/circleci: test_sql` | CircleCI |
| medium | `ci/circleci: test_medium` | CircleCI |
| **shell** | **`gha-ci: test_shell`** | **GitHub Actions (50 샤드 + `collect`)** — CircleCI 에서 빠졌다 |
| 빌드 | `ci/circleci: build`,`build_debug` / GHA `build (release|debug)` | 양쪽 |

**코드 sha 와 TC 브랜치가 짝인지 먼저 확인한다.** GHA 는 산출물에 적어 둔다:

```bash
RUN=<gha-ci run id>                       # gha-ci: test_shell 링크의 runs/<숫자>
B=http://192.168.1.48:30080/runs/$RUN
curl -s $B/results/45/build.read          # sha= / mode= / run_id=   ← 코드
curl -s $B/results/45/tc.read             # tc_sha= / tc_branch=tc/pr-<N>  ← 답안
```

- TC 브랜치 규약과 어긋난 조합이면(다른 PR 의 `tc/pr-*`) **분석하지 말고 재트리거를 요청한다** —
  답안 드리프트가 대량 NOK 로 보인다(131/131 사례, [[테스트TC-규칙]] §0).
- 로컬 재현을 할 때도 같은 짝으로 맞춘다:
  ```bash
  git -C ~/dev/sources/cubrid-testcases fetch origin tc/pr-<N> && git -C ~/dev/sources/cubrid-testcases checkout tc/pr-<N>
  git -C ~/dev/sources/cubrid-testcases-private-ex fetch origin tc/pr-<N> && ... checkout tc/pr-<N>
  goto <code sha>                     # build.read 의 sha
  ```
  `git -C ~/dev/sources/cubrid-testcases branch --show-current` 로 한 번 더 눈으로 확인한다.
- **shell 샤드는 `mode=debug` 로 돈다**(build.read). debug 산출의 fetch/ioread 수치를 성능
  판정이나 답안에 쓰지 않는다([[테스트TC-규칙]] §3.5).

## 2. T2 — 실패 수집 (CI 가 1차, 로컬은 보조)

### shell (GHA)

```bash
B=http://192.168.1.48:30080/runs/$RUN
curl -s $B/failed.list                    # "<샤드>\t<케이스 경로>"  ← 실패 전체 목록
curl -s $B/results/<샤드>/test-shell.xml  # 케이스별 <failure> CDATA (OK/NOK 서브체크 + side-by-side diff)
curl -s $B/results/<샤드>/feedback.log    # 그 샤드 실행 로그
curl -s -O $B/results/<샤드>/ctp_log.tgz  # 서버 로그·코어덤프 텍스트
curl -s $B/results/<샤드>/test_status.data # total/fail 카운트
```
- 이 아티팩트 서버는 **컨테이너에서 토큰 없이 열린다**(2026-09-11 .50 실측, `200`).
  보존 기간은 미확인 — **런이 끝나면 바로 받아 둔다.**
- ⚠ **`tc.read` 의 `tc_sha` 는 그 샤드가 돌린 리포 기준이다.** shell 은 private-ex 케이스가 많아 대개
  private-ex 의 sha 가 찍힌다 — 공개 `cubrid-testcases` 의 같은 이름 브랜치는 **다른 sha** 다
  (2026-09-11 실측: `tc/pr-7900` = private-ex `3fada2ff` / testcases `3eadb088`).
  짝 검증·`git show` 할 때 **리포를 먼저 확정**한다. 실패 케이스의 리포는 XML `file=` 접두어로 가른다.
- ⚠ **shell 하네스는 판정 입력 로그(`result*.log` 등)를 보존하지 않는다.** 케이스가
  `grep` 으로 자가판정하는 유형이면 CI 증거만으로는 A/B 를 가를 수 없어 C 로 남는다
  (2026-09-11 `bug_bts_14305`). 재현할 때 그 파일부터 따로 보관한다.
- 같은 목록은 `collect` 잡 로그에도 있다(웹 요약이 막혔을 때):
  `gh run view --repo CUBRID/cubrid --job <collect job id> --log | grep -A5 "failed cases:"`
- shell 은 **`cubrid-testcases-private-ex` 케이스도 함께** 돈다 — 경로 접두어로 리포를 가른다.

### sql / medium (CircleCI)

```bash
J=<job 번호>                                # 체크 링크 끝의 숫자
curl -s "https://circleci.com/api/v1.1/project/github/CUBRID/cubrid/$J/artifacts"
```
- 공개 리포라 **토큰 없이** 열린다. **통과한 잡은 아티팩트가 0개**다(2026-09-11 실측) — 실패 잡만 올린다.
- 코어덤프 텍스트·`build_debug` 바이너리로 발화 지점까지 좁히는 법은 [[CI코어덤프-해석]].

### 로컬 CTP (보조)

전수로 돌리지 않는다. **재현이 필요한 케이스만 단건**으로 돌린다(수십 분 + 벤치와 동시 실행 불가).

**shell 단건은 `ctp.sh` 를 쓰지 말고 케이스 `.sh` 를 직접 돌린다** — conf 를 비우는 것은 Java 하네스의
`resetCUBRID_linux()` 라, 스크립트를 직접 실행하면 그 함정을 지나간다 (2026-09-11 .50 실측, 2케이스 4회 무사고):

```bash
cp -r <케이스 dir> $SCRATCH/run/<이름>          # 리포를 더럽히지 않게 사본에서
cd $SCRATCH/run/<이름>
export init_path=~/dev/sources/cubrid-testtools/CTP/shell/init_path
bash <case>.sh > run.log 2>&1                   # 판정은 <case>.result
```
- 케이스가 끝에 `rm` 으로 산출물을 지운다. **실제 출력이 필요하면 사본에서 그 `rm` 줄만 주석 처리**하고
  한 번 돌려 `*.result` 를 확보한다(답안 정렬의 기준값).
- 케이스는 `change_db_parameter` 로 **활성 설치본 conf 를 건드린다.** 끝나면 원복되지만, 중간에 죽으면
  남는다 — 실행 후 `grep -nE '<바꾼 파라미터>' $CUBRID/conf/cubrid.conf` 로 확인한다.

## 3. T3 — 분류 (서브에이전트로 격리)

**케이스당 서브에이전트 1개**(많으면 스위트·디렉터리 단위로 묶는다). 메인 세션은 목록과 결론만 들고 있는다.

에이전트 프롬프트에 그대로 넣을 것:

```
[대상] <케이스 경로> / <스위트> / 샤드 <N>
[증거] <test-shell.xml 또는 .result diff URL·경로>, <ctp_log 경로>
[허용] cubrid-testcases · cubrid-testcases-private-ex 의 케이스·답안 파일, 위 CI 산출물
[금지] ~/dev/sources/cubrid/src/** 열람, source-notes, 설계문서, 성능규칙집.
       원인이 엔진에 있어 보이면 "엔진 의심"이라고만 적고 소스를 열지 말 것.
[반환] 아래 스키마 그대로, 10줄 이내 증거 인용 포함
  case / suite / shard / 분류(A|B|C) / 판정근거 1줄 / 증거인용 / 답안diff요약 / 재현명령 / 엔진의심(y/n)
```

분류는 **이 순서로** 판정한다:

- **B 코어** — `ctp_log`·서버 err 에 코어덤프 SUMMARY 가 있다.
  단 SUMMARY 발화점이 `fi_handler_random_exit`(fault injection)면 **고의 abort 라 코어로 세지 않는다**
  → C 로도 올리지 말고 "FI 정상"으로 제외([[테스트TC-규칙]] §1).
- **A 출력차** — 코어 없이 정상 종료했고 차이가 답안 diff 뿐이다.
  `<failure>` CDATA 의 side-by-side 에서 **어느 서브체크의 몇 줄이 다른지**까지 적는다.
- **C 미상** — 그 외 전부: 타임아웃, 하네스/환경 오류(conf 유실, "Can't access …"), 인프라·러너 실패,
  재현 불가, 증거 부족으로 A/B 를 가를 수 없는 것. **"애매하면 C"** 가 규칙이다 — A 로 잘못 넣으면
  답안이 오염된다.

⚠ 분류 단계에서 **플랜 개선/회귀를 판정하지 않는다.** 플랜 덤프 눈대중 판정은 실측에 10/18 이 뒤집힌 적이 있다
([[테스트TC-규칙]] §2-1). 플랜이 바뀐 A 는 "A(플랜 변화 포함)"로 표시만 하고 T4 에서 사용자 판단을 받는다.

## 4. T4 — 보고하고 멈춘다

사용자에게 이 표로 낸다. **여기서 워크플로는 일단 끝난다.**

```
PR #<N> / code <sha7> / tc <tc_branch>@<sha7> / 런 <링크>
A 출력차 n건 · B 코어 n건 · C 미상 n건   (FI 제외 n건)

| # | 분류 | 스위트 | 케이스 | 한 줄 근거 | 플랜변화 | 제안 |
```
- A: "답안 정렬 후보" — 승인하면 T5.
- B·C: **목록과 근거만.** 원인 분석·수정 제안을 붙이지 않는다(요청이 오면 별건 작업으로 연다).

## 5. T5 — (승인 후) A 만 출력 정렬

"출력을 맞추는 형식으로만" 진행한다 — **코드로 가지 않는다.**

1. **의도 검사 먼저**([[테스트TC-규칙]] §2). 케이스가 검증하려던 대상이 바뀐 diff 면 정렬하지 말고
   사용자에게 되돌린다("이건 재블레스가 아니라 엔진/케이스 문제입니다").
2. 옛 답안을 기준으로 **케이스의 마스킹을 양쪽에 적용해 정렬 → 필요한 줄만 반영**.
   산출 로그 통째 복사 금지(§3.5), `*_temp_diff` 를 답안으로 쓰지 말 것(§2-1.6).
3. 휘발성 값(시각 등)은 `.sh` 에 마스킹을 넣고 정렬(§3.2).
4. 커밋 전 오염 grep: `git grep -l "Can't access\|/home/cubrid\|$(hostname)" -- <바꾼 파일>`.
5. 정상 모드 재실행으로 OK 확인 후 커밋(§3.4), 푸시는 **그 PR 의 `tc/pr-<N>`** 로, head 리포 확인(§4).
6. `/run all` 은 **치지 않는다** — 사용자·리뷰어 몫([[PR브랜치-규칙]] §0). 다만 **푸시가 끝난 뒤에**
   재트리거를 요청한다(잡 시작 시점 체크아웃 함정).

## 5-1. T7 — 머지 메시지 3종 (최종 머지 직전)

엔진 PR 하나가 머지될 때 **세 리포에 각각 스쿼시 머지 메시지**가 필요하다 —
`cubrid` · `cubrid-testcases` · `cubrid-testcases-private-ex`.

**형식 (관측된 develop 이력 기준)**

```
[CBRD-XXXXX] <영문 한 줄 요약> (#<그 리포의 PR 번호>)
```
- 제목은 **영어**, 지라 키로 시작, 끝에 그 리포 PR 번호. TC 리포는 관용구가 굳어 있다:
  `[CBRD-XXXXX] TC changes for PR CUBRID/cubrid#<엔진PR> (#<TC PR>)`
  (`Revise[d] testcase for …` 도 쓰이나, 엔진 PR 동반 변경이면 앞의 형태가 표준이다.)
- 본문도 영어. **PR 본문만 요약한다 — 소스를 다시 열지 않는다**(이 워크플로의 기본 제약).

**본문에 꼭 넣는 것**

| 리포 | 넣을 것 |
|---|---|
| `cubrid` | 변경의 축을 항목별 한 줄씩 / 결과 집합 불변 여부 / **벤치 수치**(PR 본문 인용) / 리뷰에서 **철회·분리된 항목** |
| TC 두 곳 | 왜 답안이 바뀌었나(플랜 변화 등) / **판정 방법**(실측 fetch·시간 비교) / 개선·중립·회귀 건수 / **케이스 수정**(답안이 아니라 `.sh` 를 고친 것)은 따로 / **안 바꾼 것**과 그 이유 |

⚠ **PR 제목을 그대로 머지 제목으로 쓰지 않는다.** 리뷰 중 철회된 항목이 제목에 남아 있는 경우가 있다
(2026-09-11 PR#7622 실측: 제목의 `plan-comparator band` 는 본문에서 "별도 이슈로 분리"로 철회된 항목이었다).
머지 직전에 **제목과 본문 Remarks 를 대조**한다.

## 6. T6 — 남기기

- 분류 표를 그 PR/이슈 문서(`projects/CBRD-XXXXX/`)에 붙인다. 형식 선례: `projects/CBRD-27094/CI실패분류.md`.
- 반복되는 하네스 함정은 [[테스트TC-규칙]] 에, 새 CI 경로 사실은 이 문서에 환류한다.
- 보드·worklog 는 [[작업트래커]].

## 자주 걸리는 것

- **TC 브랜치 불일치** — 제일 흔한 가짜 NOK. T1 을 건너뛰지 않는다.
- **shell 을 CircleCI 에서 찾는다** — 없다. `gha-ci: test_shell` 이다.
- **debug 수치를 근거로 쓴다** — shell 샤드는 debug 다.
- **통과 잡에서 아티팩트를 찾는다** — 실패 잡만 올린다.
- **애매한 것을 A 로 넣는다** — C 로 보낸다. 답안 오염은 CI 에서 한 줄 diff 로 되돌아온다.

[[테스트TC-규칙]] [[PR브랜치-규칙]] [[CI코어덤프-해석]] [[작업-생애주기]]

## 적용 실적

- **2026-09-11 .50 — 첫 적용, PR#7900 · PR#7622** (shell 3건: A 2 / B 0 / C 1).
  A 2건(`bug_bts_13242`·`cbrd_20145_1`)은 사용자 의도 확정 후 정렬 → 로컬 2회 연속 OK → `tc/pr-7900` 푸시(`38bac27fb`).
  C 1건(`bug_bts_14305`)은 지시대로 손대지 않음. 이때 T1(짝 맞추기)·T3(서브에이전트 격리)은 그대로 통했고,
  위의 `tc.read` 리포 주의·`result*.log` 미보존·shell 단건 실행법 세 가지가 이 실행에서 나왔다.
