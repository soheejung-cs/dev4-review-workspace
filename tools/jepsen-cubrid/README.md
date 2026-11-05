# Jepsen for CUBRID (`tools/jepsen-cubrid`)

한 CUBRID 노드의 **격리·원자성·내구성**을 Jepsen 0.3.15 로 잰다 — 응용과 같은 경로(JDBC → 브로커/CAS → 서버)로 트랜잭션을 돌리며 서버를 죽이고(kill -9)·얼리고(SIGSTOP)·CAS 를 죽인 뒤, Elle·bank·set 검사기가 역사(history)를 판정한다.
처음 만든 기록(설계·발견·함정): `claude-workspace/projects/jepsen-cubrid-20261104/현황.md`(비공개).

## 무엇을 보나
| 워크로드 | 트랜잭션 | 검사기 | 잡는 것 |
|---|---|---|---|
| `append` | 키 10개(표 `jl0..3`)에 append·read, `INSERT … ON DUPLICATE KEY UPDATE val = concat(…)` | Elle list-append | G0·G1a/b/c·G-single·G2·internal — 격리 이상 |
| `wr` | 같은 키에 일반 `UPDATE`(없으면 INSERT)·read | Elle rw-register | 손실 갱신(P4)·위와 같은 사이클 |
| `bank` | 계좌 8개 이체(`--bank-mode rmw` 읽고 절대값 쓰기 / `atomic` 한 문장) + 전체 읽기 | bank | 합계 변화·음수 잔액 |
| `set` | 유일 삽입 + 전체 읽기 | set-full(linearizable) | 확인된 쓰기 유실(내구성) |

장애(`--faults`): `kill`(cub_server kill -9, master 의 revive 또는 `cubrid server start`), `pause`(SIGSTOP/SIGCONT), `kill-cas`(`--broker` 의 CAS 하나 kill -9). 기대 모델: READ COMMITTED → `read-committed`, REPEATABLE READ·SERIALIZABLE → `snapshot-isolation`(`--expect` 로 바꾼다).

## 준비와 실행
```bash
cd ~/dev/docs/dev4-review-workspace/tools/jepsen-cubrid
./setup_env.sh                      # 한 번: JDK 21·lein 을 ~/tools/jepsen-env 에(sudo 불필요), 의존성, store 링크
./run_one.sh a-rr-kill --workload append --isolation repeatable-read --expect snapshot-isolation \
             --faults kill --time-limit 90 --concurrency 10 --rate 50 --nemesis-interval 15
./run_matrix.sh 90                  # 표준 행렬 20회(약 1시간)
. ./env.sh && lein run serve        # 결과 웹 보기(타임라인·Elle 사이클 그림)
```
- 대상은 **활성 설치본**(`~/CUBRID`) — 다른 빌드는 `goto` 로 바꾼 뒤. debug 빌드면 assert 도 함께 본다.
- DB 는 도구가 만든다: `--dbname`(기본 `jepsen`)이 없으면 `$JEPSEN_CUBRID_OUT/db/<이름>` 에 createdb, 실행 처음(빈 DB 로 시작)과 끝에 deletedb — **시험용 볼륨이 남지 않는다**. 끝난 DB 를 들여다보려면 `--leave-db-running`. 같은 이름이 그 디렉터리 밖에 등록돼 있으면 그대로 쓰고 지우지 않는다.
- 결과: `$JEPSEN_CUBRID_OUT`(기본 `~/dev/scratch/jepsen-cubrid`)의 `store/<시험>/<시각>/`(history·results.edn·elle/·timeline.html), `summary.txt`(한 회 한 줄), `logs/`. 리포의 `store` 는 그곳으로의 링크(무시 목록).
- 브로커: 실행 전에 꺼져 있었으면 끝나고 다시 끈다.
- **CTP·shell TC·goto 빌드·벤치와 함께 돌리지 않는다**(서버를 내리고 cub_master 를 같이 쓴다 — H-05).

## 결과 읽기
- `summary.txt` 의 `valid=true` 가 기본. `anomalies` 가 비어 있지 않으면 `store/…/elle/<이상>.txt` 에 사이클 설명(T1·T2 의 어느 읽기가 어느 쓰기를 못 봤나)과 그림.
- **무효가 정상인 것**: `a-ser-none`(SERIALIZABLE 을 serializable 로 보면 G2-item — 매뉴얼 `en/sql/transaction.rst` "SERIALIZABLE … identical to REPEATABLE READ … write skew"), `b-rc-rmw`(RC 의 읽고-쓰기 손실 갱신 → `wrong-total`, 대조군). 이 둘이 **유효**로 바뀌면 동작이 바뀐 것이다.
- `ok/fail/info`: `fail` 은 확정 롤백(교착 −72, 잠금 시간 초과 −966~968, 직렬화 충돌 −1158, 문장 오류, 연결 실패), `info` 는 커밋 결과를 모르는 것(커밋 중 끊김). 장애 판에서 fail 이 많은 것은 서버가 내려간 동안의 연결 실패다.
- 2026-11-04 .52 debug `2ca10ed03` 20회: 기대대로(위 둘만 무효), 서버 코어 0.

## 효과와 한계 — 언제 돌리나
- **단일 노드에서는 결함을 잘 찾지 못한다**(2026-11-04 20회 결함 0, 사용자 평가 "생각보다 효과가 없다"). 워크로드가 작고(키 10개·한 페이지 표) 장애가 거칠어 페이지 분할·overflow·버퍼 교체·병렬 질의·DDL 같은 엔진 내부 경로를 거의 지나지 않는다 — 같은 날 엔진 결함(부하 아래 CREATE INDEX)은 표적 부하 시험(버퍼 최소 + 동시 페이지 작업, 자가진화 #079)이 찾았다.
- 쓸모: **격리·내구성 의미의 회귀망** — 트랜잭션·MVCC·잠금·로그·복구·브로커/CAS 를 바꾼 PR 에서 행렬을 돌려 문서화된 동작(RC 손실 갱신 허용, SERIALIZABLE=SI)이 그대로인지, 장애 아래 합계·확인된 쓰기가 지켜지는지 확인한다(`review-testing` §1 스토리지·트랜잭션 행의 "추가").
- 못 보는 것: HA(노드 여러 대의 분할·복제 지연·장애 조치 — Jepsen 이 가장 잘 잡는 영역, 노드가 더 필요), 큰 데이터, DDL 동시성.

## 만들며 데인 함정 (코드에 반영됨)
- DB 레코드가 `db/LogFiles` 와 `db/Kill` 을 함께 구현하면 Jepsen 이 매 실행 끝에 DB 를 kill 한다(자가진화 #080) — LogFiles 를 구현하지 않는다.
- 얼린 서버는 JDBC 를 영원히 붙잡는다(CUBRID `queryTimeout` 은 서버 판정, 드라이버에 소켓 시간 제한 없음) — 연산마다 `--op-timeout`, 커밋과 포기를 원자값 하나로 경쟁(#081).
- `cubrid server|broker start` 를 `clojure.java.shell` 로 부르면 데몬이 stdout 파이프를 물어 영원히 기다린다 — 파일로 리다이렉트(`daemon!`).
- `nemesis-package` 는 쓰지 않는 분할·시계 nemesis 도 SSH 로 준비하다 `--no-ssh` 에서 죽는다 — `db-package` 만.
- Jepsen 0.3.15 는 Java 21(class 65) — `env.sh` 가 JDK 21 을 쓰고 `CLASSPATH` 를 지운다.
