# session_sleeper_conn — 트랜잭션 인터럽트가 다른 작업 스레드의 conn_entry 를 읽을 때

- 이슈·PR: CBRD-27960 · CUBRID/cubrid#8427 (2026-11-10 자기 리뷰) — 리뷰 스레드 [r4489520248](https://github.com/CUBRID/cubrid/pull/8427#discussion_r4489520248)(Codex P2 "Snapshot the session ID before waking the target worker") 판정에도 쓴다.
- 대상 함수: `logtb_set_tran_index_interrupt ()` 의 SERVER_MODE 블록(site B), `logtb_interrupt_session_sleeper_mapfunc ()`(site A) — src/transaction/log_tran_table.c. 둘 다 #7272(CBRD-26904, `fcf161e74`)가 넣었다.
- 실행: `python3 tools/tla/tlc_run.py tla/transaction/session_sleeper_conn/SessionSleeperConn.tla --config tla/transaction/session_sleeper_conn/before.cfg --expect violation`
  · `… --config …/after.cfg --expect ok` · 리뷰 스레드용 `thread_*.cfg` 는 아래 결과 표의 기대값으로.
- 같은 질문의 단위 suite: `unit/transaction/log_tran_table.session_sleepers`(B4·B5·B6 가 아래 반례와 같은 엇갈림을 훅으로 재현한다).

## 무엇을 모델로 했나
T 가 트랜잭션 X 에 인터럽트를 건다(KILL QUERY 의 서버 스레드, 또는 오류로 끝난 병렬 작업). X 를 처리하는 작업 W, 다른 세션 S3 의 작업 V, X 의 연결 c1 을 닫는 연결 작업 스레드, c1 을 다시 받을 수 있는 새 클라이언트를 함께 둔다.
변형(`VARIANT`)은 같은 명세에서 상수 하나로 가른다 — `develop`(두 자리 모두 두 번 읽음, site B 는 깨운 뒤 읽음) · `pr`(한 번 읽음, site B 는 포인터를 깨우기 전에·session_id 를 뒤에) · `proposal`(리뷰 스레드 제안: session_id 도 깨우기 전에) · `validate`(스케치: 그 연결의 client_id 가 X 의 `tdes->client_id` 가 아니면 세션 ID 를 버림).
환경 스위치: `DISCONNECT`(X 의 클라이언트가 끊김 → c1 반납·재사용 가능) · `CWWAKES`(연결 종료 경로가 W 를 스스로 깨움 — 코드는 TRUE, FALSE 는 반사실) · `REUSE`(W 가 X 의 요청을 끝내고 S3 의 요청을 받을 수 있음) · `WSTART`(W 가 SLEEP() 에서 기다림 / 실행 중).

| 모델 | 코드 (PR 헤드 `488dcb882`, base develop `953e83256` — 한쪽만 있으면 표시) |
|---|---|
| `TFind` | log_tran_table.c:2989(base :2984) `logtb_find_thread_by_tran_index_except_me (tran_index)` → :6368-6373 → `logtb_find_thread_entry_mapfunc ()` :6332 `thread_ref.tran_index != tran_index` (잠금 없음) |
| `TReadConn` (pr·proposal·validate) | :2994 `CSS_CONN_ENTRY *tran_conn = tran_worker->conn_entry;` |
| `TWake` | :2997-2998(base :2988-2989) → thread_entry.cpp:816-820 → :781-801 — 엔트리 잠금, `resume_status` 가 `THREAD_SLEEP_FUNC_SUSPENDED` 일 때만 신호, 해제. 깨운 스레드를 기다리지 않는다 |
| `TReadSid` (pr) | :3003 `logtb_interrupt_session_sleepers (thread_p, tran_conn->session_id)` 의 인자 |
| `TReadSid` (proposal) | `unit/…/mut_proposal.diff` — :2994 바로 다음 줄로 옮김 |
| `TValidate` (validate) | `unit/…/mut_validate.diff` — `tdes->client_id`(log_impl.h:498, log_tran_table.c:889 에서 연결의 `client_id`)와 `tran_conn->client_id`(connection_defs.h:461, 연결마다 새 값 connection_sr.c:268-273) 비교 |
| `TDevChk` · `TDevDeref` (develop) | base :2990 `tran_worker->conn_entry != NULL` · :2994 `tran_worker->conn_entry->session_id` |
| `TMap` | :2888-2896 `logtb_interrupt_session_sleepers ()` — :2891 `DB_EMPTY_SESSION` 이면 돌아감, :2895 `map_entries` |
| `TMapW`·`TMapV` (develop) | base :2845 `tran_index_lock`, :2847-2850 현재 스레드·TS_DEAD·TS_FREE·TS_CHECK |
| `TMapWChk`·`TMapWDeref` (develop) | base :2851 `thread_ref.conn_entry != NULL && thread_ref.conn_entry->session_id == session_id` (두 번 읽음 — 이슈의 SIGSEGV 자리) |
| `TMapW`·`TMapV` (PR) | :2847 `tran_index_lock`, :2852 `conn = thread_ref.conn_entry;` |
| `TMapWCmp`·`TMapVCmp` (PR) | :2853-2856 상태 검사와 `conn != NULL && conn->session_id == session_id` |
| `MatchW`·`MatchV` | :2858-2870 `tdes->interrupt = true`·`num_interrupts`, :2881 깨움 |
| `WEnd` | server_support.c:568 `css_set_thread_info (…, local_tran_index, …)` — tran_index 반납 |
| `WNull` | server_support.c:2198 `thread_ref.conn_entry = NULL;` — `tran_index_lock` 은 :547(:572)에서 이미 놓았다 |
| `WFreed` | :2199 `TS_FREE`, :2201 `end_working_task ()` |
| `WTake` | :2176 `thread_ref.conn_entry = &m_conn;` — 잠금(:2195) 전 |
| `WRun3` | :2191 `TS_RUN`, :2195 잠금, :545 `tran_index`, :547 해제 |
| `V*` | 위 W 의 같은 줄(S3 의 작업이 요청을 끝내고 받는 것) |
| `CWClose` | connection_worker.cpp:984-987 `CONN_CLOSING`, :1028 `net_server_wakeup_workers ()` → network_sr.c:967 인터럽트, :998·:1030 `THREAD_SLEEP_FUNC_SUSPENDED` 깨움 |
| `CWFree` | connection_worker.cpp:1040-1056(작업·남은 task 가 0 일 때만) → :1107-1111 → coordinator.cpp:1006 `css_free_conn ()` → connection_sr.c:408 `session_id = DB_EMPTY_SESSION`, :663-664 free list 머리 |
| `NCConnect` | master_connector.cpp:883 `css_make_conn ()` → connection_sr.c:591-592 머리에서 꺼냄, :268-273 새 `client_id`, :291 `DB_EMPTY_SESSION` |
| `NCSession` | session.c:2855 `thread_p->conn_entry->session_id = session_p->id` |

## 성질
- `NoNullDeref`(= `NoCrashA` ∧ `NoCrashB`) — 서버가 죽지 않는다: 두 자리 어디서도 NULL 을 역참조하지 않는다. PR 이 주장하는 것.
- `SidIsTarget` — site B 가 매퍼에 넘기는 세션 ID 는 X 의 세션이거나 비어 있다. 리뷰 스레드가 주장하는 결함의 반대.
- `OnlyTargetInterrupted` — 매퍼가 인터럽트를 거는 작업은 X 의 세션 것뿐(사용자에게 보이는 것).
- `NeverTargetsS1` — 확인용, **위반이 기대값**: validate 판도 정상 경로에서는 X 의 세션을 넘긴다(버리기만 하는 모델이 아니다).

## 결과
TLC2 2.19 (tla2tools v1.7.4, sha256 `936a2620…050e88`), 2026-11-10 (.51), PR 헤드 `488dcb882`, 워커 2개.

| cfg | 변형 · 환경 | 성질 | 판정 | 상태(생성/고유) | 깊이 | 시간 |
|---|---|---|---|---|---|---|
| before.cfg | develop · 전부 켬 | NoNullDeref | **위반**(기대) — 반례 7상태(site B) | 1,378 / 463 | 8 | 2.0s |
| before_mapper.cfg | develop · 전부 켬 | NoCrashA | **위반**(기대) — 반례 11상태(site A, 이슈의 :2851) | 4,635 / 1,381 | 12 | 2.0s |
| after.cfg | pr · 전부 켬 | TypeOK NoNullDeref | **성립** | 19,285 / 5,222 | 24 | 2.6s |
| thread_develop_codex.cfg | develop · 끊김+종료 경로 깨움, W SLEEP | SidIsTarget | 성립(기대) — develop 은 깨운 뒤 W 의 지금 포인터(NULL)를 다시 읽어 이 길이 없다 | 6,995 / 1,946 | 22 | 2.1s |
| thread_pr_codex.cfg | pr · 같음 | SidIsTarget | **위반**(기대) — 반례 13상태 | 3,558 / 1,144 | 14 | 2.1s |
| thread_proposal_codex.cfg | proposal · 같음 | SidIsTarget | **위반**(기대) — 반례 13상태 | 3,592 / 1,179 | 14 | 2.1s |
| thread_pr_signal.cfg | pr · 끊김, 종료 경로가 깨우지 않음(반사실) | SidIsTarget | **위반**(기대) — 반례 13상태 | 2,641 / 875 | 14 | 1.9s |
| thread_proposal_signal.cfg | proposal · 같음 | SidIsTarget | 성립(기대) | 5,086 / 1,430 | 21 | 2.1s |
| thread_develop_reuse.cfg | develop · 작업 재사용, W SLEEP | SidIsTarget | **위반**(기대) — 반례 10상태 | 1,022 / 381 | 12 | 1.9s |
| thread_pr_reuse.cfg | pr · 같음 | SidIsTarget | 성립(기대) | 3,300 / 1,002 | 21 | 1.9s |
| thread_proposal_reuse_run.cfg | proposal · 작업 재사용, W 실행 중 | SidIsTarget | **위반**(기대) — 반례 10상태 | 1,729 / 593 | 12 | 1.9s |
| thread_pr_reuse_run.cfg | pr · 같음 | SidIsTarget | **위반**(기대) — 반례 10상태 | 1,620 / 550 | 12 | 1.3s |
| thread_develop_full.cfg | develop · 전부 켬 | SidIsTarget | **위반**(기대) — 반례 10상태 | 3,580 / 1,094 | 11 | 1.4s |
| thread_pr_full.cfg | pr · 전부 켬 | SidIsTarget | **위반**(기대) — 반례 10상태 | 3,790 / 1,183 | 11 | 1.4s |
| thread_proposal_full.cfg | proposal · 전부 켬 | SidIsTarget | **위반**(기대) — 반례 10상태 | 3,989 / 1,259 | 11 | 1.4s |
| thread_validate_full.cfg | validate · 전부 켬 | TypeOK NoNullDeref SidIsTarget OnlyTargetInterrupted | **성립** | 18,755 / 4,962 | 24 | 2.6s |
| thread_validate_reach.cfg | validate · 전부 켬 | NeverTargetsS1 | 위반(기대 — 확인용) | 1,331 / 469 | 8 | 1.9s |

반례(코드 경로로):
- **before**(site B): W 가 X 를 실행 중 → T 가 W 를 찾음(:2984) → W 가 tran_index 반납(server_support.c:568) → T 가 깨움(:2988) → T 의 NULL 검사 통과(:2990) → W 가 `conn_entry = NULL`(:2198) → T 가 다시 읽어 역참조(:2994) — NULL.
- **before_mapper**(site A, 이슈가 본 자리): …T 가 매퍼로 W 를 볼 때 상태 검사 통과 → NULL 검사(:2851 앞쪽) 통과 → W 가 :2198 → 같은 줄 뒤쪽 `->session_id` 역참조 — NULL. 이슈의 부하(짧은 질의 세션 12개)가 늘 요청을 받고 끝내는 V 들이 이 자리에 닿는다.
- **thread_pr_codex**(리뷰 스레드): X 의 클라이언트가 끊김 — 연결 종료 경로가 SLEEP() 의 W 를 **스스로** 깨움(network_sr.c:1030) → T 가 W 를 찾음 → W 가 요청을 끝냄(:568) → T 가 `tran_conn = c1`(:2994) → W 가 :2198·:2199-2201 → c1 반납(connection_sr.c:408 `DB_EMPTY_SESSION`, free list 머리) → 새 클라이언트가 c1 을 받아(:591) 세션 S2(session.c:2855) → T 가 깨움(이미 깨어 있음) → T 가 `c1->session_id` = S2(:3003) → 매퍼가 S2 의 작업에 인터럽트.
- **thread_proposal_codex**: 위와 같은 순서에서 T 의 session_id 읽기만 깨우기 앞 — 그래도 S2. W 를 놓아준 것이 T 의 신호가 아니라 종료 경로이므로 "깨우기 전에 읽기" 가 W 를 붙잡아 두지 못한다.
- **thread_pr_signal**(반사실 — 스레드 본문 그대로): 종료 경로가 깨우지 않으면 T 의 신호가 W 를 놓아주고 → 요청 끝 → c1 반납·재사용 → S2. 이 환경에서는 proposal 이 성립한다(`thread_proposal_signal`).
- **thread_develop_reuse**(develop 의 길): T 가 깨운 W 가 요청을 끝내고 S3 의 요청을 받음(:2176) → develop 이 다시 읽은 포인터는 c3 → S3.
- **thread_proposal_reuse_run**(남는 창): 실행 중인 W 가 T 의 찾기와 읽기 사이에 요청을 끝내고 S3 의 요청을 받음 → c3 → S3. pr·proposal 모두 같다.

정리: PR 은 NULL 역참조를 없앤다(before 위반 → after 성립). 다른 세션의 ID 를 넘기는 길은 develop 에도 있다(작업 재사용 — `thread_develop_reuse`); PR 의 순서는 그 길을 닫고(`thread_pr_reuse`) 연결 반납·재사용 길을 새로 연다(`thread_pr_codex`). 리뷰 스레드의 제안은 T 의 신호만이 W 를 놓아줄 때만 그 길을 닫고(`thread_proposal_signal`), 스레드가 말한 바로 그 경합(끊기는 세션)에서는 종료 경로가 W 를 깨우므로 닫지 못한다(`thread_proposal_codex`). 찾기와 읽기 사이의 창은 셋 다 남는다(`thread_proposal_reuse_run`). client_id 검증 판은 모든 환경에서 성립한다.

## 모델이 보지 않는 것
- X 를 함께 쓰는 병렬 작업 여럿 — `logtb_find_thread_by_tran_index_except_me ()` 는 그중 첫 번째를 돌려준다. 모델의 W 는 하나다.
- 트랜잭션 인덱스 재사용(X 가 반납된 뒤 새 클라이언트에 다시 배정) — 찾기가 새 주인의 작업을 돌려줄 수 있다. validate 스케치는 `tdes->client_id` 를 블록 처음에 읽어 이것도 거르지만 모델에는 넣지 않았다.
- 같은 세션 ID 로 다시 붙는 클라이언트, SLEEP() 시간 초과(종료 경로의 깨움과 같은 효과), `net_server_wakeup_workers ()` 가 부르는 site B 자체.
- 시간·확률 — TLC 는 도달 가능성만 본다. 창이 명령 몇 개 길이인지, 그 사이에 연결 종료·반납·새 접속·세션 수립이 다 들어갈 확률은 다루지 않는다.
- 컴파일러·메모리 순서 — 소스의 읽기 하나를 원자 걸음 하나로 둔다(잠금 없는 공유 필드라 C++ 메모리 모델로는 데이터 경쟁, release 빌드가 develop 의 두 읽기를 하나로 합친 것도 모델 밖). validate 스케치의 `session_id` → `client_id` 읽기 순서는 x86(TSO)에서는 지켜지지만 약한 메모리 모델에서는 획득 순서(acquire)나 장벽이 필요하다(성능규칙집 COH-10).
- 매퍼가 쥐는 `tran_index_lock` 은 W·V 의 tran_index 설정(:2195-547)만 막는다고 두었다 — conn_entry 쓰기(:2176·:2198)는 그 잠금 밖이다(코드 그대로).
