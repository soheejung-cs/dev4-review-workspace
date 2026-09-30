# tools/ab — A/B 표준 스크립트 (2026-09-30, CBRD-27215 3차 리뷰 측정에서 추출)

설치본 둘(NEW·REF = goto 캐시 디렉터리)에 **같은 SQL** 을 돌려 정합(byte)과 시간(교대 반복)을 비교한다. 환경변수로만 설치본을 받는다 — 경로를 스크립트에 박지 않는다.

| 스크립트 | 무엇 | 예 |
|---|---|---|
| `pick_ref.sh` | develop 기반 goto 캐시 후보를 보여 준다: BUILD_REV 가 develop 조상인지, 머지베이스 이후 바뀐 디렉터리(옵티마이저만이면 "실행 의미 동일 후보") | `pick_ref.sh` |
| `ab_sa.sh <sql> [tag]` | SA csql 로 두 설치본에 같은 SQL, `(N sec)` 제거 후 byte 비교. DB 는 각 설치본 `databases/abdb` 에 스스로 만든다 | `NEW=~/release/CUBRID-goto-X REF=~/release/CUBRID-goto-Y ab_sa.sh case.sql` |
| `ab_time.sh <sql>...` | SA csql 시간 A/B: 질의마다 REF·NEW 를 **교대**로 N 회, `selected. (N sec)` 의 **첫 괄호**(마지막 괄호는 commit 시간). 로그 한 줄 = 한 샘플 | `DB=t33db N=5 ab_time.sh q1.sql q2.sql > t.log` |
| `ab_summarize.py <log>` | ab_time 로그 → 질의별 median±MAD 와 NEW/REF 표(마크다운) | `ab_summarize.py t.log` |
| `mkbench_numeric.sql` | 벤치 볼륨이 이 빌드에서 열리지 않을 때(upgradedb 메타데이터 버저닝) 두 설치본에 **결정적으로 같은** 합성 테이블(NUMERIC 4컬럼 + INT + DATE, 2^22 행)을 만든다 — `csql -S -u dba <db> -i mkbench_numeric.sql` | |

규약(측정-규칙): 호스트 유휴 확인(`~/bin/host_idle_guard.sh`) → 두 설치본 conf 동일(`data_buffer_size`·`parallelism`) → 교대 실행 → median-of-N + MAD 병기 → 결과 정합은 `ab_sa.sh` 로 따로. 성능 대안이 둘이면 **둘 다 빌드해 같은 계약으로 재고 나서 고른다**.
