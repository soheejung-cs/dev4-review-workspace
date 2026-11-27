# btree_latch_order — key filter 부질의가 만드는 페이지 래치 순서 역전 (CBRD-28421)

- 이슈: CBRD-28421(key filter 안의 부질의가 리프를 쥔 채 다른 인덱스·힙 페이지를 잡아 래치 순서가 뒤집힘). 2026-11-26 분석(.50), 기준 develop `3f4816831`.
- 종류: 동시성(교착). 질문은 "이슈의 경우 A 말고 같은 꼴로 순서가 보장되지 않는 경로가 더 있나, 수정 후보 1(부질의 항을 key filter 에서 data filter 로)이 그것을 모두 없애나".
- 실행:
  - 대표 조합: `python3 tools/tla/tlc_run.py tla/storage/btree_latch_order/MC_<split|merge|queue|bulk|heapscan>.tla --config tla/storage/btree_latch_order/MC_<이름>_<before|after>.cfg --expect <violation|ok>` — before 는 `violation`(deadlock), after 는 `ok`.
  - 전수: `python3 tla/storage/btree_latch_order/enumerate.py <작업 디렉터리> <develop|datafilter> <K> <프로그램,...> --jobs N` — 크기 2..K 의 프로그램 다중집합을 모두 돌리고(이미 교착난 조합을 포함하는 것은 건너뜀) `<FIX>.tsv` 에 교착 상태의 잡은 것/기다리는 것을 적는다.

## 무엇을 모델로 했나
페이지는 루트 `Root`, 리프 `L1`·`L2`(왼쪽→오른쪽), 힙 페이지 `H` 넷이다. 스레드마다 프로그램 하나(`F` 무조건 fix, `FC` 조건부 fix, `U` unfix 의 열)를 돈다.
pgbuf 의 래치 규칙은 그대로 옮겼다: 잡은 스레드의 재 fix 는 허용, 빈 페이지는 허용, **READ 위의 READ 는 대기열이 비어 있을 때만**(대기자가 있으면 줄을 선다), 마지막 holder 가 놓으면 맨 앞을 깨우고 맨 앞이 reader 면 줄 선 reader 를 모두 깨운다. 조건부 fix 가 실패하면 그 스레드는 잡은 것을 놓고 끝난다(`ER_DESC_ISCAN_ABORTED`). 대기 시간 제한은 없다 — TLC 의 교착 검사가 판정이다.

| 프로그램 | 순서 | 코드 (develop `3f4816831`) |
|---|---|---|
| `FWD` | Root R → L1 R → (Root 놓음) → L2 R(무조건, L1 쥔 채) | btree.c:21589 다음 리프 `pgbuf_fix` |
| `DESC` | Root R → L2 R → L1 R **조건부** | btree.c:21575 `PGBUF_CONDITIONAL_LATCH` |
| `INS1/2` | Root R → 리프 W | btree.c:33592 부모를 쥔 채 자식 fix |
| `INSW1/2` | Root W → 리프 W (split·긴 키 뒤 재시작) | btree.c:33385 등 `nonleaf_latch_mode = PGBUF_LATCH_WRITE` |
| `SPLIT1`·`MERGE` | Root W → L1 W → L2 W | btree.c:18874 `btree_get_next_page ()`, :37105 병합의 오른쪽 페이지(부모 W 로 재시작한 뒤) |
| `MERGER` | Root **R** → L1 W → L2 W → (부모 승격, 실패하면 놓고 재시작) | `btree_merge_node_and_advance ()` 첫 시도: 부모는 `nonleaf_latch_mode`(READ), 자식 리프는 W(btree.c:37032), 오른쪽 이웃 W(:37105), 그다음 `PGBUF_PROMOTE_ONLY_READER` 승격 — vacuum·delete |
| `HUPD` | H W | 힙 갱신 |
| `HFS1/2` | H R → (부질의) Root R → 리프 R | scan_manager.c:6124 힙 스캔의 `eval_data_filter ()` 가 힙 페이지를 쥔 채 — **fixed 스캔일 때만**(heap_scancache 의 `cache_last_fix_page = fixed`, scan_manager.c:4313). 부질의·저장 함수가 있는 블록은 fixed 를 끄도록 되어 있어, 이 프로그램은 그 판단이 빠진 자리(CBRD-28426 표 A·B·C·H)를 나타낸다 |
| `BULK1/2` | H W → Root R → 리프 W | locator_sr.c:16832 `cache_last_fix_page = true` + `home_hint`, heap_file.c:26874 — 새 힙 페이지를 쥔 채 `locator_insert_force ()` 의 인덱스 삽입. 부르는 곳은 loaddb(load_server_loader.cpp:1056) 하나 |
| `KFxy` | Root R → 리프 x R → (Root 놓음) → **부질의: Root R → 리프 y R** → … | btree.c:21991 `eval_key_filter ()` 가 리프를 쥔 채 |
| `KFHx` | Root R → 리프 x R → **부질의: H R** | 같음, 부질의가 힙을 읽음 |

`FIX = "datafilter"`(수정 후보 1)에서는 `KF*` 가 리프를 놓은 뒤 힙 페이지를 잡고 부질의를 돈다 — scan_manager.c:7035 인덱스 스캔의 `eval_data_filter ()` 처럼 리프를 먼저 놓는 꼴.

## 성질
- `TypeOK`, `LatchSafe`(W 는 혼자, R 과 W 는 함께 없음) — 모든 판에서 성립.
- 교착 없음(TLC deadlock 검사) — 끝나지 않은 스레드가 있는데 아무도 움직일 수 없는 상태가 없다.

## 결과 — 전수(develop)
17개 프로그램(`BULK`·`MERGER` 제외), 크기 4까지: 4,331 조합 중 **교착 32**. `BULK` 를 넣은 14개, 크기 3: 623 조합 중 교착 6(그중 `BULK` 4). `MERGER` 를 넣은 14개, 크기 3: 577 조합 중 교착 10(그중 `MERGER` 2 — 2스레드 `KF21`, 3스레드 `INSW1`+`KF22`). 최소 꼴은 다섯이다.

| 꼴 | 최소 조합 | 순환 | 대표 cfg |
|---|---|---|---|
| ① 루트 W 쓰기 | `KFxy` + `SPLIT1`·`MERGE`·`INSWy` (2스레드, 12조합) | KF: 리프 y R 쥠 → Root R 대기 / 쓰기: Root W 쥠 → 리프 y W 대기. **split 이 없어도** 삽입이 W 모드로 재시작하기만 하면 된다 | `MC_split` (이슈의 경우 A) |
| ①′ 부모 READ 병합 | `KF21` + `MERGER` (2) | KF: 루트·L2 R 쥠 → 부질의가 L1 R 대기 / 병합: 루트 R·L1 W 쥠 → L2 W 대기. 루트는 둘 다 R 이라 함께 쥘 수 있다 | `MC_merge` (이슈의 경우 B, SQLancer 코어와 같은 대기) |
| ② 대기열 공정성 | `KF11` + `INS1` + `INSW2` (3), `FWD` + `INS1` + `INS2` + `KF21` (4) | 루트·리프가 R 로만 잡혀 있어도 W 대기자가 줄에 있으면 R 요청이 줄을 선다. 4스레드 꼴은 **SMO·루트 W 없이** 평범한 삽입 둘: FWD 가 L1 쥐고 L2 대기(INS2 의 W 뒤), KF21 이 L2 쥐고 부질의로 L1 대기(INS1 의 W 뒤) | `MC_queue` |
| ③ 힙↔인덱스 (힙 스캔) | `KFH1` + `HFS1` + `HUPD` + `INS1`·`SPLIT1`·… (4, 16조합) | KFH 가 리프 쥐고 힙 대기(HUPD 의 W 뒤) / HFS 가 힙 쥐고 data filter 부질의로 그 리프 대기(INS 의 W 뒤) | `MC_heapscan` |
| ④ 힙↔인덱스 (loaddb) | `BULK1` + `KFH1` (2) | loaddb 가 새 힙 페이지 W 를 쥔 채 리프 W 대기 / KFH 가 그 리프 R 을 쥔 채 그 힙 페이지 R 대기 | `MC_bulk` |

교착이 없는 것: 기존 프로그램끼리(`FWD`·`DESC`·`INS*`·`INSW*`·`SPLIT1`·`MERGE`·`HUPD`·`HFS*`·`BULK*`)만의 조합은 교착 0(`BULK` 는 크기 3, 나머지는 크기 4까지) — 모든 교착에 `KF*`/`KFH*` 가 들어 있다. 2스레드의 `INS` + `KF` 도 교착이 없다(②는 대기자가 하나 더 있어야 한다).

## 결과 — 수정 후보 1 (`datafilter`)
같은 17개 크기 4: 5,967 조합 교착 0. `BULK` 를 넣은 14개 크기 3: 665 조합 교착 0, 16개 크기 4: 4,828 조합 교착 0. `MERGER` 를 넣은 14개 크기 4: 3,045 조합 교착 0. 이유: 남는 "쥔 채 잡기" 는 위→아래(Root→리프), 왼→오른(리프→리프), 힙→인덱스(`HFS`·`BULK`·data filter 부질의)뿐이고 리프→루트·리프→힙 간선이 사라져 순서가 한 방향이 된다.

| 날짜 | 대상 | cfg | 판정 | 상태(생성/고유) | 깊이 | 도구 |
|---|---|---|---|---|---|---|
| 2026-11-26 (.50) | develop | MC_split_before.cfg | **교착**(기대대로) — 8상태 | 62 / 45 | 11 | TLC2 (tla2tools v1.7.4), Java 11 |
| 2026-11-26 (.50) | 후보 1 | MC_split_after.cfg | 성립 | 136 / 85 | 19 | 같음 |
| 2026-11-27 (.50) | develop | MC_merge_before.cfg | **교착**(기대대로) — 9상태 | 93 / 59 | 11 | 같음 |
| 2026-11-27 (.50) | 후보 1 | MC_merge_after.cfg | 성립 | 166 / 96 | 19 | 같음 |
| 2026-11-26 (.50) | develop | MC_queue_before.cfg | **교착**(기대대로) — 14상태 | 4,783 / 1,685 | 15 | 같음 |
| 2026-11-26 (.50) | 후보 1 | MC_queue_after.cfg | 성립 | 10,798 / 3,424 | 29 | 같음 |
| 2026-11-26 (.50) | develop | MC_heapscan_before.cfg | **교착**(기대대로) — 11상태 | 2,375 / 925 | 13 | 같음 |
| 2026-11-26 (.50) | 후보 1 | MC_heapscan_after.cfg | 성립 | 6,108 / 2,056 | 25 | 같음 |
| 2026-11-26 (.50) | develop | MC_bulk_before.cfg | **교착**(기대대로) — 8상태 | 92 / 60 | 14 | 같음 |
| 2026-11-26 (.50) | 후보 1 | MC_bulk_after.cfg | 성립 | 118 / 73 | 17 | 같음 |
| 2026-11-26 (.50) | develop / 후보 1 | 전수 17개 K=4 | 교착 32 / 0 | 조합 4,331 / 5,967 | — | enumerate.py |
| 2026-11-26 (.50) | develop / 후보 1 | 전수 `BULK` 포함 14개 K=3 | 교착 6 / 0 | 조합 623 / 665 | — | 같음 |
| 2026-11-26 (.50) | 후보 1 | 전수 `BULK` 포함 16개 K=4 | 교착 0 | 조합 4,828 | — | 같음 |
| 2026-11-27 (.50) | develop / 후보 1 | 전수 `MERGER` 포함 14개 K=3 / K=4 | 교착 10 / 0 | 조합 577 / 3,045 | — | 같음 |

## 코드 대조 — 페이지를 쥔 채 식을 평가하는 자리 (2026-11-27, 코드 읽기만)
모델의 `KF`·`HFS` 같은 "쥔 채 다른 문장을 시작하는" 자리가 key filter 말고 더 있는지 develop `3f4816831` 을 훑었다. 같은 원인을 한데 모은 CBRD-28426 의 표(A~I)와 대조한 결과다.

| 자리 | 쥔 페이지 | 시작될 수 있는 것 | CBRD-28426 |
|---|---|---|---|
| `qexec_covered_visit ()`(query_executor.c:29482) — btree.c:22323 `gby->visit` 로 리프를 쥔 채 `after_join_pred`·`if_pred`·`qexec_end_one_iteration ()` | 리프 R | select 목록의 저장 함수(`qexec_covered_visit_scan ()` :29441 이 dptr·LINK_TO_REGU aptr·serial 은 막지만 TYPE_SP 는 보지 않음) | D |
| fixed 힙 스캔(scan_manager.c:4313 → heap_file.c:8306)의 행마다 `dptr_list`(query_executor.c:12799)·`after_join_pred`·`if_pred`·출력 | 힙 R | 저장 함수(fixed 금지 표시 `XASL_NO_FIXED_SCAN` 이 serial·통계 함수에만 붙음, xasl_generation.c:12969·13204~13240), 인덱스 스캔이 없는 조인의 안쪽 블록 상관 부질의(query_executor.c:23303 의 dptr 검사가 인덱스 스캔 갈래에만) | A·B |
| `locator_lock_and_get_object_with_evaluation ()`(locator_sr.c:15958) — 힙 페이지를 쥔 채 재평가(:16651·:16667·:16679), 인덱스 스캔 쪽 호출(scan_manager.c:7060)도 리프는 놓았지만 힙은 쥠 | 힙 R | WHERE 의 부질의·저장 함수 | F |
| `qexec_execute_obj_fetch ()`(query_executor.c:20565·20577·20729) | 힙 R | where_pred 의 저장 함수 | G |
| batch 스캔이 행 경로로 넘긴 페이지(scan_manager.c:6040~6060) | 힙 R | — fixed 스캔에서만 consumer 가 붙고(scan_manager.c:8122 assert), filter iterator 는 fixed 가 아니면 행마다 놓음 → fixed 금지가 빠진 A 와 같은 자리 | A |

안전으로 확인한 것: filtered index 술어(부질의·저장 함수를 의미 검사가 막음, semantic_check.c:18055), 함수 인덱스 식(parse_tree_cl.c:19462), `btree_dump_curr_key ()`·loose index scan·ISS 의 `rest_regu_list`(속성만), MRO 키 비교, fixed 가 아닌 인덱스 스캔의 힙 읽기(COPY), 키 범위 값(btree 를 잡기 전), list·set·JSON·method·dblink 스캔(질의 자신의 임시 페이지), unique·FK 검사. 결론: **CBRD-28426 표 밖의 새 촉발 자리는 찾지 못했다.** 모델이 더한 것은 촉발 자리가 아니라 순환의 상대다 — 평범한 INSERT 둘의 대기열(②), loaddb 의 힙 W → 리프 W(④), 부모 READ 병합(①′). 이들은 촉발 자리가 고쳐지면 함께 사라진다.

## 모델이 보지 않는 것
- 트리는 2단이다. 이슈의 경우 B 는 처음에 3단이 필요하다고 보았으나, 병합의 첫 시도가 부모를 READ 로 쥔다는 것(`MERGER`)을 넣자 2단에서 그대로 나왔다(2026-11-27). 3단 이상에서 생기는 간선(리프→중간 노드)은 리프→루트와 같은 방향이라 새 꼴을 만들지 않는다고 보지만 모델로 확인하지는 않았다.
- 힙 페이지는 하나다. 힙→힙 순서는 `pgbuf_ordered_fix ()` 가 지킨다(한 스레드가 쥔 힙 페이지보다 순위가 낮은 페이지를 잡을 때 쥔 것을 놓았다 다시 잡는다). 바깥 힙 스캔은 data filter 평가 전 페이지 LSA 를 적고(scan_manager.c:6116) 평가 뒤 `PGBUF_IS_PAGE_CHANGED` 로 다시 읽는다(:6131) — 코드로 확인. 다만 이 재확인은 data filter 둘레에만 있고, fixed 스캔에서 select 목록·`if_pred`·`dptr` 의 부질의·저장 함수가 바깥 페이지를 놓았다 다시 잡게 하면 PEEK 로 읽은 값이 낡을 수 있다(미검증 — fixed 금지가 빠진 자리에서만). 후보 1 에서 data filter 의 부질의가 다른 힙 페이지를 읽는 경로(힙 쥔 채 힙)는 그 장치에 기댄다.
- btree 안의 다른 경로(`btree_key_lock_object ()`·`btree_fk_object_does_exist ()`)는 힙을 읽거나 락을 잡기 전에 리프를 놓으므로 넣지 않았다. 커버링 스캔이 리프를 쥔 채 리스트 파일에 쓰는 것(btree → 임시 파일·디스크 페이지)은 split 의 할당 순서(btree → 파일 → 디스크)와 같은 방향이라 넣지 않았다(코드로만 확인).
- 대기 시간 제한·인터럽트, 락 관리자(객체 락)와 래치의 섞인 순환, `DESC` 외의 조건부 fix 는 없다.
- 모델 안에서만이다 — trace validation(코드 대조)은 하지 않았다. 경우 A 는 이슈의 gdb 스택과 같은 대기(KF 가 리프 R 쥐고 루트 R 대기, split 이 루트·L1 W 쥐고 L2 W 대기)로 나온다.
