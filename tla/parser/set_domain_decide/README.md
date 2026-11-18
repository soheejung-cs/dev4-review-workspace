# set_domain_decide — 집합 생성자 도메인의 결정과 서버 도메인 캐시 (CBRD-28066)

- 이슈: CBRD-28066 (회귀 원인 CBRD-27817 `dea63010e`, 캐시 쪽 앞선 수정 CBRD-27016 #7550). 2026-11-18 구현 하네스(.50), 엔진 브랜치 `CBRD-28066` head `9599c3513` = develop `2e67e1d94` + 1커밋.
- 대상 함수: `pt_function_to_regu ()`·`pt_make_function ()`·`pt_reader_domain ()`·`pt_make_regu_subquery ()`(src/parser/xasl_generation.c), `pt_add_type_to_set ()`(parse_dbi.c), `pt_coerce_node_collation ()`(type_checking.c), `tp_domain_find_set ()`·`tp_is_domain_cached ()`(src/object/object_domain.c), `or_unpack_domain ()`(object_representation.c), `stx_unpack_domain ()`·`stx_map_stream_to_xasl ()`(src/query/stream_to_xasl.c). 고친 판에서 더한 것: `pt_decide_collection_domain ()`·`pt_domain_is_decided ()`·`tp_domain_coll_flags_match ()`.
- 종류: 상태기계(동시성 없음). 하위 스킬 [`tla.md`](../../../skills/harness-review/tla.md) §1 은 파서 같은 순수 계산은 돌리지 않는다고 하지만, 사용자 요청(2026-11-18 "TLA+ 쪽이 낫겠다, 해당 방향으로 진행해 줘")으로 돌렸다. 모델로 보는 이유: 결과가 **서버 수명 동안 남는 도메인 캐시의 이력**에 달려 있다. 같은 문장도 앞에 무엇이 돌았느냐에 따라 실패하거나 가려지므로, 재현 SQL 은 고른 순서만 보고 모델은 범위 안의 모든 순서를 본다. 또 고침이 세 자리(생성자·리더·캐시)와 중첩 재귀로 나뉘어 있어, 하나를 빼면 무엇이 깨지는지를 cfg 로 가른다.
- 실행: `python3 tools/tla/tlc_run.py tla/parser/set_domain_decide/SetDomainDecide.tla --config tla/parser/set_domain_decide/<cfg> --expect <violation|ok>` — before·before_history·cache_only_unresolved·elem_shallow·no_reader 는 `violation`, cache_only·elem_no_cache·after 는 `ok`.

## 무엇을 모델로 했나
서버 하나의 수명 동안 세션들이 계획을 `MaxRuns` 개 실행한다(어느 세션인지는 결과에 영향이 없어 이름을 두지 않는다 — 캐시가 프로세스 전역이다). 계획은 운반 자리(`Carriers`)와 원소 둘의 종류(`Kinds`)로 고른다.
- 원소 종류: `col`(열·리터럴 — NORMAL), `enf`(NVL (?, 'x') 처럼 바인드를 감싼 ENFORCE CAST 를 거친 식 — 실측의 coll=1), `leave`(MAYBE 의 기본 도메인 — LEAVE).
- 운반 자리: `ctor`(IN 목록·= ANY {}·SET{} — 원소 둘), `nested`(행 값 IN ((a, b), (c, d)) — 원소가 집합), `reader`(파생 표·CTE·부질의의 집합 열을 읽는 쪽 — 생산자와 리더 도메인 둘을 싣는다), `scalar`(스칼라 regu — 대조군, XASL 생성이 이미 정한다).
- 도메인은 깊이 2까지의 레코드(`t`·`flag`·`elems`)이고, 캐시 키(`Shape`)는 플래그를 뺀 모양이다(`tp_domain_match ()` 가 문자열의 collation_flag 를 보지 않는 것).

| 모델 행동·연산자 | 코드 (develop `2e67e1d94` / 고친 판 `9599c3513`) |
|---|---|
| `TCFlag` | type_checking.c:22221·22258 `pt_coerce_node_collation ()` — MAYBE 를 CAST(… AS VARCHAR, ENFORCE)로 감쌈; :4981·:9059·:12009 LEAVE 기본 도메인 |
| `Tree` (집합 data_type) | parse_dbi.c:198 `pt_add_type_to_set ()` — 원소 data_type 을 플래그째 복사(MAYBE 원소는 건너뜀) |
| `Build` (`FixElem = FALSE`) | xasl_generation.c:8913 `pt_make_function ()` — `pt_xasl_node_to_domain (node)` 로 파스 트리 도메인 |
| `Build` (`FixElem`, `Recursive`) | 고친 판 xasl_generation.c:9201 `pt_decide_collection_domain ()` — 미결정이면 피연산자 regu 의 확정 도메인으로 원소 목록(피연산자가 먼저 만들어지므로 아래에서 위로), :9263 `pt_function_to_regu ()` 가 부름. `Recursive = FALSE` 는 맨 위 원소 플래그만 고치는 대안 |
| `Reader` | xasl_generation.c:6907 `pt_reader_domain ()` — develop 은 문자열 스칼라만 생산자 콜레이션을 받음. `FixReader`: 미결정 집합이면 결정된 생산자 도메인. :9428 `pt_make_regu_subquery ()` 리스트 갈래의 조건도 같은 판정(`pt_domain_is_decided ()` :6868) |
| `Lookup` / `UnpackAll` | object_representation.c:3560 `or_unpack_domain ()` → object_domain.c:2968 `tp_domain_find_set ()` — 맞는 첫 캐시 항목, 없으면 넣음. `FixCache`: :2062 `tp_domain_coll_flags_match ()` 로 중첩 원소의 플래그까지(같은 도우미를 :2100 `tp_is_domain_cached ()` 에도) |
| `Execute` 의 `accepted` | stream_to_xasl.c:7457 `stx_unpack_domain ()` → :7443 `stx_domain_is_decided ()`, :268 `ER_QPROC_DOMAIN_UNRESOLVED`(debug 는 :267 assert) |

## 성질
- `NoUnresolved` — 모든 계획이 적재된다(이슈).
- `CacheFaithful` — 서버가 푼 도메인 = 클라이언트가 싼 도메인(캐시가 다른 계획의 도메인을 건네지 않음).
- `HistoryIndependent` — 계획의 성패가 같은 서버에서 앞서 돈 계획과 무관하다(새 서버에서 돌린 결과 `alone` 과 같음).

## 모델이 보지 않는 것
- 스칼라 regu 의 결정(`pt_decide_function_collation ()` 등)은 맞다고 둔다 — 퍼징(무작위 5534건·행렬 1456건)에서 스칼라 도메인의 -1383 은 한 건도 없었다.
- 집합 원소의 중복 제거·순서, MIDXKEY, 카탈로그(열 타입)의 집합 도메인. 순서 없는 집합의 원소 짝짓기(고친 판은 짝지은 원소끼리 플래그를 비교하고, 중첩 원소는 목록 순서로 비교해 다르면 캐시를 놓칠 뿐이다).
- 클라이언트 쪽 캐시(`tp_is_domain_cached ()`)와 서버 쪽 캐시는 하나로 본다 — SA 에서는 실제로 하나다.

## 결과
| 날짜 | 대상 | cfg | 판정 | 상태(생성/고유) | 깊이 | 시간 | 도구 |
|---|---|---|---|---|---|---|---|
| 2026-11-18 (.50) | develop | before.cfg | **NoUnresolved 위반**(기대대로) — 반례 2상태: `enf` 원소의 ctor 하나 | 12 / 12 | 3 | 1.3s | TLC2 (tla2tools v1.7.4), Java 11 |
| 2026-11-18 (.50) | develop | before_history.cfg | **CacheFaithful 위반**(기대대로) — 결정된 ctor 가 캐시된 뒤 같은 모양의 `enf` ctor 가 그 항목을 받음(가림) | 42 / 42 | 3 | 1.3s | 같음 |
| 2026-11-18 (.50) | FixCache 만 | cache_only.cfg | 성립 | 94,645 / 47,989 | 4 | 4.1s | 같음 |
| 2026-11-18 (.50) | FixCache 만 | cache_only_unresolved.cfg | **NoUnresolved 위반**(기대대로) — 컴파일은 그대로 미결정 | 11 / 11 | 3 | 1.3s | 같음 |
| 2026-11-18 (.50) | 맨 위 원소만 | elem_shallow.cfg | **NoUnresolved 위반**(기대대로) — nested | 13 / 13 | 3 | 1.3s | 같음 |
| 2026-11-18 (.50) | 리더 없이 | no_reader.cfg | **NoUnresolved 위반**(기대대로) — reader | 15 / 15 | 3 | 1.3s | 같음 |
| 2026-11-18 (.50) | 캐시 고침 없이 | elem_no_cache.cfg | 성립 — 컴파일을 고치면 미결정 집합 도메인이 캐시에 닿지 않는다(M2 는 잠복) | 94,645 / 47,989 | 4 | 7.2s | 같음 |
| 2026-11-18 (.50) | 고친 판 | after.cfg | 성립 | 94,645 / 47,989 | 4 | 4.9s | 같음 |

실측 대조(모델 밖): develop 에서 CS 서버를 새로 띄우고 A(`SET{IFNULL(?, q.c0), 1}`) → 다른 세션의 B·바인드 없는 `SELECT SET{c0, 1}` 가 실패, B 먼저면 A 통과 — `before_history` 반례와 같은 두 방향. 고친 판 `9599c3513` 에서는 A → B → C 모두 성공.
