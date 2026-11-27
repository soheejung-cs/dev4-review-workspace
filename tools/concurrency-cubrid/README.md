# 동시성 시험 (`tools/concurrency-cubrid`)

세션 여럿이 함께 돌 때의 CUBRID 동작을 응용과 같은 경로(JDBC → 브로커/CAS → 서버)로 시험한다. 정답을 미리 적지 않는다 — 매뉴얼이 정한 격리 수준 정의, 또는 병렬 연산자를 끈 같은 질의의 결과가 기준이다.
처음 만든 기록(결과·설계): `claude-workspace/projects/동시성시험-20261127/`(비공개).

| 도구 | 보는 것 | 판정 | 시간 |
|---|---|---|---|
| `Hermitage.java` | 격리 이상 14꼴(Adya G0·G1a·G1b·G1c·OTV·PMP·PMP-write·P4·G-single·G-single-write·G2-item·G2 + 유일 키 경쟁 2) × RC·RR·SER × 접근 경로 3(기본 키·힙 강제·보조 인덱스 강제) | 각 꼴의 판정 함수가 이상/막음을 내고 매뉴얼 기대(`en/sql/transaction.rst`)와 대조 | 약 75초 |
| `ParallelInject.java` | 병렬 워커 안의 오류·취소가 메인 스레드로 옮겨지는 길(CBRD-28412 꼴) — 롤백만 하는 동시 쓰기 아래서 | 오류: 병렬 연산자를 끈 판(힌트 `NO_PARALLEL_SCAN NO_PARALLEL_HASH_JOIN NO_PARALLEL_SUBQUERY`)과 같은 오류 코드 / 취소: 같은 연결의 다음 질의가 기준 답 / 확인: 병렬 답 = 기준 답 | 적재 수 분 + 지정한 초 |

둘 다 `run.sh` 가 서버 pid(재기동)와 코어 디렉터리(`/data/core`, 서버를 `ulimit -c unlimited` 로 띄운다)를 앞뒤로 본다. debug 설치본이면 assert 도 판정 수단이다.

## 실행
```bash
cd ~/dev/docs/dev4-review-workspace/tools/concurrency-cubrid
bash run.sh hermitage [시나리오 이름 앞부분]     # DB conchm 을 만들고 끝나면 지운다
bash run.sh parallel <초> [클라이언트 수=6]      # DB concpx(pt 60만·pu 30만 행)는 남겨 다음 실행이 다시 적재하지 않는다
bash db.sh down concpx                           # 다 쓰면 지운다
```
- 대상은 **활성 설치본**(`~/CUBRID`) — 다른 빌드는 `goto` 로 바꾼 뒤. 자바는 `~/tools/jdk-21`(단일 파일 실행, 시스템 java 8 로는 안 된다), JDBC 는 설치본의 `jdbc/cubrid_jdbc.jar`, 브로커는 `BROKER1`(33000) — 꺼져 있었으면 끝나고 다시 끈다.
- 결과: `$CONC_OUT`(기본 `~/dev/scratch/concurrency-cubrid`)의 `<도구>-<시각>.log`, 한 회 한 줄 `summary.txt`.
- **CTP·shell TC·goto 빌드·벤치·Jepsen 과 함께 돌리지 않는다**(서버를 내리고 cub_master·브로커를 같이 쓴다 — H-05). 두 도구끼리도 차례로(끝에 브로커를 끈다).

## 결과 읽기
- Hermitage: 줄마다 `접근경로 시나리오 L수준 결과 판정`. 결과 `ok`(막아야 할 것을 막음)·`allowed`(매뉴얼이 허용하는 이상)는 정상, `FAIL`(막아야 할 이상이 생김·근거 없는 오류)·`HANG`·`stricter`(허용된 것을 막음 — 결함은 아니고 동작 변화)를 본다. 모든 실행의 단계별 기록이 로그에 있다(`!!!` 가 기대와 다른 실행).
- 2026-11-27 .51 debug(develop 6c43128bd 기반): 126회 전부 매뉴얼대로 — RC 는 PMP·PMP-write·P4·G-single 허용, RR·SER 는 그것들을 막고(-1158 Serializable conflict) write skew(G2-item·G2)만 허용.
- 2026-11-27 .51 debug(같은 빌드) ParallelInject 300초·6클라이언트: 오류 주입 627회 모두 직렬과 같은 코드(-181·-539·-427), 취소 307·시간 초과 155 모두 실행 중에 닿음(-4), 확인 489회 모두 기준 답, 롤백 쓰기 885, FINDING 0, 서버 재기동·코어 0. 확인 질의·오류 꼴·취소 대상 모두 SHOW TRACE 로 병렬(`parallel workers` ≥ 1)을 확인했다.
- ParallelInject: `FINDING` 줄이 결함 후보 — `error-path`(직렬과 병렬의 오류 코드가 다름·한쪽만 오류), 취소·시간 초과 뒤 기준과 다른 답, 예상 밖 오류 코드. `== counts` 의 `error:<꼴>:same<코드>` 가 같은 오류로 끝난 횟수, `cancel:err-4`·`timeout:…` 가 취소가 실행 중에 닿은 횟수.

## 효과와 한계
- Hermitage 는 결함을 찾는 도구가 아니라 **격리 의미의 회귀망·정답표**다(고정 꼴, 두 행). MVCC·잠금·유일 키를 바꾼 PR 에서 돌린다. 생성된 SQL 로 넓히는 것은 격리 오라클(설계문서 `설계문서-격리오라클.md`).
- ParallelInject 의 오류 주입은 SQL 실행 오류(0 나누기·변환 실패·오버플로)만이다 — 워커의 메모리·I/O 실패는 엔진 장애 주입 지점(`fault_injection_ids`, 지금은 스토리지·로그·연결 쪽뿐)이나 gdb 분기 주입(메모리 howto)이 필요하다.
