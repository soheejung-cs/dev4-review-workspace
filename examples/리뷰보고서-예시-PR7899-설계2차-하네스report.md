# PR #7899 리뷰 보고서 (하네스 생성 골격)

**PR:** https://github.com/CUBRID/cubrid/pull/7899  **HEAD:** `1cb88447c`  **JIRA:** CBRD-27401

> **TL;DR** (Non-blocking): valid 10 (blocking 0) · inconclusive/invalid 0 · requery 0

## Findings

### [설계 리뷰]

- [설계 리뷰] [설계 판정] 🔴 `src/storage/btree.c:13777` — 팽창의 원인은 '재사용 OID 산포'가 아니라 '아직 vacuum 되지 않은 옛 버전이 자리를 차지한 채 새 버전이 옆에 추가되는 것'이다(btree_insert_object_ordered_by_oid 3966-3973: 같은 OID 를 만나면 'Just add the OID here'). COMPACT 는 그 결과(반 빈 페이지)를 사후에 정리하고, 원인은 그대로 둔다
  - 왜 문제인가: CBRD-27324 형 워크로드(같은 키에 삭제·재삽입 반복)는 COMPACT 직후 다시 50% 로 갈라지므로 수동 명령은 반복 운영 부담이 되고, 운영자가 언제 돌릴지 판단해야 한다. PG 는 같은 병(버전 churn 인덱스 팽창)을 PG14 bottom-up deletion 으로 '갈라지려는 순간 그 페이지 안의 죽은 항목을 먼저 지우는' 방식으로 예방했고, Oracle 은 인덱스 항목에 버전이 없어 삭제 항목을 블록이 공간을 필요로 할 때 회수한다 — 둘 다 삽입 시점 회수다.
  - 제안: 후속 이슈(이 PR 범위 밖)로 'split 직전 즉석 vacuum': btree_ovf_dir_grow_chain() 진입 직전(btree_ovf_dir_append_object 의 '공간 부족 → grow' 분기)에 새 helper btree_ovf_page_prune_dead(page) 를 호출해, 그 페이지의 객체 중 delete MVCCID 가 있고 MVCC_ID_PRECEDES(delid, log_Gl.mvcc_table.get_global_oldest_visible()) 인 것(= 어떤 활성 스냅샷에도 안 보임, 곧 vacuum 대상)을 한 sysop 으로 물리 제거(RVBT_RECORD_MODIFY_UNDOREDO, 오늘 컴팩션과 같은 로깅)한 뒤 다시 공간을 재고, 그래도 부족할 때만 split. 재사용 OID 의 새 버전은 정렬상 옛 버전 자리에 들어가므로 페이지가 자라지 않는다. 계약 확인: (1) vacuum 은 delete MVCCID 정확 일치로 지우고(btree.c:15343-15347) 못 찾으면 '이미 vacuum 됨' 경고로 지나간다(35325-35345) — 즉 선제 제거가 log-driven vacuum 과 호환되되 경고 로그가 남으므로 prune 한 객체 수를 vacuum 이 알 수 있게 헤더 플래그 하나(예: BTREE_OVF_PAGE_PRUNED) 또는 경고 등급 하향이 필요. (2) separator 불변조건은 '삭제는 절대 separator 를 무효화하지 않는다'(btree.c:12356 주석) 라 라우팅에 영향 없음. (3) 비용은 split 경로(드묾)에서 페이지 1회 스캔 + 로그 1건. (4) 리프 레코드 객체에도 같은 훅을 두면 리프 팽창까지 예방. 대안표: 수동 COMPACT(현 PR, 사후 정리) / vacuum 시 병합(작성자 후속안, C 등급 데몬에 쓰기 추가) / split 시 즉석 vacuum(예방, 삽입 경로 소폭 비용). 권고: 이 PR 은 그대로 머지하고, 즉석 vacuum 을 CBRD 신규 발번으로 — COMPACT 의 역할을 '이미 팽창한 볼륨의 1회 정리'로 문서에 못 박는다.
  - 검증: [static] btree_insert_object_ordered_by_oid 3966-3973 같은 OID 추가 확인; vacuum 매칭 15343-15347 delid 정확 일치; 미발견 관용 35325-35345; 전역 최소 가시 MVCCID API mvcc_table.hpp:90 (vacuum.c:3281 이 같은 임계값 사용); PG16 nbtdedup.c _bt_bottomupdel_pass, nbtree README L567; Oracle SQL Ref 10-90 COALESCE(삭제 항목 회수는 Admin Guide/Concepts 범위 — 매뉴얼 미확인)
  - status **valid**, non-blocking, rules -
- [설계 리뷰] [측정 요청] 🟡 `src/communication/network.h:202` — 성능 변경인데 기능 회귀 테스트(CTP) 통과 근거가 본문에 없다
  - 왜 문제인가: 정확성이 성능보다 먼저다 — 회귀 테스트 근거가 없으면 빨라진 코드가 틀린 답을 내는지 아무도 확인하지 않은 상태로 머지된다.
  - 제안: `/run all` 결과 또는 CTP sql/medium 실행 결과(코어 0, NOK 분류)를 본문 Verification 에 한 줄로. 예: "CTP sql 통과, medium NOK 2건은 기존 답안 차이".
  - status **valid**, non-blocking, rules ['MEAS-05']
- [설계 리뷰] [설계 판정] 🟡 `src/query/execute_schema.c:4403` — CUBRID 의 DDL 은 트랜잭션 안에서 롤백 가능한데 COMPACT 는 병합마다 sysop 을 독립 커밋하는 '되돌릴 수 없는 첫 ALTER INDEX' 다 — 문 뒤에 ROLLBACK 해도 페이지는 돌아오지 않고, 반대로 SCH_S 는 커밋까지 남아 DROP/ALTER INDEX 를 막는다(보호할 것이 없는데). Oracle 은 DDL 이 자동 커밋이라 이 불일치가 없다
  - 왜 문제인가: 오토커밋 OFF 사용자가 ROLLBACK 을 기대하면 예상과 다르고(REBUILD 는 스키마 트랜잭션이라 롤백된다), 긴 트랜잭션 안에서 COMPACT 를 돌리면 SCH_S 가 불필요하게 오래 남아 DDL 대기를 만든다.
  - 제안: 둘 중 하나: (a) 매뉴얼에 'COMPACT 는 즉시 반영되며 ROLLBACK 으로 되돌릴 수 없다(REBUILD 와 다름)' 명시 + SCH_S 를 문 끝에서 놓는 것을 검토(sysop 이 이미 커밋돼 보호 대상 없음), (b) UPDATE STATISTICS 류처럼 암묵 커밋 대상으로 분류. 어느 쪽이든 문서 한 줄은 필수.
  - 검증: [static] 4273-4274 주석: AU_FETCH_READ SCH_S 는 트랜잭션 끝까지; 14610 log_sysop_commit 은 호출 트랜잭션과 무관히 확정(PR 본문·주석). REBUILD 의 롤백 가능 여부는 확인 질문으로 남김
  - status **valid**, non-blocking, rules -
- [설계 리뷰] [설계 판정] 🟡 `src/storage/btree.c:14473` — 정리 시점을 '관리자 수동 명령'으로 고정했다. PG 는 페이지가 갈라지려는 순간(split 직전) 그 페이지 안에서 죽은 항목을 먼저 회수하는 bottom-up deletion(PG14, nbtree README §Bottom-up deletion)으로 팽창을 예방하고, 빈 페이지만 VACUUM 이 삭제한다(README: 'Page deletion always begins from an empty leaf page'). Oracle 은 수동 COALESCE + Segment Advisor 권고다
  - 왜 문제인가: 수동 명령은 '누가 언제 돌리나'가 운영 부담이고, 27324 처럼 재사용 OID 가 흩어지는 워크로드는 정리 직후 다시 50% 로 갈라진다. split 시점의 국소 병합(오른쪽 이웃에 남은 공간이 있으면 갈라지기 전에 먼저 당겨 채움)은 명령 없이 팽창을 막고 비용은 split 경로(드묾)에만 든다.
  - 제안: 후속 이슈로 btree_ovf_dir_grow_chain() 의 split 직전에 '이웃 흡수 시도'(오늘 btree_ovf_compact_pull 을 그대로 재사용) 를 설계 대안으로 검토. 대안표: 수동 COMPACT(현 PR) / vacuum 시 병합(작성자 후속안, C 등급 데몬에 쓰기 추가) / split 시 국소 병합(PG bottom-up 유사, 삽입 경로 비용 소폭). 이 PR 범위는 유지하고 매뉴얼에 '언제 돌리나'(free 30% 기준) 만 명시.
  - 검증: [static] PG16 nbtree README L241·L567, nbtdedup.c _bt_bottomupdel_pass 확인; Oracle SQL Ref 10-90 COALESCE 는 수동
  - status **valid**, question, rules -
- [설계 리뷰] [설계 판정] 🟡 `src/storage/btree.c:15101` — COMPACT 는 인덱스의 leafs/pages(BTREE_STATS 는 오버플로 페이지를 leafs 에 포함)를 최대 수 배 바꾸지만 카탈로그 통계를 갱신하지 않아, 플래너(qo_iscan_cost 의 leaves = ceil(sel * cum_stats.leafs))는 컴팩션 뒤에도 인덱스를 팽창된 크기로 비용 계산한다
  - 왜 문제인가: 컴팩션의 목적이 인덱스 크기·I/O 를 줄이는 것인데 옵티마이저는 그 사실을 UPDATE STATISTICS 전까지 모른다 — 인덱스 스캔 비용이 과대평가되어 정리 직후에도 옛 플랜(풀스캔·다른 인덱스)이 유지된다. PG 는 VACUUM 의 페이지 삭제가 relpages 로 바로 반영되고, Oracle 은 COALESCE 뒤 DBMS_STATS 가 필요하다고 문서화한다.
  - 제안: xbtree_compact_overflow() 끝에서 pages_freed 만큼 BTREE_STATS.pages/leafs 를 카탈로그에서 감산(정확하고 싸다) 하거나, 최소한 매뉴얼에 'COMPACT 후 UPDATE STATISTICS ON t' 를 적는다. 확인 질문: REBUILD 는 통계를 갱신하는가(같은 정책이어야 한다).
  - 검증: [static] statistics.h:70 leafs 는 오버플로 포함; query_planner.c:2331 leaves = ceil(sel*leafs); PR diff 에 통계 갱신 호출 없음(statistics_sr/catalog 변경 0)
  - status **valid**, non-blocking, rules -
- [설계 리뷰] [문서 제안] 🟢 `src/parser/csql_grammar.y:3902` — 이름은 COMPACT(인덱스 전체 함의)인데 실제 범위는 비유니크 인덱스의 오버플로 OID 체인만이다 — 대량 삭제 뒤 반쯤 빈 리프 페이지는 그대로 남고(PG 는 빈 리프만 삭제, Oracle COALESCE 는 리프 블록 병합) REBUILD 만이 답이다
  - 왜 문제인가: 사용자는 'COMPACT 를 돌렸는데 인덱스가 안 줄었다'고 보고하게 된다 — 리프 팽창은 이 명령의 범위 밖이라는 사실을 문법·매뉴얼이 말해 주지 않는다.
  - 제안: 매뉴얼 첫 문장에 범위 명시: 'COMPACT 는 중복 키의 오버플로 OID 체인만 정리한다. 리프 페이지 밀도(대량 삭제 후)는 ALTER INDEX ... REBUILD 로.' 확장 여지를 남기려면 문법은 그대로 두고 매뉴얼에 '향후 리프 병합도 같은 명령으로' 를 적어 둔다.
  - 검증: [static] xbtree_compact_overflow 는 BTREE_LEAF_RECORD_OVERFLOW_OIDS 키만 처리(15067-15072), 리프 병합 경로 없음
  - status **valid**, non-blocking, rules -
- [설계 리뷰] [주석 제안] 🟢 `src/storage/btree.c:15038` — 리프 순회가 현재 리프 WRITE 래치를 든 채 다음 리프를 WRITE 로 잡는다(좌→우 WRITE-WRITE 커플링). 오름차순 범위 스캔은 READ-READ 커플링이고, 리프 병합(vacuum/delete 경로의 btree_merge_node 계열)이 이웃 리프를 무조건 래치로 잡는다면 '컴팩션: L 보유·L+1 대기 / 병합: L+1 보유·L 대기' 순서 역전이 성립한다 — 이웃 fix 가 CONDITIONAL 인지 확인 필요
  - 왜 문제인가: 페이지 래치에는 데드락 감지기가 없어 순서 역전이 나면 두 스레드가 pgbuf_fix 안에서 영원히 잠들고, 그 리프에 닿는 DML·스캔·(SCH_S 때문에) DDL 까지 줄줄이 걸린다 — 서버 재시작 외 회복 불가.
  - 제안: 리프 커플링 순서 규약 주석을 next_leaf pgbuf_fix 위에(suggestion 게시함): "leaves are coupled strictly left to right ... never fix a left neighbour while holding a right one".
  - 검증: [static] btree_merge_node_and_advance(btree.c:33644~)도 부모를 든 채 left_vpid→right_vpid 순서로 UNCONDITIONAL fix — 방향 동일, 역전 없음. 질문 철회.
  - status **valid**, question, rules -
- [설계 리뷰] [설계 판정] 🟢 `src/storage/storage_common.h:317` — FILL_FACTOR 를 명령 인자로만 둔다. PG 의 fillfactor 는 인덱스 저장 속성(BTREE_DEFAULT_FILLFACTOR 90, ALTER INDEX SET)이라 CREATE/REINDEX 와 rightmost split 이 같은 값을 쓰고, Oracle 은 PCTFREE 가 생성 속성이다. CUBRID 는 COMPACT 로 90% 를 만든 직후 다음 split 이 다시 50/50 으로 갈라 정책이 어긋난다
  - 왜 문제인가: 채움 비율이 명령마다 다르면 같은 인덱스가 실행자에 따라 다른 밀도가 되고, 벌크 적재(btree_load)·split 은 이 값을 모른다. 속성으로 두면 COMPACT 기본값·향후 split/적재 정책이 한 곳에서 결정된다.
  - 제안: CREATE/ALTER INDEX ... WITH FILL_FACTOR = n 을 인덱스 속성(SM_CLASS_CONSTRAINT 의 속성, 기본 90)으로 저장하고 COMPACT 는 인자 없으면 그 값을, 있으면 이번 실행만 override. 후속 이슈로 split(btree_ovf_dir_grow_chain)과 btree_load 가 같은 값을 참조.
  - 검증: [static] postgres-16 nbtree.h:200 BTREE_DEFAULT_FILLFACTOR 90; Oracle SQL Ref 10-90 COALESCE 는 채움 인자 없음(PCTFREE 생성 속성)
  - status **valid**, non-blocking, rules -

### [코드 리뷰]

- [코드 리뷰] [버그 가능성] 🟡 `src/storage/btree.c:14594` — 부분 이동의 separator 후퇴가 이웃 페이지 앞부분이 '같은 재사용 OID 의 미청소 버전 런' 하나로만 채워져 있으면 num_move 를 0 까지 내려 그 페이지 쌍을 건너뛴다 — 런이 한 페이지 이상 이어지는 체인은 컴팩션이 조용히 아무 일도 하지 않고(work_done=false) 결과 보고에도 드러나지 않는다
  - 왜 문제인가: CBRD-27324 의 재사용 OID 워크로드가 바로 이 형상이라 명령이 가장 필요한 곳에서 조용히 무동작이 되고, 결과 카운트도 0 이라 사용자는 원인을 알 수 없다.
  - 제안: pairs_skipped_no_boundary 카운터 + er_log_debug 결과 줄, 매뉴얼 진단 절에 "vacuum 후 재실행" 한 줄.
  - 검증: [static] num_move 후퇴 루프(14583-14591)가 0 까지 내려갈 수 있고 그 경우 work_done 이 false 로 남는 것을 코드로 확인. 동적 재현은 vacuum 정지가 필요해 TC 제안으로 대체.
  - status **valid**, non-blocking, rules -
- [코드 리뷰] [주석 제안] 🟢 `src/storage/btree.c:29811` — OID 앵커 재개의 정확성은 이 호출과 재고정(29890) 사이에 리프 READ 래치(C_page)가 유지되어 컴팩션이 끼어들 수 없다는 데 기댄다(확인함: 문제 없음). 전제를 주석으로 남기면 나중에 리프를 먼저 놓는 최적화가 이 경로를 깨뜨리는 일을 막는다
  - 왜 문제인가: 전제가 깨지면 resume_offset 이 옮겨진 객체를 가리켜 스캔이 행을 누락·중복한다 — 에러 없는 오답. 지금은 성립하므로 주석 요청.
  - 제안: btree_ovf_scan_locate_resume 호출 위 주석(suggestion 게시함): C_page READ 래치가 재고정까지 유지되어야 한다는 전제.
  - 검증: [static] bts->C_page 가 함수 진입 assert(29698)로 고정되어 있고 재고정(29890)까지 놓는 경로 없음 — 전제 성립.
  - status **valid**, non-blocking, rules -

### 판정 보류 (requery.json)

없음

## 아키텍처 리스크 (결정론 탐지)


## 돌릴 것 (제안 — 요청자 확인 후)

- 변경 계층 ['communication', 'parser', 'query', 'storage'] → review-testing 매트릭스로 CTP/동시성/JOB/TPC-H 제안

_manifest: harness b08c8d1, model unset_