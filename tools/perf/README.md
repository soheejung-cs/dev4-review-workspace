# tools/perf — 성능 작업의 '구조 먼저' 도구 (2026-10-18, CBRD-27361 에서 추출)

규약: [`harness/구조우선-성능작업.md`](../../harness/구조우선-성능작업.md) · 스키마 `harness/schemas/structure.json` · 검사 `python3.11 -m tools.harness.structure_check`.

| 도구 | 무엇 | structure.json 칸 |
|---|---|---|
| `perf_attach.sh <db> <sql> <out.data>` | cub_server(<db>)의 모든 스레드에 perf 를 붙여 질의 하나를 잰다(병렬 워커 포함) | time_map.source |
| `prof_bucket.py <perf.data\|flat> [--json]` | 자기 시간을 축으로 묶는다: 페이지 버퍼·B-tree·힙·술어/값·리스트·집계/정렬/해시·할당·기타 | time_map.buckets |
| `trace_scan.py <csql 출력> [--json]` | `;trace on` 스캔 줄 → 경로(batch / `batch: no (사유)`), fetch, fix/key, fix/row | coverage.map, cost_census(page_fix) |
| `mech_bench.sh <shapes> <outdir> [N]` | 기법별 미시 벤치: 모양마다 질의 하나, 직렬·병렬, 예열 1 + N | mechanism_bench |
| `mech_summarize.py --ref … --new … --shapes …` | median±MAD, 판정(2×MAD·3%), 행 집합 출력 대조, 라운드 불일치·가장 불리한 짝 | mechanism_bench.result |
| `shapes_filter_scan.txt` | lineitem 필터 스캔 15모양(숫자·날짜·BETWEEN·두 컬럼·LIKE·CHAR·IN·OR·SUBSTRING·행 반환·GROUP BY) | 출발점 |

함정: batch 로 돈 스캔은 trace 의 heap time·fetch 가 0 이다(2026-10 현재) — 그 스캔은 perf 로 본다. trace 시간은 중첩(바깥에 안쪽이 들어감)이라 비교용으로만.
