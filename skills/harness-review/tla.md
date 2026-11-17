# harness-review 하위 스킬 — TLA+ 모델 검사

사용자 지시 2026-11-10: "TLA 검증 단계를 추가하고 싶어 이번 PR부터 사용하고 싶고 harness review시의 세미 스킬로 들어갔으면 해"
(그 앞 메시지: "동시성에 중점을 맞춘 언어이고 자체적으로 무결함을 증명하는 기능도 있어서 동시성과 관련있는 pr 진행하실때 사용하시면 도움이 될거같습니다").
SKILL.md 절차 3-2 에서 이 파일을 연다. 첫 적용: CUBRID/cubrid#8472(CBRD-28007), 명세 `tla/query/scan_minmax_turn/`.
**준비 — 세 컨테이너 모두**(사용자 결정 2026-11-10): `~/tools/tla/tla2tools.jar`(v1.7.4)와 Java 11 이상. 외부 코드라 **사람이 설치한다**(§4 명령) — 없으면 claude-workspace 세션 시작 훅(`hooks/session_start.sh`)이 설치 명령을 알린다.
설치 확인: 크기 2,274,532바이트(GitHub 릴리스 자산), sha256 `936a2620…050e88` — 세션 시작 훅이 크기를, `tlc_run.py` 가 sha256 을 대조한다. 일반 셸에서는 명령 앞에 `!` 를 붙이지 않는다(bash 의 `!` 는 종료 코드를 뒤집어 `&&` 뒤의 `curl` 이 돌지 않는다 — 11-10 실제로 있었다).

## 왜 — 재현·단위 테스트가 못 덮는 것
- 재현 SQL·단위 테스트는 **우리가 고른 입력 몇 개**만 본다. 동시성 결함은 스레드가 엇갈리는 순서에 달려 있어 재현이 운이고, 상태기계 결함은 "앞 단계가 남긴 상태" 조합에 달려 있어 사례를 빠뜨리기 쉽다.
- TLC(TLA+ 모델 검사기)는 명세의 **상수 범위 안의 모든 상태와 모든 엇갈림**을 끝까지 따라가, 성질이 깨지면 가장 짧은 반례(trace)를 낸다.

## 1. 언제 돌리나
`python3 tools/tla/tla_candidates.py --pr <PR>`(미push 커밋이면 `git diff <base>..<head> > d && … --diff d`)가 후보를 내고, 그 변경이 아래 중 하나일 때.
- **동시성** — 잠금·래치를 잡고 놓는 순서, 원자 연산·CAS, 대기·깨우기, 데몬·워커와 공유하는 상태, 로그·회복 순서. 성질: 상호배제, 잃는 갱신 없음, 교착 없음, 결국 진행.
- **상태기계** — 스캔 커서·위치, 재시도 루프, 프로토콜 단계처럼 "단계 사이에 지켜야 할 관계"(포인터 = 번호, 잡은 래치 = 기록한 래치)를 말할 수 있는 것.
돌리지 않는 것: 식·타입·파서처럼 순수 계산, 성능만 바꾸는 것, 상태가 없는 한 줄 조건 — 재현·단위 테스트로 충분하다. 후보인데 돌리지 않으면 보고서 §0 '리뷰 신뢰도' 에 이유를 한 줄 쓴다.

## 2. 이미 있는 명세부터
`tla/<모듈>/<이름>/README.md` 의 대상 함수에 이 PR 의 변경 함수가 있으면, 그 명세를 이 PR 판으로 맞춰(대응표의 줄·갈래) 먼저 돌린다 — 단위 suite 회귀와 같은 자리다.

## 3. 모델 쓰기 규칙
1. **코드 갈래 하나 = 행동 하나, 코드 상태 하나 = 변수 하나.** 행동 이름 옆 주석에 코드 이름을 쓰고, README 에 대응표(행동 → file:line, PR 헤드와 base rev)를 둔다. 대응표에 없는 코드 동작은 모델이 보지 않는 것이다 — README 「모델이 보지 않는 것」에 적는다.
2. **수정 전·후를 상수 하나로 가른다**(`FIX`) — 같은 명세에 `before.cfg`(develop)·`after.cfg`(PR). 둘이 따로 놀면 "고쳐서 맞다" 가 아니라 "다른 모델이라 맞다" 일 수 있다.
3. **입력은 초기 상태가 고른다** — `x \in 1..Max`, `S \in SUBSET …`. 사례를 손으로 고르지 않는다.
4. **성질은 둘 다**: 사용자에게 보이는 것(질의 결과·교착 없음)과 PR 이 주장하는 내부 관계(포인터 = 번호 같은 불변식). 끝나야 하는 것은 `PROPERTY`(약한 공정성 `WF_vars`).
5. **동시성이면** 스레드마다 `pc` 를 두고, 원자 단위를 코드와 같게 — 잠금 안 구간 하나, 원자 연산 하나가 한 걸음. 잠금 밖의 읽기·쓰기는 걸음을 나눠야 엇갈림이 드러난다.
6. **범위는 결함이 나타나는 최소보다 한두 단계 크게**(묶음 경계를 넘게, 스레드 2~3개), 한 번에 600초(도구 기본) 안. 큰 범위는 따로 돌리고 리뷰에는 결과만.
7. **결과를 만드는 쪽도 모델에 넣고, 그 의미는 코드를 읽어 정한다** — 바꾼 함수만 옮기면 성질이 틀린다. #8472 첫 명세: 스캔이 돌려준 행을 실행기가 어떻게 누적하나(정방향 행은 보통 MAX 로 누적, 뒤집은 뒤 첫 행은 누적값을 **바꾸고** 끝냄, 역방향에서 행이 없으면 정방향 값이 남음 — query_aggregate.cpp min_max_optimized)를 읽고 썼다. '역방향에서 못 받으면 NULL' 로 짐작했다면 develop 반례가 코드와 다른 모양이 됐다.
8. **수정이 기대는 패스 순서·시점(표시 → 접기 등)은 가정으로 두지 않고 상수로 둔다** — 늦은 쪽 상수로 위반을 기대하는 cfg 를 함께 돌리고, 그 상수가 코드에서 참인지는 패스 순서표로 확인한다(등록부 #111·#108, set_op_fold 의 `EarlyMark`).
9. **「모델이 보지 않는 것」 은 안전 지대가 아니다** — 고친 코드가 그 목록의 항목을 읽거나 옮기면(set_op_fold: 접기가 옮기는 집합 연산의 ORDER BY·LIMIT 자리에 DISTINCT 를 더함) 그 항목을 상수로 넣고, 고치기 전 판에서 위반을 기대하는 cfg 를 함께 돌린다. 보고에는 목록에 남은 항목을 '확인하지 않은 것' 으로 적는다(등록부 #117, set_op_fold 의 `TClauseChoice`·`ViewChoice`·`GroupChoice`).

## 3-T. 코드 대조 — 모델이 실제 실행을 설명하나 (trace validation, 2026-11-17)
사용자 지시 2026-11-17 "이거 넣어 줬으면" — Specula(arXiv 2607.25333, `github.com/specula-org/Specula`, 하네스-작성-규칙 R-24)의 방식이다.
§3 의 대응표는 사람이 쓴 주장이고, before 위반은 "모델이 결함을 담았다" 만 보인다. **after 성립이 "모델이 코드와 달라서" 일 수 있다** — 그것을 실제 실행으로 막는다.
코드에 계측을 넣어 돌리고, 그 실행의 행동 순서·상태를 TLC 가 모델 행동으로 하나도 빠짐없이 설명할 수 있는지 본다.

**언제**: 동시성 명세의 after 성립을 보고서 §0 근거로 쓸 때는 **한다**(못 하면 §0 에 "모델 안에서만 확인 — 코드 대조 안 함 + 이유"). 모델 반례만으로 🔴 를 내려 할 때 재현이 안 되면 한다. 동시성 없는 상태기계는 선택(대응표 + before 위반으로 충분한 경우가 많다).

**절차** (도구: `tools/tla/trace/tla_trace.h` · `tools/tla/trace_to_tla.py` · `tlc_run.py --expect trace`, 끝까지 도는 예 `tools/tla/trace/selftest/run.sh`)
1. **계측 자리** — 모델 행동 하나 = `TLA_TRACE_BEGIN`/`END` 한 쌍, `event` 이름 = 행동 이름(1:1). 구간은 모델의 원자 단위와 같게(§3 규칙 5: 잠금 안 구간 하나, 원자 연산 하나). 상태(`END_ST`)는 그 행동이 바꾸거나 본 모델 변수만 — **기록한 필드는 모두 대조한다**, 대조하지 않을 필드는 기록하지 않는다.
2. **패치로만** — 엔진 worktree 에서 계측하고 `git diff > tla/<모듈>/<이름>/trace/instrument.patch`. 엔진 브랜치에 커밋하지 않는다. `#include "tla_trace.h"` 는 `-DTLA_TRACE` 가 없으면 빈 매크로다.
3. **실제 코드를 돌린다** — 기본은 단위 suite: `python3 -m tools.harness.unitseam run … --patch <instrument.patch> --cflags "-DTLA_TRACE -I $PWD/tools/tla/trace"` 를 `TLA_TRACE_DIR=<dir>` 로, 테스트가 스레드 2~3개(시작 장벽)로 대상 함수를 부른다. 서버가 있어야 하는 경로는 사용자 확인 뒤 전용 브랜치·goto 빌드로만(H-05 동시 실행 금지). **손으로 쓴 trace·시뮬레이터·기록용 뮤텍스는 쓰지 않는다**(Specula 규칙 — 관찰하려는 엇갈림이 바뀐다).
4. **변환** — `python3 tools/tla/trace_to_tla.py <dir> --out tla/<…>/trace/TraceData.tla --merged <dir>/merged.json --events <행동,…>`. 오류(이름 불일치·한 스레드 안 겹침·손으로 쓴 시각)면 계측부터 고친다. "스레드 사이 겹친 구간이 없다" 경고면 엇갈림을 시험하지 못한 실행이다 — 시나리오를 다시 짠다(selftest 의 첫 판이 그랬다).
5. **TraceCheck** — `tools/tla/trace/selftest/TraceCheck.tla` 를 `trace/` 로 복사해 `VARIABLES`·`INSTANCE`·`MatchEvent`(행동 이름 → 모델 행동 ∧ 기록 상태 대조)만 바꾼다. 겹친 구간은 TLC 가 모든 순서를 시도한다(`ViablePIDs`, 시간 상자).

**Specula 형식과 맞춤**(2026-11-17, 사용자 "지금 맞춰줘" — Specula `skills/harness-generation/references/concurrent-timebox-guide.md`):
| 층 | Specula | 여기 |
|---|---|---|
| 기록 한 줄 | `{"tag":"trace","event","tid"(0부터),"start","end","state",["args","msg"]}` | 같다(`tla_trace.h`) |
| 파일 | `<prefix>-thread-<tid>.ndjson` | 같다(`TLA_TRACE_PREFIX`, 기본 `trace`) |
| 병합 | 전처리기 → `{"num_threads","max_timestamp","traces":{"<tid>":[…]}}`, 시각은 1부터 조밀한 순위 | 같다(`trace_to_tla.py --merged`) — Specula `Trace.tla` 의 `JsonDeserialize` 가 그대로 읽는다 — **Specula TLC(2026.11.15)로 확인(2026-11-17)**: selftest 의 locked 받아들임(61 states·depth 61), racy 는 depth 10 에서 막힘(우리 CONFORMS·DIVERGES '행동 9개까지' 와 같은 자리). 쓴 명세 `tools/tla/trace/selftest/specula/` |
| Trace 명세 | `Trace.tla`: `pc`·`ViablePIDs`·`MatchEvent`, `JsonDeserialize(TraceFile)` | 같은 구조·이름, 데이터만 TLA 리터럴(`TraceData.tla`) — Json 모듈(CommunityModules jar) 없이 우리 tla2tools.jar 하나로 돈다 |
| 완료 판정 | `PROPERTY TraceFullyConsumed`(<> 모두 소비) | 불변식 `TraceIncomplete` 위반 = 모두 소비한 상태에 닿는 순서가 **하나라도** 있음. Specula 꼴은 겹친 구간에서 한 순서만 막혀도 불일치로 나와(모든 행동열을 본다) 여기서는 쓰지 않는다. 가이드의 `TraceSpec` 은 공정성이 없어 **맞는 trace 도 Stuttering 반례로 실패한다** — Specula 예제처럼 `/\ WF_allVars(TraceNext)` 를 붙이거나(braft) PROPERTY 를 빼고 교착 검사로 판정한다(cometbft) |
옛 우리 형식(`ev`·`t0`·`t1`·`st`, `trace.<pid>.<tid>.ndjson`)도 변환기가 읽는다.
6. **돌리기** — `python3 tools/tla/tlc_run.py tla/<…>/trace/TraceCheck.tla --config tla/<…>/trace/TraceCheck.cfg --expect trace`

| 결과 | 뜻 | 할 일 |
|---|---|---|
| `TRACE CONFORMS` | 이 실행은 모델로 설명된다 — 대응표가 그 실행 범위에서 확인됐다 | §0 에 "코드 대조: 행동 N개·스레드 k·겹친 쌍 m" 한 줄 |
| `TRACE DIVERGES` (행동 n개까지) | n+1 번째 부근에서 모델이 코드를 못 따라간다 | 셋 중 무엇인지 가른다: ① 계측 자리·이름 오류 ② 모델 추상화가 틀림 → 모델을 고치고 before/after 를 **다시** 돌린다 ③ 코드가 모델의 계약을 어김(selftest 의 racy 판: 잃은 갱신) → 결함 후보, 재현으로 |

**Specula 를 그대로 쓸 때**(사람이 설치 — §4 아래 설치 절):
- 새 명세를 처음 쓸 때 `specula-lite`(`~/tools/specula/skills/specula-lite/SKILL.md` — 전역 스킬로 깔지 않는다)의 순서(불변식마다 근거 인용한 `modeling-brief.md` → 모델 → 커버리지 감사 → TLC → 재현)를 따른다. trace 대조는 하지 않으므로 위 1~6 은 따로 한다.
- 버퍼 latch·락 관리자·로그처럼 큰 동시성 하위 시스템은 리뷰 밖 캠페인으로 `specula run --byom=<우리 명세 디렉터리> --artifact=<엔진 전용 worktree> --guidance=<g.md> --keep-original`. 실행 시간 중앙값 3.7시간·시스템당 $19~168(저자 보고)이고 자체 빌드를 하므로 **사용자 승인 뒤, CTP·goto·벤치가 돌지 않을 때만**. guidance 의 Priority Questions 는 "When [사건], can [보이는 나쁜 결과]?" 꼴로, 영역 판정 4단계(①~④)에서 뽑는다.
- `TRACE DIVERGES` 원인 찾기에 Specula 의 MCP `trace_debugger`(`run_trace_validation`·`run_trace_debugging` — 중단점·식 평가)를 쓴다: `trace_to_tla.py --merged` 의 JSON + Specula 꼴 `Trace.tla`(본보기 `tools/tla/trace/selftest/specula/`, base 를 우리 명세로, `MatchEvent` 를 TraceCheck 의 것으로) + `community_jar=~/tools/specula/lib/CommunityModules-deps.jar`.
  - 함정(11-17 .51): MCP 는 PATH 의 `java` 를 부른다(`trace_validation.py` 에 박힘) — 시스템 Java 8 이면 `UnsupportedClassVersionError(55.0)`. 등록에 PATH 를 넣는다: `claude mcp remove tracedebugger --scope user` 뒤 설치 절의 `claude mcp add` 에 `-e PATH=$HOME/tools/jdk-21/bin:/usr/local/bin:/usr/bin:/bin` 를 더해(사람이, 새 세션부터). **11-17 .51 재등록 뒤 MCP 경로 확인**: `run_trace_validation` 으로 selftest locked → `success`(62 states), racy → `TLC reported an error`·`Deadlock reached`(불일치 — 응답의 `success: true` 는 도구 호출 성공일 뿐이니 `status` 를 본다; 막히는 깊이는 실행마다 다르다). 등록 전이거나 다른 컨테이너면 같은 명령을 손으로: `cd <dir> && ~/tools/jdk-21/bin/java -Xmx4G -cp ~/tools/specula/lib/tla2tools.jar:~/tools/specula/lib/CommunityModules-deps.jar tlc2.TLC -config Trace.cfg Trace.tla -lncheck final`.

## 4. 돌리기
```bash
cd ~/dev/docs/dev4-review-workspace
python3 tools/tla/tlc_run.py tla/<모듈>/<이름>/<Spec>.tla --config tla/<모듈>/<이름>/before.cfg --expect violation
python3 tools/tla/tlc_run.py tla/<모듈>/<이름>/<Spec>.tla --config tla/<모듈>/<이름>/after.cfg  --expect ok
```
한 줄 출력(AS EXPECTED / UNEXPECTED · 위반한 성질 · 상태 수 · 깊이 · 시간), 상세는 `out/<cfg>/`(result.json·tlc.log·trace.txt). 도구가 없으면 설치 명령을 찍는다 — 외부 코드라 **설치는 사람이 한다**(2026-11-10 자동 모드가 에이전트의 내려받기·실행을 막았다).
코드 대조 도구가 바뀌면 `sh tools/tla/trace/selftest/run.sh <스크래치 디렉터리>` — locked PASS(CONFORMS)·racy PASS(DIVERGES) 둘 다여야 한다.

**Specula 설치(선택, 사람이)** — Java 21 이상·Python 3.10 이상·`uv`·Maven·`gh` 가 필요하다(우리 TLC 는 Java 11·tla2tools v1.7.4 고정이고, Specula 는 자기 TLA+ jar 를 따로 받는다 — 결과 표에 그 판을 따로 적는다).
```bash
curl -LsSf https://astral.sh/uv/install.sh | sh          # ~/.local/bin/uv
uv python install 3.12                                    # pip 없는 python3.11 대신
# JDK 21: 사용자 공간 tarball 을 ~/tools/jdk-21 에 풀고 JAVA_HOME 을 그쪽으로(시스템 rpm 은 sudo)
git clone https://github.com/specula-org/Specula.git ~/tools/specula && cd ~/tools/specula && uv tool install -e . && specula setup
```
설치 함정(2026-11-17 .51 실측 — 다른 컨테이너도 같은 OS 라 같다):
- `setup.sh` 는 PATH 의 `python3` 로 가상환경을 만든다 — 시스템 3.6 이면 `mcp` 설치에서 죽는다. 위처럼 uv 의 3.12 를 PATH 앞에 두고 돌린다(`python3 --version` 이 3.12).
- 스킬 설치는 `~/.claude/skills` 의 한글 이름(11-17 당시 `출근` — 그 스킬은 같은 날 삭제)을 이상한 항목으로 보고 **전체를 멈춘다**. 우리는 Specula 스킬을 전역에 깔지 않는다(모든 세션 목록에 12개가 실린다) — 쓸 때 `~/tools/specula/skills/<이름>/SKILL.md` 를 경로로 읽는다(`specula-lite` 등). setup 의 Claude Code 질문에는 `n`.
- setup 은 `CLAUDE_CONFIG_DIR=~/.claude` 로 MCP 를 등록해, `CLAUDE_CONFIG_DIR` 를 안 쓰는 우리 환경에서는 아무도 읽지 않는 `~/.claude/.claude.json` 에 들어간다(실제 설정 `~/.claude.json` 은 그대로). trace 디버거만 손으로 등록한다:
  `claude mcp add tracedebugger --transport stdio --scope user -e SPECULA_ROOT=$HOME/tools/specula -- $HOME/tools/specula/tools/trace_debugger/.venv/bin/python $HOME/tools/specula/tools/trace_debugger/mcp_server.py` — 이름을 맨 앞에(`-e` 는 값을 여러 개 받아 뒤의 이름까지 먹는다), 새 세션부터 보인다.

## 5. 판정과 보고
| before | after | 뜻 | 할 일 |
|---|---|---|---|
| 위반 | 성립 | 수정이 범위 안에서 맞다 | 보고서 §0 '리뷰 신뢰도' 에 한 줄: 명세 경로·범위(상수)·상태 수·before 반례 요약 |
| 위반 | 위반 | 수정이 모자라다 | after 반례를 지적 후보로(아래) |
| 성립 | — | 모델이 결함을 담지 못했다 | 대응표부터 다시 — 이 상태의 after 성립은 근거로 쓰지 않는다 |
| 오류·시간 초과 | | 문법·범위 문제 | 고치거나 범위를 줄인다. 못 하면 §0 에 "모델 검사 못 함 + 이유" |

after 성립의 무게는 코드 대조(§3-T)에 달렸다 — `CONFORMS` 면 "코드 대조 확인", 대조를 안 했으면 "모델 안에서만", `DIVERGES` 를 풀지 못했으면 after 성립을 근거로 쓰지 않는다.

- **반례를 지적으로 낼 때**: trace 의 상태마다 대응표로 코드 경로(file:line)를 적고, 할 수 있으면 재현(SQL·`harness-unit`)으로 확인한다. finding 의 `verification` 은 재현까지 되면 `dynamic`, 모델만이면 `method: "model"`, `result` 에 "성질 X 위반, 반례 N걸음: <코드 경로 요약>", `artifact` 에 명세 디렉터리. 판정기는 `model` 을 에러 우려의 확인으로 세되 artifact 가 있어야 하고, 도달 가능성(`reach_trace`) 증명으로는 세지 않는다.
- 게시 코멘트에는 모델 이름·반례의 코드 경로·재현 SQL 을 쓴다. TLA+ 문법을 리뷰 대상자가 읽어야 하는 글로 만들지 않는다.

## 6. 남기기
- 코드 대조를 했으면 `tla/<모듈>/<이름>/trace/` 에 `instrument.patch`·`TraceCheck.tla`·`TraceCheck.cfg`·시나리오(단위 test.cpp 경로)를 커밋한다. `TraceData.tla` 와 원 trace 는 실행마다 다르므로 커밋하지 않고, 결과 표에 행동 수·스레드·겹친 쌍·판정만 남긴다.
- 명세·cfg·README(대응표·결과 표)는 `tla/<모듈>/<이름>/` 로 커밋한다(dev4 는 공개 리포 — 참조 구현 이름·로컬 경로를 쓰지 않는다). `out/` 은 커밋하지 않는다(.gitignore).
- 결과 표에는 날짜·PR 헤드·cfg·판정·상태 수·깊이·시간과 tla2tools 판(result.json 의 `tlc_version`·`jar_sha256`)을 남긴다.

## 무엇을 보장하나 / 못 하나
- 보장: cfg 의 상수 범위 안에서, 모델로 옮긴 갈래에 대해서는 모든 경우를 봤다.
- 코드 대조(§3-T)를 했으면 더해서: 그 실행에서 코드가 한 일은 모델 행동으로 설명된다 — 대응표가 그 실행 범위에서는 사람 주장이 아니다.
- 못 함: 범위 밖(대개 작은 범위의 결함이 큰 범위에도 있다 — "small scope hypothesis" 일 뿐 증명은 아니다), 대응표 밖의 코드, 모델과 코드의 차이(추상화가 틀리면 결론도 틀린다 — 그래서 before 가 위반해야 after 를 믿고, 코드 대조로 그 차이를 실행 단위로 줄인다). 코드 대조는 돌린 실행만 본다 — 안 나온 엇갈림은 여전히 모델 검사의 몫이다. 범위 없는 증명(TLAPS)은 쓰지 않는다.
