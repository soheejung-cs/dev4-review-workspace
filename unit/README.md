# unit/ — 리뷰용 단위 테스트 라이브러리

gha-ci 용이 아니다. 리뷰 중 지적을 서버·DB 없이 동적으로 확인하려고 만든 테스트를 **모듈별로 쌓아**, 다음 PR 이 같은 파일을
건드리면 하네스(`run full` 의 `unit_regress` 단계)가 PR head 와 base 에 다시 돌린다(사용자 결정 2026-10-16, 목 허용).

## 배치
```
unit/<module>/<suite>/
  suite.json   requires(대상 심볼 — 이 리비전에 suite 가 적용되나) · file(대상 TU, repo 경로) · mode(server|cs|sa) · seams(weak 로 바꿀 함수, static 변수는 var:이름)
               · appendix(TU 끝에 붙는 훅) · test · watch(이 파일들이 바뀌어도 돌린다) · fakes(가짜가 무엇을 흉내 내나) · origin(PR·finding·코멘트)
  test.cpp     Catch2 v2. 케이스 태그로 의미를 정한다:
                 [contract]  계약 — head FAIL·base PASS 면 회귀 후보
                 [defect]    지난 리뷰의 결함 재현(PASS = 결함 있음) — head 에서 FAIL 이면 고쳐졌거나 동작이 바뀌었다
               이미 고쳐진 결함은 suite.json 에 defects_fixed_in(커밋)을 적어 이력으로 둔다 — 다시 PASS 하면 '재발' 로 보고된다.
               고쳐진 결함과 열린 결함이 한 suite 에 같이 있으면 {"케이스 이름 접두어": "커밋"} 사전으로 적는다(접두어가 맞는 케이스만 이력)
  hooks.cpp    (선택) TU 의 static 에 닿는 접근 함수
```
`<module>` 은 `src/` 아래 첫 디렉터리(query·optimizer·compat …).

## 돌리기
```bash
cd ~/dev/docs/dev4-review-workspace
python3.11 -m tools.harness.unit_regress --list                       # suite 목록
python3.11 -m tools.harness.unit_regress --head <rev> --base <rev> --files src/query/xasl_cache.c --out /tmp/x
python3.11 -m tools.harness.unitseam run --rev <rev> --file … --mode … --test … [--seam …] [--appendix …]   # suite 없이 한 번
```
`run full --pr N` 는 자동으로 돈다(`HARNESS_NO_UNIT=1` 이면 건너뜀). 결과 `OUT/unit_regress.{md,json}` 는 preamble 과 report 에 실린다.

## 쌓는 규칙
- 리뷰에서 `harness-unit` 이 만든 테스트는 **대조군 [contract] 1개 이상 + 재현 [defect]** 으로 여기에 옮긴다.
- 가짜는 실제 함수의 해당 분기를 읽고 같게 만든다 — `suite.json` 의 `fakes` 에 무엇을 흉내 냈는지 적는다. 가짜가 틀리면 재현이 무의미하다.
- 엔진이 시그니처·구조를 바꿔 suite 가 컴파일되지 않으면 `unit_regress` 가 "suite 갱신 필요" 로 보고한다 — 그때 고친다.
- 부팅된 프로세스라면 가졌을 전역 기본값은 `tools/harness/unitseam_prelude.cpp` 에 weak 로 추가한다.

## 목록
| suite | 대상 | 출처 |
|---|---|---|
| `query/xasl_cache.bind_variant` | `xcache_bind_variant` 상태기계 | PR#8013 F1 고아 예약 |
| `compat/db_vdb.bind_variant_key` | `db_bind_variant_switch` 변형 키 할당(parser 아레나) | PR#8013 F2 전환마다 키 누적 |
| `optimizer/histogram_cl.unique_pinned` | `bind_target_spec_is_unique_pinned` UNIQUE 계열 고정 판정 | PR#8013 F4 함수·필터 UNIQUE 도 고정(잠재 — 문법이 UNIQUE 함수·필터 인덱스를 거부) |
| `optimizer/histogram_cl.char_keys` | `string_key_size_for_column`·`histogram_repad_char_value`/`histogram_repad_ctx`·HV2 폭 슬롯·`histogram_extract_key` 프로브 키 | PR#7977 계약 29건(결함 없음, base 2건 동작 차이 = CBRD-27251 수정분) |
| `query/px_scan_checker.worker_sp` | 병렬 스캔 체커 `CANNOT_RUN_IN_WORKER` 전파(select list·집계·퍼센타일·뒤 조인·CONNECT BY 세 술어) | PR#8136 계약 13건 + F1 CONNECT BY 미검사 이력 4건(머지 전 4b7108f25 에서 고쳐짐) |
| `query/bind_variant.distance` | `bind_variant.h` 지문 거리·known·nearest 경계 | PR#8013 F9 비교 가능 슬롯 0개 → 1.0(도달성 미확정 — ENUM 바인드만 후보) · F25 계약 |
| `optimizer/histogram_cl.bind_watch_check` | `histogram_bind_watch_check` band·첫 기록·항 수 변화·NULL/UNKNOWN 슬롯 | PR#8013 F15 band≤1.0 이면 worst_i=-1 인덱스(잠재 — 호출자는 1.1/1.5 만) · F22 NULL 은 슬롯 유지(주석과 반대) |
| `executables/util_sa.optimizedb_options` | `optimizedb` -n / --no-histogram / --no-histogram-classes 파싱·조합·누수 | PR#8166 계약 33건(리뷰 T0~T3 영역, 변이 4종으로 감도 확인) |
| `object/schema_manager.update_all_statistics` | `sm_update_all_statistics` skip 전달·서버 실패 조기 반환 | PR#8166 계약(base 선재: 서버 실패를 성공으로 보고 → PR 이 고침) |
| `communication/network_interface_cl.histogram_all_classes` | `update_histogram_for_all_classes` OID 제외·경고·LIST_MOPS 해제 | PR#8166 F1(잠재) 중복 MOP 경고 과다 · base 선재 LIST_MOPS 누수 → PR 이 고침 |
| `optimizer/histogram_cl.like_fingerprint` | `histogram_split_hv_predicate` LIKE/NOT LIKE 후보·`bind_fp_walk` LIKE 성분(frexp 2배 밴드·NOT LIKE 보수·선택도 0 밴드)·`hv_pred_walk` 히스토그램 결합 | PR#8006 계약 25건(결함 없음; 3d0148e41 에서 G1·G2 3건 FAIL = 1d5858374 수정분, 변이 4종 감도). PR#8013 머지 뒤 갱신 필요(bind_term_walk·시그니처 교체) |
| `optimizer/query_graph.join_snapshot_selectivity` | `qo_snapshot_apply`·`_sarg_matches`·`_const_value`·`_threshold` 행 스냅샷 조인 선택도(k/ncard·평균 P×(1−nf)·NULL/중복 키·행 수 경계·폴백·에러 격리) | PR#8029 계약 26건(결함 없음, 변이 5종 감도, Q2-B dirty fetch 패치에도 유지); '(현재 동작)' 관찰 3건 — 전부 NULL 키 비단조·두 스냅샷 노드 순서 의존·nf=1.0 인자 생략 |
| `query/domain_resolve.convert_numeric` | `tp_value_cast`/`coerce`/`cast_force`/`coerce_strict` 경계·NUMERIC 사칙 p/s·변환기 정책 행렬 | PR#8022 계약 7건(변환 경계 불변) + **PR8087 F1 실결함: DOUBLE·MONETARY·문자열 [2^31-0.5, 2^31)→INT 가 INT_MIN 으로 랩(16eb8474d 가 만듦)** |
| `query/domain_resolve.compare_collation` | 해소된 비교·`domain_compare_by_type_pair` = `tp_value_compare_with_error`(41값 전 쌍)·인자 순서 반대칭·문자 결과 콜레이션 | PR#8022 계약 9건 + F2(잠재 — 코드셋≠콜레이션 문자열에서 비-exact 키 색인, 도달 경로 미발견) |
| `query/domain_resolve.operand_arith` | `qexec_resolve_late_bind_node`·`qexec_resolve_operand`(바인드 타입 회전)·수 승격·FLOAT+BIGINT 비대칭·SUM/AVG 누산 | PR#8022 계약 10건(결함 없음) |
| `query/expr_compile.arith_kernels` | 산술 커널 표·결과 도메인·lazy·리스트·게이트 상수·open 스텝·무작위 리스트(CSE·CASE·NUMERIC 혼합)를 해석 경로와 같은 바이너리에서 차분 | PR#7658 계약 22건 + F2(잠재) 실패한 prologue 재실행 생략 |
| `query/expr_compile.generic_step` | generic 스텝 피연산자 규약(전 opcode·'?'·!d/!t·WIDTH_BUCKET)·재귀 한도·중첩 프로그램을 해석기 fetch switch 와 차분 | PR#7658 계약 12건 + F1 오른쪽 깊은 체인 컴파일 지수 시간·거절 |
| `query/expr_compile.sum_acc` | NUMERIC SUM 누산기 41자리 경계(add_dbv·워드 경로·merge) = + 체인 | PR#7658(CBRD-27408 포트) 계약 6건, 결함 없음(base 3건 FAIL = 의도된 변경) |
| `query/query_hash_join.bloom_filter` | `hjoin_bloom_*` 해시 조인 둘째 입력 Bloom 필터(거짓 음성 0·강제변환 짝 7종·NULL/거짓 양성·비트 크기·붙는 조건·owner/clone·detach · 병렬: 직렬화 판정·prepare·워커) — 첫 `--extra` suite | CBRD-27659 v2 `fdc3fcd32` 계약 19건(v2 신규 attach 제외 5장면·`bloom_stats` 실행마다 0·trace 뒤 유지 포함, 변이 3종 감도) + 이력 F1 묶이지 않은 레코드(크래시·행 누락) · F3 잠재 부분 필터 · F4 FPR 2.2배 — `e55956083` 재현, `fdc3fcd32` 수정(defects_fixed_in). 구 F2 trace 카운터 소거(5a8c0b7d8)는 v2 `bloom_stats` 분리로 닫혀 계약으로 전환. **v3 `1a7bccdcc`** 병렬 경로 계약 12건 추가(guard + B6 `hjoin_bloom_wants_serial` 3 · B7 `hjoin_bloom_prepare` 1 · B8 `hjoin_bloom_worker_*` 7 — std::thread 2개 동시 판정·end 포함, 변이 5종 감도). B6-B8 은 API 가 링크될 때만 등록(weak) — fdc3fcd32 에서는 적용 안 됨. **v4 `e8f629fcc`** 상태 기계(WAITING→READY/OFF) 계약 9건 추가(guard + B9 threshold·fill·4,096 표본 3 · B10 `hjoin_bloom_poll` 1 · B11 워커 배치·OFF 전파·std::thread 2 + poll 스레드 2 · B12 detach 결과·실패 OFF_FAILED·첫 이유 유지 2, 변이 6종 감도), 기존 '첫 행 build' 계약은 새 규약(첫 행 setup, threshold 행 fill)으로 — hooks.cpp 가 두 구조체 배치를 `HJOIN_BLOOM_SEEN_BATCH` 로 갈라 e55956083~e8f629fcc 모두에서 돈다. B9-B12 는 1a7bccdcc 이하에서 적용 안 됨. 결함 없음(관찰 3건). **v5 `3726fd95d`**(사이 `8ad5ee2df`) 늦게 도착하는 필터·투영 전 검사 — 공개 API 가 바뀌어(attach 인자·detach/wants_serial 삭제·input_done/finish/skip_row 신설) test.cpp 맨 위에 모든 리비전 API 를 weak 로 두고 래퍼가 고른다, hooks.cpp 는 구조체 멤버(first_done·key_regus) SFINAE 로 가른다 — 여섯 리비전 모두에서 돈다. 계약 14건 추가(guard + B13 (K) 투영 전 키 해시 = 실제 투영·hjoin_fetch_key 키 해시 4 (12짝 12만 2천 행 불일치 0) · B14 (L) key_regus·skip_row·워커 3 + 관찰 1 · B15 (M) first_done 게이트·finish 결과·스레드별 해제·스레드 200 라운드 4 · B16 (N) fill 오류 프레임 1, 변이 8종 감도), `--extra` 에 query_opfunc.c·fetch.c·domain_resolve.c·storage_common.c. **잠재 F5** 같은 문자열 형·다른 코드셋 값을 투영 전 검사만 변환(해시 갈림·거짓 음성 가능, 도달성 미확인 — 수정안 fix_f5_relabel.diff 로 닫힘), 관찰 O5 스칼라 부질의 키 → 부질의 오류 id 소실(도달성 미확인). `defects_fixed_in` 을 케이스 접두어 사전으로 |
| `query/query_hash_join.rtf_clone` | `hjoin_rtf_bind_clone`·`hjoin_rtf_unbind_clone`(clone, orig 카운터 병합)·`hjoin_rtf_release`·`hjoin_rtf_first_input`·`hjoin_rtf_build`(decline)·`hjoin_rtf_pass`·`hjoin_rtf_contains` — 병렬 워커 클론에 묶인 해시 조인 런타임 필터와 XASL 노드별 카운터 | PR#8196 G3G4-01 계약 8건 + [defect] 3건 이력(release 뒤 반납 클론에 free 된 포인터 잔류 → 다음 실행의 build decline 때 행 루프까지 살아남음; base 함수 없음; `be6c64d40` 에서 unbind+build 진입 리셋으로 닫힘, defects_fixed_in). A14(a) 카운터 이동(`a2578d999`): pass 는 순수 조회, 클론 3/1+5/2+리더 1 → 9/3 병합·미바인드 클론 0·`hjoin_rtf_contains` 계약 추가 |
| `optimizer/query_planner.parallel_cost` | `qo_mackert_lohman_pages`(유효 캐시 MAX)·`qo_parallel_cost_divisor`·`qo_hjoin_runs_parallel`·`qo_scan_runs_parallel`·`qo_join_input_factor` | PR#8212 계약 6건 + [defect] F2 3건(PARALLEL(0·1)·MERGE 에서 실행기 직렬인데 비용 병렬 할인) · F3 계약 'cost_effective_cache_pages 0 = 캐시 없음' head FAIL(하한화) — 변이 1종 감도 |
