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
               이미 고쳐진 결함은 suite.json 에 defects_fixed_in(커밋)을 적어 이력으로 둔다 — 다시 PASS 하면 '재발' 로 보고된다
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
