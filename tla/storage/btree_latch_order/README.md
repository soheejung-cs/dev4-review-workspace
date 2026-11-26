# btree_latch_order — key filter 부질의가 만드는 페이지 래치 순서 역전 (CBRD-28421)

- 이슈: CBRD-28421(key filter 안의 부질의가 리프를 쥔 채 다른 인덱스·힙 페이지를 잡아 래치 순서가 뒤집힘). 2026-11-26 분석(.50), 기준 develop `3f4816831`.
- 종류: 동시성(교착). 질문은 "이슈의 경우 A 말고 같은 꼴로 순서가 보장되지 않는 경로가 더 있나, 수정 후보 1(부질의 항을 key filter 에서 data filter 로)이 그것을 모두 없애나".
- 실행:
  - 대표 조합: `python3 tools/tla/tlc_run.py tla/storage/btree_latch_order/MC_<split|queue|bulk|heapscan>.tla --config tla/storage/btree_latch_order/MC_<이름>_<before|after>.cfg --expect <violation|ok>` — before 는 `violation`(deadlock), after 는 `ok`.
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
| `SPLIT1`·`MERGE` | Root W → L1 W → L2 W | btree.c:18874 `btree_get_next_page ()`, :37105 병합의 오른쪽 페이지 |
| `HUPD` | H W | 힙 갱신 |
| `HFS1/2` | H R → (부질의) Root R → 리프 R | scan_manager.c:6124 힙 스캔의 `eval_data_filter ()` 가 힙 페이지를 쥔 채 |
| `BULK1/2` | H W → Root R → 리프 W | locator_sr.c:16832 `cache_last_fix_page = true` + `home_hint`, heap_file.c:26874 — 새 힙 페이지를 쥔 채 `locator_insert_force ()` 의 인덱스 삽입. 부르는 곳은 loaddb(load_server_loader.cpp:1056) 하나 |
| `KFxy` | Root R → 리프 x R → (Root 놓음) → **부질의: Root R → 리프 y R** → … | btree.c:21991 `eval_key_filter ()` 가 리프를 쥔 채 |
| `KFHx` | Root R → 리프 x R → **부질의: H R** | 같음, 부질의가 힙을 읽음 |

`FIX = "datafilter"`(수정 후보 1)에서는 `KF*` 가 리프를 놓은 뒤 힙 페이지를 잡고 부질의를 돈다 — scan_manager.c:7035 인덱스 스캔의 `eval_data_filter ()` 처럼 리프를 먼저 놓는 꼴.

## 성질
- `TypeOK`, `LatchSafe`(W 는 혼자, R 과 W 는 함께 없음) — 모든 판에서 성립.
- 교착 없음(TLC deadlock 검사) — 끝나지 않은 스레드가 있는데 아무도 움직일 수 없는 상태가 없다.

## 결과 — 전수(develop)
17개 프로그램(`BULK` 제외), 크기 4까지: 4,331 조합 중 **교착 32**. `BULK` 를 넣은 14개, 크기 3: 623 조합 중 교착 6(그중 `BULK` 4). 최소 꼴은 넷이다.

| 꼴 | 최소 조합 | 순환 | 대표 cfg |
|---|---|---|---|
| ① 루트 W 쓰기 | `KFxy` + `SPLIT1`·`MERGE`·`INSWy` (2스레드, 12조합) | KF: 리프 y R 쥠 → Root R 대기 / 쓰기: Root W 쥠 → 리프 y W 대기. **split 이 없어도** 삽입이 W 모드로 재시작하기만 하면 된다 | `MC_split` (이슈의 경우 A) |
| ② 대기열 공정성 | `KF11` + `INS1` + `INSW2` (3), `FWD` + `INS1` + `INS2` + `KF21` (4) | 루트·리프가 R 로만 잡혀 있어도 W 대기자가 줄에 있으면 R 요청이 줄을 선다. 4스레드 꼴은 **SMO·루트 W 없이** 평범한 삽입 둘: FWD 가 L1 쥐고 L2 대기(INS2 의 W 뒤), KF21 이 L2 쥐고 부질의로 L1 대기(INS1 의 W 뒤) | `MC_queue` |
| ③ 힙↔인덱스 (힙 스캔) | `KFH1` + `HFS1` + `HUPD` + `INS1`·`SPLIT1`·… (4, 16조합) | KFH 가 리프 쥐고 힙 대기(HUPD 의 W 뒤) / HFS 가 힙 쥐고 data filter 부질의로 그 리프 대기(INS 의 W 뒤) | `MC_heapscan` |
| ④ 힙↔인덱스 (loaddb) | `BULK1` + `KFH1` (2) | loaddb 가 새 힙 페이지 W 를 쥔 채 리프 W 대기 / KFH 가 그 리프 R 을 쥔 채 그 힙 페이지 R 대기 | `MC_bulk` |

교착이 없는 것: 기존 프로그램끼리(`FWD`·`DESC`·`INS*`·`INSW*`·`SPLIT1`·`MERGE`·`HUPD`·`HFS*`·`BULK*`)만의 조합은 교착 0(`BULK` 는 크기 3, 나머지는 크기 4까지) — 모든 교착에 `KF*`/`KFH*` 가 들어 있다. 2스레드의 `INS` + `KF` 도 교착이 없다(②는 대기자가 하나 더 있어야 한다).

## 결과 — 수정 후보 1 (`datafilter`)
같은 17개 크기 4: 5,967 조합 교착 0. `BULK` 를 넣은 14개 크기 3: 665 조합 교착 0, 16개 크기 4: 4,828 조합 교착 0. 이유: 남는 "쥔 채 잡기" 는 위→아래(Root→리프), 왼→오른(리프→리프), 힙→인덱스(`HFS`·`BULK`·data filter 부질의)뿐이고 리프→루트·리프→힙 간선이 사라져 순서가 한 방향이 된다.

| 날짜 | 대상 | cfg | 판정 | 상태(생성/고유) | 깊이 | 도구 |
|---|---|---|---|---|---|---|
| 2026-11-26 (.50) | develop | MC_split_before.cfg | **교착**(기대대로) — 8상태 | 62 / 45 | 11 | TLC2 (tla2tools v1.7.4), Java 11 |
| 2026-11-26 (.50) | 후보 1 | MC_split_after.cfg | 성립 | 136 / 85 | 19 | 같음 |
| 2026-11-26 (.50) | develop | MC_queue_before.cfg | **교착**(기대대로) — 14상태 | 4,783 / 1,685 | 15 | 같음 |
| 2026-11-26 (.50) | 후보 1 | MC_queue_after.cfg | 성립 | 10,798 / 3,424 | 29 | 같음 |
| 2026-11-26 (.50) | develop | MC_heapscan_before.cfg | **교착**(기대대로) — 11상태 | 2,375 / 925 | 13 | 같음 |
| 2026-11-26 (.50) | 후보 1 | MC_heapscan_after.cfg | 성립 | 6,108 / 2,056 | 25 | 같음 |
| 2026-11-26 (.50) | develop | MC_bulk_before.cfg | **교착**(기대대로) — 8상태 | 92 / 60 | 14 | 같음 |
| 2026-11-26 (.50) | 후보 1 | MC_bulk_after.cfg | 성립 | 118 / 73 | 17 | 같음 |
| 2026-11-26 (.50) | develop / 후보 1 | 전수 17개 K=4 | 교착 32 / 0 | 조합 4,331 / 5,967 | — | enumerate.py |
| 2026-11-26 (.50) | develop / 후보 1 | 전수 `BULK` 포함 14개 K=3 | 교착 6 / 0 | 조합 623 / 665 | — | 같음 |
| 2026-11-26 (.50) | 후보 1 | 전수 `BULK` 포함 16개 K=4 | 교착 0 | 조합 4,828 | — | 같음 |

## 모델이 보지 않는 것
- 트리는 2단이다. 이슈의 경우 B(질의가 루트·오른쪽 리프 R, vacuum 병합이 루트·왼쪽 리프 W 를 쥔 꼴)는 루트를 R·W 로 함께 쥘 수 없어 2단에서는 그대로 나오지 않는다 — 부모가 서로 다른 3단 이상의 꼴로 본다. 같은 계열(리프를 쥔 채 왼쪽·위를 기다림)은 ①의 `MERGE` 조합으로 나오고, 후보 1 이 리프→위·리프→왼쪽 간선을 없애므로 함께 사라진다고 보지만 3단 모델로 확인하지는 않았다.
- 힙 페이지는 하나다. 힙→힙 순서는 `pgbuf_ordered_fix ()` 와 `PGBUF_IS_PAGE_CHANGED` 재확인이 지킨다고 두었다. 후보 1 에서 data filter 의 부질의가 다른 힙 페이지를 읽는 경로(힙 쥔 채 힙)는 그 장치에 기댄다.
- btree 안의 다른 경로(`btree_key_lock_object ()`·`btree_fk_object_does_exist ()`)는 힙을 읽거나 락을 잡기 전에 리프를 놓으므로 넣지 않았다. 커버링 스캔이 리프를 쥔 채 리스트 파일에 쓰는 것(btree → 임시 파일·디스크 페이지)은 split 의 할당 순서(btree → 파일 → 디스크)와 같은 방향이라 넣지 않았다(코드로만 확인).
- 대기 시간 제한·인터럽트, 락 관리자(객체 락)와 래치의 섞인 순환, `DESC` 외의 조건부 fix 는 없다.
- 모델 안에서만이다 — trace validation(코드 대조)은 하지 않았다. 경우 A 는 이슈의 gdb 스택과 같은 대기(KF 가 리프 R 쥐고 루트 R 대기, split 이 루트·L1 W 쥐고 L2 W 대기)로 나온다.
