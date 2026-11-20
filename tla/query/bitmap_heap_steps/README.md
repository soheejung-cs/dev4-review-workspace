# bitmap_heap_steps — 병렬 bitmap heap scan 의 두 단계(인덱스 단계 → 장벽 → 힙 단계)

- 이슈·PR: CUBRID/cubrid#8627 "Bitmap heap scan" (head `efca4544a`, base develop `3f11aeb26` — merge-base). 2026-11-20 리뷰 절차 3-2(.52).
  `tla_candidates.py --pr 8627` 의 13개 중 이 PR 이 새로 만든 동시성은 `px_scan_input_handler_index` 의 장벽(mutex·condvar·`m_bm_index_pending`)과
  `scan_bitmap_marks` 의 `claim_worker`·`take_partition`(fetch_add)·`take_over`(compare_exchange — 후보 도구는 `scan_next_index_scan` 으로 잘못 붙였다, 실제 자리는 `scan_bitmap_marks::take_over ()`)뿐이다.
  나머지 후보(`S_BEFORE/S_ON/S_END`·`curr_oidno`·`pgbuf_ordered_fix`)는 직렬 bitmap 경로·NL batch 의 단일 스레드 상태기계로, 이 모델 밖이다(「모델이 보지 않는 것」).
- 대상 함수(PR 헤드): `parallel_scan::input_handler_index::bitmap_init_on_main ()`·`bitmap_leave_index_step ()`·`bitmap_wait_index_step ()`(px_scan_input_handler_index.cpp),
  `scan_bitmap_marks::claim_worker ()`·`take_partition ()`(scan_manager.h)·`mark ()`·`end_marks ()`·`load ()`·`take_over ()`·`close_worker ()`·`~scan_bitmap_marks ()`(scan_manager.c),
  그것을 부르는 `task::loop ()`·`task::bitmap_heap_step ()`(px_scan_task.cpp), 주 스레드 `manager::next ()`·`end ()`(px_scan.cpp).
- 실행: `python3 tools/tla/tlc_run.py tla/query/bitmap_heap_steps/BitmapHeapSteps.tla --config tla/query/bitmap_heap_steps/after.cfg --expect ok`
  · 변이 `mut_*.cfg` 와 관찰 `race.cfg` 는 `--expect violation`.

## 무엇을 모델로 했나
워커 W 개(= `m_parallelism` = 장벽의 초기 `pending`)가 각자 잎에서 받은 OID 를 자기 `scan_bitmap_worker` 에 기록하고(`Mark` — 아직 남에게 안 보임),
`end_marks` 로 공개한 뒤 장벽을 떠나고(`Leave`) 모두가 떠나기를 기다린다(`WaitPass`/`WaitStop`). 힙 단계는 `fetch_add` 로 파티션을 하나씩 가져가
모든 워커가 공개한 그 파티션의 OID 를 읽고(`Load`·`ReadPart`), 파티션이 바닥나면 `compare_exchange` 로 넓은 슬롯 OID 목록(`Over`)을 한 워커만 합쳐 읽는다.
초기 상태가 OID 를 워커에 나누는 모든 방식(겹침 포함), `start_tasks ()` 가 일부만 넣는 경우(`StartChoices`)를 고르고, `CanFail` 의 워커는 아무 걸음에서나 실패해
작업 코드의 모든 오류 경로처럼 interrupt 를 올린다. 주 스레드는 시작 실패면 `JOB_ENDED` 를 올리고, 모두 끝나면 결과를 읽고 marks 를 해제한다.
새 기능이라 develop 판이 없다 — **before = 해당 없음**. 대신 `MUT` 상수로 장치 하나씩을 부순 변이를 돌려 성질이 실제로 깨질 수 있음을 보인다(tla.md §5 의 "before 성립 = 모델이 결함을 못 담음" 대용).

| 모델 행동 | 코드 (PR 헤드 `efca4544a`) |
|---|---|
| `Init` 의 `pending = |W|` | px_scan_input_handler_index.cpp:63-77 `bitmap_init_on_main ()` (:75) — px_scan.cpp:1734 `init_on_main (…, m_parallelism)`, :1807-1825 `start_tasks ()` 가 `m_parallelism` 개를 넣음 |
| `Mark` | px_scan_index_leaf_slot_walker.cpp:485-489 → scan_manager.c:7402 `mark ()` (자기 `scan_bitmap_worker` 만 씀; 넓은 슬롯은 자기 `over` 목록 — 마지막 페이지를 잡은 채) |
| `IdxEnd` | px_scan_task.cpp:1297-1307 `get_next_page_with_fix` S_END → `drain_late_joiner_chains` → `bitmap_heap_step` |
| `IdxIntr` / `IdxFail` | px_scan_task.cpp:1247-1267(interrupt·취소 확인 → break) / :1313-1328·:1347-1361(S_ERROR·set_page 오류 → interrupt → break) — 장벽을 떠나지 않는다(:1393 주석 "a worker that failed … raised the interrupt instead"). 실행 전 실패 :61-68 도 interrupt |
| `EndMarks` / `EndFail` | px_scan_task.cpp:1402-1405 → scan_manager.c:7537 `end_marks ()`(:7540-7543 목록 닫기, tbl_start/tbl_order, 마지막 batch write) / 실패 → `raise ()` 뒤에도 장벽은 떠남 |
| `Leave` | px_scan_task.cpp:1407 → px_scan_input_handler_index.cpp:80-89 (`m_bm_mutex` 아래 `--pending`, 0 이면 notify_all) |
| `WaitPass` / `WaitStop` | px_scan_task.cpp:1408-1411 → px_scan_input_handler_index.cpp:91-102 (pending>0 ∧ interrupt 없음 동안 10ms wait_for, 반환 = interrupt 없음) |
| `TakePart` | px_scan_task.cpp:1474 → scan_manager.h:1055-1059 `take_partition ()` (fetch_add) |
| `Load` | px_scan_task.cpp:1478-1486 → scan_manager.c:7569 `load (part, 0)` — 모든 워커의 tbl 항목(:7620-7636, end_marks 를 안 한 워커는 tbl_start 가 비어 건너뜀 :7624-7625)과 기록 사슬 |
| `ReadPart` | px_scan_task.cpp:1488-1517 (64쪽마다 `interrupted ()` :1500, `read_page` → `drain_slot_oids`) |
| `HeapFail` | load·set_bitmap_page·행 오류·take_over·next_over 의 `raise ()` (px_scan_task.cpp:1387-1398) |
| `OverTry` | px_scan_task.cpp:1519-1523 → scan_manager.c:7671 `compare_exchange_strong` |
| `MergeStart`·`MergeEnd` | scan_manager.c:7674-7712 — 첫 비지 않은 목록이 base, :7688 `qfile_append_list (base->over, w.over)`, :7699 `qfile_sort_list (base->over, …, Q_DISTINCT)` → list_file.c:4937 `qfile_sort_list_with_func` 가 base 의 QFILE_LIST_ID 를 destroy 뒤 :5068 `qfile_copy_list_id` 로 덮음 |
| `OverRead` | px_scan_task.cpp:1527-1555 `next_over ()` (1024행마다 `interrupted ()`) |
| `Close` | px_scan_task.cpp:71 `finalize ()` → px_scan_slot_iterator_index.cpp:67-71 → scan_manager.c:7737-7748 `close_worker ()`(:7741 자기 `over` 를 `qfile_close_list`) |
| `MainStartFail` | px_scan.cpp:1891-1910 (`start_tasks` 실패 → S_ERROR) → :2120-2131 `end ()`: JOB_ENDED, `release_workers ()` |
| `MainCollect`·`MainFree` | px_scan.cpp:1913-1993 (result read, 워커 오류면 `wait_workers ()` 뒤 오류) → `end ()` → `reset ()`/`close ()` 의 `~input_handler_index` → scan_manager.c:7195 `~scan_bitmap_marks` |

## 성질
- `NoDup` — 사용자에게 보이는 것: 어떤 실행에서도 한 행이 두 번 나가지 않는다(파티션 하나 = 워커 하나, 넓은 슬롯 목록 = 한 워커).
- `ResultOK` — 주 스레드가 성공으로 돌려준 결과에는 인덱스가 준 모든 OID 가 정확히 한 번 있다(어떤 워커의 미공개 기록도 잃지 않는다).
- `ErrNotLost` — 어느 워커가 실패하면 주 스레드는 성공을 돌려주지 않는다.
- `BarrierSafe` — PR 이 세우는 관계: 힙 단계에 든 워커가 있으면 인덱스 단계(기록·end_marks·떠나기 전)에 남은 워커가 없다 — 남의 표를 읽는 동안 쓰는 이가 없다(scan_manager.h 의 클래스 주석 "read by every worker once all index steps are over").
- `OverClosedAtMerge` — `qfile_append_list ()` 의 단언(list_file.c `last_pgptr == NULL` 둘 다): 합치는 동안 마지막 페이지를 쥔 목록이 없다.
- `OneTaker` — 넓은 슬롯 목록을 합치고 읽는 워커는 하나.
- `FreeLast` — marks 해제는 모든 워커가 끝난 뒤.
- `Ends`(PROPERTY) — 교착 없이 끝난다: 실패한 워커가 장벽을 안 떠나도(interrupt), `start_tasks ()` 가 일부만 넣어도(주 스레드의 JOB_ENDED) 기다리는 워커가 풀린다. TLC 의 교착 검사도 함께.
- 관찰 `OverStructNotShared`(race.cfg 만) — 합치는 워커가 base 워커의 `QFILE_LIST_ID` 를 덮어쓰는 동안 base 의 주인이 `close_worker ()` 로 같은 구조체를 읽지 않는다.

## 결과
| 날짜 | 대상 | cfg | 판정 | 상태(생성/고유) | 깊이 | 시간 | 도구 |
|---|---|---|---|---|---|---|---|
| 2026-11-20 (.52) | PR `efca4544a`(MUT="pr"), 워커 2·OID 3(넓은 슬롯 1)·파티션 2, OID 를 나누는 모든 방식, 둘 다 실패 가능, 시작 {둘}·{하나} | after.cfg | **전부 성립**(TypeOK·NoDup·ResultOK·ErrNotLost·BarrierSafe·OverClosedAtMerge·OneTaker·FreeLast·Ends, 교착 없음) | 127,803 / 66,183 | 32 | 8.1s | TLC2 2.19 (tla2tools v1.7.4, sha256 `936a2620…`) |
| 2026-11-20 (.52) | PR, 워커 3·OID 4(넓은 슬롯 1)·파티션 2, 셋 다 실패 가능, 시작 {셋}·{둘} | after_3.cfg | **시간 초과**(900s, 고유 830만 상태에서 깊이 7) — 범위를 줄여 아래 둘로 | — | — | 900s | 같음 |
| 2026-11-20 (.52) | PR, 워커 3·OID 3(넓은 슬롯 1)·파티션 2, 셋 다 실패 가능, 모두 시작 — 안전성·교착만(PROPERTY 없음) | after_3s_safe.cfg | **전부 성립**(불변식 8개, 교착 없음) | 27,355,490 / 9,402,361 | 42 | 77.6s(-workers 8) | 같음 |
| 2026-11-20 (.52) | 같은 범위 + PROPERTY Ends | after_3s.cfg | 끝내지 못함 — 5분 넘게 돌다 시간 예산으로 멈춤(활성 검사 그래프). 끝남(Ends)은 워커 2 범위(after.cfg)에서만 확인 | — | — | — | 같음 |
| 2026-11-20 (.52) | 변이: 장벽을 end_marks 전에 떠남 | mut_leave_first.cfg | **BarrierSafe 위반**(기대대로) — 7걸음 | 21,291 / 10,453 | 7 | 2.8s | 같음 |
| 2026-11-20 (.52) | 변이: 대기 루프가 interrupt 를 안 봄 | mut_wait_no_intr.cfg | **교착**(기대대로) — 5걸음: 인덱스 단계에서 실패한 워커가 장벽을 안 떠남 | 15,009 / 7,795 | 6 | 2.4s | 같음 |
| 2026-11-20 (.52) | 변이: take_partition 을 load·store 둘로 | mut_part_split.cfg | **NoDup 위반**(기대대로) — 18걸음 | 60,249 / 30,888 | 18 | 5.4s | 같음 |
| 2026-11-20 (.52) | 변이: take_over CAS 를 load·store 둘로 | mut_cas_split.cfg | **OneTaker 위반**(기대대로) — 21걸음 | 80,843 / 42,246 | 21 | 6.0s | 같음 |
| 2026-11-20 (.52) | 변이: start_tasks 실패 뒤 주 스레드가 JOB_ENDED 를 안 올림 | mut_main_silent.cfg | **교착**(기대대로) — 5걸음: 시작된 워커가 장벽에서 영원히 기다림 | 14,578 / 7,363 | 6 | 2.3s | 같음 |
| 2026-11-20 (.52) | 관찰: PR, after.cfg 와 같은 범위 | race.cfg | **OverStructNotShared 위반** — 18걸음(아래) | 63,445 / 32,577 | 18 | 3.3s | 같음 |

race.cfg 반례(코드 경로): 워커 2 만 넓은 슬롯 OID 를 기록(`mark ()` → 자기 `over`) → 둘 다 `end_marks ()`(목록 닫힘)·장벽 통과 → 워커 1 은 파티션이 바닥나 `take_over ()` CAS 를 이겨(scan_manager.c:7671) base = 워커 2 의 목록으로 `qfile_append_list`/`qfile_sort_list`(:7688·:7699 → list_file.c:5068 `qfile_copy_list_id` 가 구조체 전체를 memcpy) —
그동안 워커 2 는 CAS 를 지고(또는 반례처럼 오류로) `bitmap_heap_step` 을 나와 `finalize ()` → `close_worker ()` 가 같은 `w->over` 의 `last_pgptr` 를 읽는다(scan_manager.c:7739-7742). 반례는 워커 2 의 load 실패로 짧게 갔지만 CAS 를 진 정상 종료로도 같은 겹침이 난다(진 워커는 곧장 finalize).
값으로는 무해하다 — `end_marks ()` 가 이미 닫아 `last_pgptr` 는 NULL 이고, 정렬 결과도 닫힌 목록(`do_close = true`)이라 NULL 을 NULL 로 덮는다. 다만 C++ 메모리 모델로는 데이터 경합(TSan 보고 대상)이고, `close_worker ()` 가 닫을 일이 있는 것은 `end_marks ()` 를 못 한 워커뿐이다.

## 코드 대조 (trace validation, tla.md §3-T)
- **계측** `trace/instrument.patch`(엔진에 커밋하지 않음, PR 헤드에 적용): 행동 하나 = 기록 한 줄. `Mark`(mark () 전체, args w·oid = pageid×1000+slot)·`EndMarks`(end_marks () 전체)·`Leave`(락 안, state pending)·`WaitPass`/`WaitStop`(bitmap_wait_index_step () 전체, 반환값으로 이름)·`TakePart`(fetch_add, state part — 없으면 nparts)·`Load`(load () 전체, args part, state heap = heap_tbl 의 OID 들)·`OverTry`(CAS, state won)·`Close`(close_worker ()).
  기록은 scan_manager.c 의 `tla_bm_emit ()` 하나로만 쓴다(`tla_trace.h` 의 파일 상태가 TU 마다 따로라 두 TU 가 같은 이름의 파일을 열지 않게). 워커 번호는 `claim_worker ()` 가 스레드 지역 변수에 둔다.
- **실행** `trace/test_trace.cpp`: unitseam 으로 px_scan_input_handler_index.cpp(장벽) + `--extra` scan_manager.c(실제 `scan_bitmap_marks`)를 PR 빌드 플래그로 컴파일하고, 스레드 2~3개가 시작 장벽에서 함께 출발해 px_scan_task.cpp 의 순서(:1236-1241, :1402-1411, :1474-1486, :1519-1523, finalize)로 실제 함수를 부른다.
  가짜는 다른 TU 의 `logtb_get_mvcc_snapshot ()`(스냅샷 있음)·`file_get_all_data_sectors ()`(볼륨 0 의 섹터 0·1, 모든 페이지 할당)뿐이고 나머지는 unitseam 의 0 stub(run.sh 의 `UNITSEAM_ZERO`). 세 번째마다 한 워커가 인덱스 단계에서 loop () 의 오류 경로처럼 interrupt 를 올리고 장벽을 떠나지 않는다(`UT_SCENARIO=intr`).
  빌드: `UNITSEAM_STUB=zero TLA_TRACE_DIR=<OUT>/build python3.11 -m tools.harness.unitseam run --rev efca4544a --file src/query/parallel/px_scan/px_scan_input_handler_index.cpp --extra src/query/scan_manager.c --mode server --test tla/query/bitmap_heap_steps/trace/test_trace.cpp --patch tla/query/bitmap_heap_steps/trace/instrument.patch --cflags "-DTLA_TRACE -I $PWD/tools/tla/trace" --out <OUT>/build` → `sh tla/query/bitmap_heap_steps/trace/run.sh <OUT> 30`(unitseam 도 바이너리를 한 번 돌린다 — `TLA_TRACE_DIR` 이 없으면 cwd(리포 최상위)에 trace 가 남는다).
  (2026-11-20 unitseam 고침: `--cflags` 가 도움말과 달리 `--extra` TU 에 붙지 않아 extra 의 계측이 꺼진 채 링크됐다.)
- **판정** `trace/TraceCheck.tla`(`tlc_run --expect trace`): 기록한 행동은 순서대로 모델 행동과 맞아야 하고(기록한 필드는 모두 대조), 계측할 수 없는 px_scan_task.cpp 쪽 걸음(`IdxEnd`·`IdxIntr`·`IdxFail`·`ReadPart`·`MergeStart`·`MergeEnd`·`OverRead`·주 스레드)은 조용한 모델 걸음으로 아무 때나 끼울 수 있다. 워커별 `found` 는 그 워커의 `Mark` 기록으로 고정한다. 감도: 같은 trace 를 `leave_first` 변이 모델(`TraceCheck_leave_first.cfg`)에도 돌린다.

| 날짜 | 대상 | 실행 | PR 모델(MUT="pr") | 변이 모델(leave_first, 감도) | 행동 횟수 | 겹친 쌍 |
|---|---|---|---|---|---|---|
| 2026-11-20 (.52) | head `efca4544a` + instrument.patch, 워커 2 (13회)·3 (17회), OID 분할 무작위, 10회는 한 워커가 인덱스 단계에서 실패 | 30 | **30/30 CONFORMS** | 27 DIVERGES · 3 CONFORMS | 582 — Mark 139 · EndMarks 62 · Leave 62 · WaitPass 50 · WaitStop 12 · TakePart 90 · Load 40 · OverTry 50 · Close 77 | 494 (실행당 1~35) |

- 변이 모델이 설명하는 3회는 첫 OID 전에 실패한 워커와, 그 interrupt 를 보고 기록 없이 멈춘 다른 워커뿐인 실행(행동 2개)이다 — 장벽까지 간 실행은 모두 변이 모델로 설명되지 않는다(EndMarks 다음 Leave 순서가 기록으로 고정된다).
- 겹친 구간이 실행마다 있었다(두 워커의 Load·TakePart·OverTry, 마지막 Leave 와 다른 워커의 Wait 가 겹침) — TLC 가 그 안의 모든 순서를 시도해 설명했다. 그래서 장벽·fetch_add·CAS 의 대응표는 실제 엇갈림으로 확인됐다. 다만 오류 경로·행 읽기·목록 합치기는 계측하지 못하는 px_scan_task.cpp 쪽이라 조용한 걸음이다(그 대응은 사람 주장 그대로).
- Specula MCP `tracedebugger` 는 쓰지 않았다 — DIVERGES 가 없어 원인 찾기가 필요 없었다.

## 모델이 보지 않는 것
- **힙 단계의 페이지 래치를 쥔 채 행 처리** — `process_bitmap_row ()`(px_scan_index_leaf_slot_walker.cpp)가 `cache_last_fix_page = true` 로 힙 페이지를 쥔 채 데이터 필터를 평가하고, 행이 `row_body ()`(조인·dptr·결과 쓰기)로 나가는 동안에도 그 페이지를 놓지 않는다(`next_bitmap_row ()` 가 슬롯이 바닥날 때 놓음). 다른 리뷰어가 직렬 경로로 지적한 래치 교착(놓친 지적 #4)의 병렬판 자리다 — 래치는 이 모델에 없다.
- 단일 스레드 상태기계: 직렬 bitmap 경로(`scan_index_bitmap_*`·`scan_next_index_scan` 의 S_BEFORE/S_ON/S_END)·NL batch(`nl_slice`)·`scan_gby_skip_note_key_start`·`scan_reset_scan_block`.
- 표가 찬 뒤의 전환(`switched`·기록 사슬·`unit_of`·`nsub > 1` 의 하위 파티션)과 넓은 슬롯 목록의 실제 qfile 동작(append·정렬·DISTINCT) — 모델은 "공개된 OID 집합" 으로 추상화했다. trace 실행도 표가 차지 않는 크기(174 항목, 페이지 4개)와 넓은 슬롯 OID 없이 돌았다.
- 취소의 지연: `bitmap_heap_step` 의 파티션 루프 끝(:1474)과 `take_over` 사이에 공유 interrupt 확인이 없어, 다른 워커가 이미 실패했어도 마지막 워커가 목록 합치기·정렬을 한다(결과에는 영향 없음 — 주 스레드가 오류를 돌려줌).
- `INST_NUM_SATISFIED`(LIMIT 충족)·`USER_INTERRUPTED` 의 구분 — 모두 "interrupt 있음" 하나로 본다. trace 통계(`m_bitmap_fixes`)·통계 병합.
