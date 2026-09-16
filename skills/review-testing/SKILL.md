---
name: review-testing
description: 리뷰 검증 규율 — 이 PR 에 무엇을 돌릴지(CTP sql/medium/shell, 동시성 재현, JOB, TPC-H) 결정하고, 어느 컨테이너에서 어떤 계약으로 재고, 무엇으로 판정하나. "이거 플랜 바뀌는 것 같은데 JOB 돌려? TPC-H 돌려?", "리뷰 전에 테스트 뭐 돌려야 해", 검증 없이 승인하려 할 때.
---

# 리뷰 검증 규율 (review-testing)

리뷰 승인은 **읽어서** 내리는 게 아니라 **돌려서** 내린다. 무엇을 돌릴지는 PR 이 바꾸는 것으로 결정한다.

## 1. 변경 유형 → 돌릴 것 (매트릭스)
| PR 이 바꾸는 것 | 필수 | 추가 | 판정 기준 |
|---|---|---|---|
| **옵티마이저**(조인 순서·선택도·히스토그램·비용 상수·플랜 비교기) | JOB 113 스크리닝(min-of-3) → **플랜 덤프 diff** → 바뀐 질의만 median-of-5 확정 | 바뀐 질의가 집계/스캔 지배면 TPC-H 해당 질의 | PG 조인 순서 일치 여부(지표) + MAD 기준 시간, 부호 쏠림 시 순서 반전 |
| **실행기**(스캔·집계·정렬·해시조인·병렬·임시파일) | TPC-H SF10 22 (SSOT r1 계약: 8192M/par6/warmup1+median-of-3) | JOB 은 회귀 유무만(스크리닝) | median + MAD, 결과 정합(행 수·체크섬) |
| **스토리지·트랜잭션**(락·MVCC·카탈로그·통계 수집·복구) | CTP sql + medium **코어 여부** + **동시성 재현 스크립트**(release·debug) | 관련 shell 디렉터리(`tests/shell-tc-index` 로 고른다) | 코어 0 · 시나리오 PASS · debug assert 0 |
| **파서·타입·함수·SQL 의미** | CTP sql (로컬은 코어만, 답안 정렬은 CI 몫) | medium | 코어 0 |
| **카탈로그·DDL·권한** | CTP sql + shell 관련 디렉터리 | `_db_*` 뷰 답안 변경 사유 코멘트 | 코어 0, 답안 변경은 의도 기준 |
| **리팩터링·헤더 정리** | 빌드(release+debug) + CTP sql 코어 | 빌드 시간 비교(PHYS) | 동작 무변경 |
| **"플랜이 바뀌는 것 같다"고만 알 때** | JOB 스크리닝으로 **어느 질의의 플랜이 바뀌는지** 먼저 확정 | 그 질의 성격에 따라 위 행으로 | 둘 다 전수로 돌리지 않는다 |

## 2. 어디서
| 벤치/테스트 | 컨테이너 | 비고 |
|---|---|---|
| JOB (joinorder 53G, 히스토그램 300버킷) | **.51**(원본), .52 | `.51` 하네스: `setup_arm_51.sh` → `screen_min3_51.sh` → `pick_movers_51.py` → `ab_interleave_51.sh` → `job_verdict.py` |
| TPC-H SF10 | **.50** | `.51/.52` 엔 없다. 질의 22개는 SF10 전용 |
| CTP sql/medium/shell | 어느 컨테이너나 — 단 **CTP 와 벤치를 동시에 돌리지 않는다**(`cub_master`·`databases.txt` 공유) | 컨테이너 격리 러너: `imports/xmilex-git/skills/ctp-run`(podman·justfile 필요, 호스트 CTP 는 `pkill cub` 로 이 사용자 cub_* 전부 죽임) |
| 동시성 재현 | 어느 컨테이너나, **demodb 또는 버릴 DB** | 벤치 볼륨에 재현하지 않는다 |

## 3. 측정 계약 (어기면 수치 무효)
1. `~/bin/host_idle_guard.sh --wait 180` — 세 컨테이너가 한 물리 호스트·같은 `taskset -c 0-15` 를 쓴다. A/B 는 **팔마다** 게이트.
2. 통계는 `UPDATE STATISTICS ON t WITH FULLSCAN, 300 BUCKETS` 표준 → **서버 재기동**(XASL 캐시가 옛 플랜을 쓴다).
3. run 반복 median + MAD, 질의별 부호 분포 확인, 쏠리면 **순서 반전**. 1회 측정으로 판정하지 않는다.
4. 계약 트랙을 섞지 않는다(구 16G min-of-2 vs SSOT r1 8192M median-of-3). 한 변경 = 한 메커니즘 = 한 측정 — 메커니즘이 둘이면 갈라서 잰다.
5. 메커니즘 주장(캐시라인·락)은 벽시계보다 `perf c2c`/`perf stat` 로 유무를 먼저 가른다.
6. 빌드는 `goto <rev> release|debug`, 검증은 **release 로 성능·debug 로 assert** 둘 다.

## 4. CTP 판정 규칙
- 로컬 CTP 는 **코어 발생 여부만** 본다. NOK 는 CI(`tc/pr-<n>` 짝)의 몫 — 단 NOK 가 수백 건이면 원인 분류(`tc-analysis`)는 한다(예: 2026-09-16 `-494` 캐스케이드는 PL 연결 잔존).
- fault-injection 코어(`fi_handler_random_exit`)는 결함이 아니다.
- **CTP 가 `$CUBRID/conf` 를 `ha_mode=yes`·`port 1822` 로 바꿔 놓는다** — 뒤에 평범 서버를 쓰려면 되돌린다(`테스트TC-규칙 §6`).
- shell TC 단독 실행: `init_path`·`result_file`·`case_name` export, 로그는 작업 디렉터리 밖으로.

## 5. 리뷰 답글에 적는 형식
"무엇을 어디서 어떤 계약으로 돌렸고 결과가 무엇인가" 한 문단 — 시나리오 / 수정 전 / 수정 후 표. **CTP 수치는 답글에 붙이지 않는다**(사용자 지시). 재현 스크립트는 `projects/<JIRA키>/repro/` 와 JIRA 첨부.

## 6. 연동 도구
- 실패 케이스가 이 PR 과 관련 있는지: `tools/tc_relevance.py --pr <n> --failed <케이스 경로…>` (`tests/shell-tc-index/index.json` 기반, 높음/낮음 + 이유).
- 규칙 근거: `rules/성능-리뷰-규칙.md` §6·§7.
