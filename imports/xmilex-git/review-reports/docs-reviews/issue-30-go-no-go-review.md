# Issue #30 진행상황 리뷰 및 GO/NO-GO 판정

- 대상: [`xmilex-git/workspace#30 — HTAP 지도: CUBRID → Debezium → Kafka → ClickHouse Compact POC`](https://github.com/xmilex-git/workspace/issues/30)
- 검토 기준일: 2026-08-16
- 검토 기준 커밋
  - `xmilex-git/workspace` `main`: `120bcf45000c2798eaa4da4006f895159537cd91`
  - `xmilex-git/debezium` `cubrid-connector`: `37c57b8f75918c8ab9fc98bfc6975f98b53888d3`
- 검토 방식: 이슈·하위 티켓·ADR·커밋·E2E/장애 검증 스크립트·커넥터 핵심 코드를 대조한 정적 리뷰
- 한계: 검토자가 CUBRID/Podman 실행 환경에서 전체 캠페인을 독립 재실행한 결과는 아니다. 따라서 기존 PASS 기록은 저장소에 남은 코드·스크립트·코멘트의 정합성을 기준으로 평가했다.

---

## 1. 최종 판정

> # **NO-GO**
>
> 현재 상태로는 Issue #42와 Issue #30을 완료 처리하면 안 된다.
>
> 기본 데이터 경로와 정상 재시작 시나리오는 상당 수준 완성됐지만, **커밋된 대형 트랜잭션을 Kafka Connect 큐로 내보내는 도중 워커가 종료되면 일부 이벤트가 영구 유실될 수 있는 offset-anchor 결함**이 커넥터 코드에 존재한다. 이는 Issue #30의 핵심 완료 계약인 “재시작 후 이벤트 유실 0”을 직접 위반할 수 있는 P0 blocker다.

판정 범위는 다음과 같이 구분한다.

| 판정 대상 | 결과 | 판단 |
|---|---:|---|
| 현재 구현으로 Compact POC 종료 | **NO-GO** | 장애 복구 경로에 P0 유실 가능성 존재 |
| P0 수정 후 동일 범위 POC 재판정 | **GO 가능** | 정상 경로·스냅샷·sink 수렴 기반은 이미 충분히 구축됨 |
| 알파/제품 배포 | **NO-GO** | online snapshot, DDL, savepoint, HA, 권한, backpressure, 성능 SLO 등 별도 제품화 과제가 남아 있음 |
| 실서비스/운영 보증 | **NO-GO** | 현재 이슈가 명시적으로 Compact POC 범위임 |

**판정 신뢰도: 높음.** P0는 실행 결과에 대한 추측이 아니라, 커넥터의 anchor 갱신 순서, SourceRecord offset 생성 방식, bounded queue/batch 구조를 함께 대조해 도출한 코드 수준 결함이다.

---

## 2. 현재 진행상황 요약

GitHub 진행률 기준으로 Issue #30의 하위 이슈는 **14개 중 13개가 닫혀 92%**이며, 남은 하나가 최종 판정 티켓인 [#42](https://github.com/xmilex-git/workspace/issues/42)다. 즉 행정적으로는 최종 gate만 남은 상태다.

구현 산출물은 실제로 존재한다.

- CUBRID CDC 하네스와 로그 덤프
- Debezium CUBRID 소스 커넥터 스켈레톤 및 snapshot/streaming 구현
- Kafka KRaft, Kafka Connect, ClickHouse 단일 노드 인프라
- ClickHouse `ReplacingMergeTree` 및 canonical `FINAL` view
- 스냅샷→스트리밍→Kafka→sink→ClickHouse E2E 스크립트
- 태스크 재시작, 워커 재시작, ClickHouse 중단, 토픽 재전송 장애 스크립트
- CUBRID와 ClickHouse 비교용 differential checker
- ADR 0002~0006 및 이슈별 결정 기록

따라서 “아무것도 완성되지 않은 POC”는 아니다. 오히려 정상 경로와 기본적인 장애 시나리오는 잘 관통됐고, 설계 결정의 추적성도 좋은 편이다. 다만 마지막 gate에서는 **기록된 PASS 수가 아니라, 실패 시 데이터 유실을 막는 offset 안전성이 실제로 성립하는지**를 기준으로 판단해야 한다.

---

## 3. 완료 계약 10개 항목 판정

Issue #30은 `htap-cubrid.md` §1.3의 10개 수직 흐름을 완료 계약으로 사용하되, direct sink 대신 Kafka Connect 경로를 사용한다.

| # | 완료 계약 | 판정 | 검토 결과 |
|---:|---|---:|---|
| 1 | CUBRID 단일 DB, ClickHouse 단일 노드 | **PASS** | rootless Podman 기반 Kafka/Connect/ClickHouse 인프라와 `htapdb`가 구성됨 |
| 2 | PK가 있는 2~3개 테이블 | **PASS** | `t_order`, `t_item` 두 PK 테이블로 스냅샷·CDC·검증 수행 |
| 3 | 쓰기 일시 중지 초기 스냅샷 | **PASS** | JDBC snapshot, RR 승격, JNA barrier LSA, `_version=0` 전환이 구현·검증됨 |
| 4 | I/U/D/COMMIT/ABORT 추출 | **PASS** | P0 하네스와 E2E에서 모두 확인됨 |
| 5 | COMMIT 전 트랜잭션별 버퍼링 | **부분 FAIL** | 정상 COMMIT/ABORT는 동작하지만, COMMIT 이벤트 publish 중 offset anchor가 너무 빨리 전진할 수 있음 |
| 6 | RMT full-row upsert 및 tombstone | **PASS** | 다중 UPDATE, insert→delete, delete→insert, PK 변경이 canonical view로 수렴 |
| 7 | durable LSA checkpoint | **부분 FAIL** | 정상 워커 재시작은 통과했으나, partial transaction publish 상황에서 checkpoint가 안전하지 않음 |
| 8 | 재시작·중복·ClickHouse 장애 복구 | **FAIL** | 기존 4개 시나리오는 통과했으나, 핵심 crash window가 미검증이며 코드상 유실 가능성이 존재 |
| 9 | CUBRID↔ClickHouse differential check | **PARTIAL** | 검증기는 존재하지만 행 단위 결합을 잃는 checksum 방식 때문에 일부 불일치를 놓칠 수 있음 |
| 10 | 별도 OLTP/OLAP endpoint | **PASS(구조적)** | CUBRID `csql` 경로와 ClickHouse client/view 경로가 분리되어 있음. 별도 acceptance 항목으로 명시되지는 않았음 |

전체 10개 중 정상 기능은 대부분 성립했지만, **5·7·8번은 하나의 동일한 P0 offset 문제로 완료라고 볼 수 없다.** 따라서 총괄 판정은 NO-GO다.

---

## 4. P0 Blocker — COMMIT publish 도중 anchor 조기 전진으로 인한 이벤트 유실 가능성

### 4.1 관련 코드

핵심 파일은 다음과 같다.

- [`CubridStreamingChangeEventSource.java`](https://github.com/xmilex-git/debezium/blob/37c57b8f75918c8ab9fc98bfc6975f98b53888d3/debezium-connector-cubrid/src/main/java/io/debezium/connector/cubrid/CubridStreamingChangeEventSource.java)
- [`CubridOffsetContext.java`](https://github.com/xmilex-git/debezium/blob/37c57b8f75918c8ab9fc98bfc6975f98b53888d3/debezium-connector-cubrid/src/main/java/io/debezium/connector/cubrid/CubridOffsetContext.java)
- [`CubridConnectorTask.java`](https://github.com/xmilex-git/debezium/blob/37c57b8f75918c8ab9fc98bfc6975f98b53888d3/debezium-connector-cubrid/src/main/java/io/debezium/connector/cubrid/CubridConnectorTask.java)

현재 DCL 처리 흐름은 개념적으로 다음과 같다.

```java
TxnBuffer buffer = inflight.remove(item.transactionId());
if (COMMIT) {
    publishTransaction(..., inflight, buffer, ...);
}
```

`publishTransaction()`은 이미 COMMIT 중인 트랜잭션이 `inflight`에서 제거된 상태에서 anchor를 정한다.

```java
if (!inflight.isEmpty()) {
    anchor = remainingOldestTransaction.start;
}
else {
    anchor = currentCommitBatchStart;
}
```

그 결과 COMMIT 중인 트랜잭션의 변경 레코드에 붙는 source offset은 **그 트랜잭션의 최초 DML 위치가 아니라 COMMIT 배치 또는 나중에 시작한 다른 트랜잭션 위치**가 될 수 있다.

`CubridOffsetContext.getOffset()`은 이 anchor를 실제 Kafka Connect source offset으로 직렬화한다. `EventDispatcher`는 각 SourceRecord를 만들 때 `offsetContext.getOffset()`을 복사하고, `CubridConnectorTask`는 bounded `ChangeEventQueue`를 poll하여 여러 Kafka Connect batch로 내보낸다. `ChangeEventSourceCoordinator`는 streaming source를 별도 executor에서 실행하므로 queue 생산과 task poll은 병렬로 진행된다. 또한 `BaseSourceTask.commitRecord()`는 Kafka producer가 확인한 **각 SourceRecord의 offset**을 최신 offset으로 갱신하며, CUBRID 트랜잭션의 전체 publish 완료 여부를 별도로 알지 못한다.

### 4.2 유실이 발생할 수 있는 순서

단일 대형 트랜잭션 T1을 예로 들면 다음과 같다.

1. T1의 DML이 LSA `A`부터 여러 로그 배치에 걸쳐 기록된다.
2. T1 COMMIT DCL이 훨씬 뒤의 배치 `C`에서 추출된다.
3. 현재 코드는 T1을 `inflight`에서 먼저 제거한다.
4. `publishTransaction()`은 T1의 모든 레코드에 `C`를 restart anchor로 붙인다.
5. T1의 이벤트가 ChangeEventQueue에 순차 enqueue된다.
6. Kafka Connect가 앞쪽 일부 레코드를 poll·produce·ack하고 source offset `C`를 저장한다.
7. 아직 뒤쪽 T1 레코드가 enqueue 또는 produce되지 않은 시점에 워커가 종료된다.
8. 재시작은 저장된 offset `C`에서 시작한다.
9. `C`부터는 T1의 이전 DML이 존재하지 않고 COMMIT DCL만 보이므로, 새 프로세스의 T1 buffer는 비어 있다.
10. 아직 발행되지 않았던 T1 레코드는 다시 생성되지 못하고 영구 유실된다.

이 경로는 이론적인 극단값만의 문제가 아니다.

- Debezium 공통 기본값은 `max.queue.size=8192`, `max.batch.size=2048`이다.
- 현재 `cubrid-source.json`은 이 값을 별도로 확대하지 않는다.
- P0 하네스에서는 30,000행 트랜잭션이 존재할 수 있음을 이미 확인했다.
- 따라서 한 트랜잭션이 queue와 poll batch를 넘어 여러 번에 나뉘어 전달되는 조건은 충분히 현실적이다.

작은 트랜잭션에서도 source thread와 poll thread가 병렬로 동작하므로 이론적인 crash window는 존재하지만, 큰 트랜잭션에서는 재현 가능성이 훨씬 높아진다.

### 4.3 기존 장애 테스트가 이 문제를 잡지 못한 이유

[`run-faults.sh`](https://github.com/xmilex-git/workspace/blob/120bcf45000c2798eaa4da4006f895159537cd91/htap-poc/e2e/run-faults.sh)의 S1/S2는 다음 형태다.

1. 작은 phase-1 트랜잭션을 완료한다.
2. 태스크 또는 워커를 재시작한다.
3. phase-2 트랜잭션을 수행한다.
4. 최종 상태를 비교한다.

재시작 시점이 **한 COMMIT의 buffered changes를 `publishTransaction()`이 여러 queue/poll batch로 내보내는 도중**으로 고정되어 있지 않다. phase-1이 이미 모두 Kafka로 전달된 뒤 재시작되면 현재 코드도 정상 통과한다.

즉 기존 테스트는 “정상 anchor 재개”는 검증했지만, **partial COMMIT publication 이후 offset이 저장된 crash window**를 검증하지 않았다.

### 4.4 최소 수정 방향

가장 작은 수정은 COMMIT 트랜잭션을 모든 이벤트 enqueue가 끝날 때까지 `inflight`에 남겨 두는 것이다.

```java
case DCL -> {
    TxnBuffer buffer = inflight.get(item.transactionId());
    if (buffer == null) {
        continue;
    }

    if (item.dclType() == COMMIT) {
        // current committing txn must remain part of the safe replay set
        publishTransaction(..., inflight, buffer, ...);
        inflight.remove(item.transactionId());
    }
    else {
        inflight.remove(item.transactionId());
    }
}
```

이렇게 하면 `publishTransaction()`이 선택하는 가장 오래된 in-flight anchor에 현재 COMMIT 트랜잭션도 포함된다. 모든 레코드가 enqueue된 뒤에만 해당 buffer를 제거하고, 그 뒤 heartbeat 또는 후속 레코드가 더 앞선 anchor를 기록하게 해야 한다.

보다 명시적으로 구현하려면 `read cursor`, `safe replay anchor`, `per-event seq`를 별도 상태로 유지하고 다음 불변식을 코드와 테스트에 고정하는 편이 좋다.

> **한 트랜잭션의 마지막 변경 레코드가 queue에 들어가기 전에는, 그 트랜잭션 최초 DML보다 뒤의 source offset을 어떤 레코드에도 부여하지 않는다.**

### 4.5 GO 전 필수 재현 테스트

다음 테스트가 최소 3회 연속 통과해야 한다.

1. `max.queue.size=64`, `max.batch.size=16`처럼 queue를 작게 설정한다.
2. 30,000개 이상의 서로 다른 PK INSERT를 단일 트랜잭션으로 COMMIT한다.
3. Kafka topic에 일부 레코드가 도착하고 source offset이 실제 저장됐지만 전체 건수에는 못 미치는 시점에 Connect JVM을 강제 종료한다.
4. 재기동 후 CUBRID와 ClickHouse canonical view를 **PK를 포함한 full-row digest**로 비교한다.
5. 유실 0, 중복은 동일 `_version`으로만 발생, 최종 view mismatch 0을 확인한다.
6. 아래 네 crash point를 각각 수행한다.
   - 첫 변경 레코드 enqueue 전
   - COMMIT 트랜잭션의 중간 레코드 publish 중
   - 마지막 데이터 레코드 enqueue 후, anchor-advance heartbeat 전
   - anchor-advance heartbeat가 Kafka에 기록된 뒤
7. T1 시작 → T2 시작 → T1 COMMIT → T1 publish 중 kill 형태의 interleaved transaction도 추가한다.

---

## 5. 주요 리뷰 발견사항

### P1-1. `diff-check.sh`가 행 단위 대응관계를 보존하지 않는다

파일: [`htap-poc/e2e/diff-check.sh`](https://github.com/xmilex-git/workspace/blob/120bcf45000c2798eaa4da4006f895159537cd91/htap-poc/e2e/diff-check.sh)

현재 구현은 같은 bucket 안에서 각 컬럼 값을 독립적으로 정렬한 뒤 컬럼별 MD5를 계산한다.

```python
cs = md5("\n".join(sorted(r[i] for r in crows)))
hs = md5("\n".join(sorted(r[i] for r in hrows)))
```

이 방식은 row count와 각 컬럼의 값 multiset은 비교하지만, **어떤 PK에 어떤 값이 결합되어 있는지**를 잃는다.

예를 들어 같은 bucket에 아래 두 행이 있다고 가정한다.

```text
CUBRID:      (1, alice, 10), (9, bob, 20)
ClickHouse:  (1, bob,   20), (9, alice, 10)
```

PK, customer, amount 각각의 값 집합과 row count는 동일하므로 현재 checksum은 모두 일치할 수 있지만, 두 데이터베이스의 실제 행은 다르다.

#### 수정 권고

- 각 행을 `PK + 모든 컬럼`의 canonical serialization으로 만든다.
- serialization은 delimiter 단순 결합보다 length-prefix, canonical JSON, CBOR 등 충돌 없는 형식을 사용한다.
- bucket별로 **전체 canonical row 문자열을 정렬한 digest**를 비교한다.
- 컬럼별 checksum은 mismatch 원인 진단용 보조 지표로만 유지한다.
- 검증기 자체 테스트에 “동일 컬럼 multiset, 다른 PK-값 결합” 케이스를 추가한다.

이 문제만으로 현재 측정 결과가 틀렸다고 단정할 수는 없지만, differential checker를 correctness oracle로 사용하기에는 false negative 가능성이 남아 있다.

### P1-2. 전체 토픽 재전송 테스트가 lag 미소진 상태에서도 PASS할 수 있다

파일: [`htap-poc/e2e/run-faults.sh`](https://github.com/xmilex-git/workspace/blob/120bcf45000c2798eaa4da4006f895159537cd91/htap-poc/e2e/run-faults.sh)

S4는 consumer group offset을 earliest로 돌린 뒤 최대 40회 lag를 확인하지만, loop 종료 뒤 `LAG == 0`을 assert하지 않는다.

```bash
for _ in $(seq 1 40); do
    LAG=...
    [ "$LAG" = 0 ] && break
    sleep 3
done

echo "redelivery drained (lag=$LAG)"
converge
```

40회가 끝날 때 lag가 남아 있어도 다음 단계로 진행한다. 기존 canonical view가 이미 CUBRID와 같으면 `converge`는 즉시 성공할 수 있고, before/after view도 같으므로 **전체 재전송이 완료되지 않았는데도 테스트가 PASS할 수 있다.**

또한 코멘트에는 raw RMT에 `copies=2`가 관찰됐다고 기록되어 있지만, 스크립트 자체는 다음을 assert하지 않는다.

- reset 직후 committed offset이 실제 earliest로 이동했는지
- 재전송 전후 topic end offset과 consumer committed offset 변화
- 최종 lag가 정확히 0인지
- raw RMT 물리 행 수가 기대한 만큼 증가했는지
- 중복 행의 `_version`과 payload가 원본과 동일한지

#### 수정 권고

```bash
[ "${LAG:-}" = 0 ] || {
    echo "FAIL: replay lag did not drain: $LAG" >&2
    exit 1
}
```

여기에 offset reset 전후 값, topic end offset, raw duplicate count와 `_version` equality를 함께 assert해야 한다.

### P1-3. 빌드가 커밋에 고정되지 않고 acceptance build에서도 테스트를 전부 건너뛴다

파일: [`htap-poc/e2e/build-connector.sh`](https://github.com/xmilex-git/workspace/blob/120bcf45000c2798eaa4da4006f895159537cd91/htap-poc/e2e/build-connector.sh)

현재 스크립트는 로컬 worktree의 현재 상태를 그대로 빌드한다.

```bash
DEBEZIUM_SRC="${DEBEZIUM_SRC:-$HOME/htap-cdc/debezium}"
mvn package -DskipTests -DskipITs -Dcheckstyle.skip -Dformat.skip -Drevapi.skip -Denforcer.skip
```

문서에는 `37c57b8`이 기준이라고 적혀 있지만 스크립트가 해당 SHA를 검증하지 않는다. 브랜치 HEAD나 로컬 수정이 바뀌면 같은 명령으로 다른 바이너리가 만들어질 수 있다. 또한 커넥터 test tree에는 JNA smoke test 한 개만 존재하고, acceptance build는 그것마저 실행하지 않는다.

#### 수정 권고

- `DEBEZIUM_REF=37c57b8...`를 기본값으로 두고 detached checkout 또는 `git rev-parse HEAD` assert
- dirty worktree 거부 또는 명시적인 override 필요
- acceptance 단계에서는 최소 unit/smoke test 실행
- 생성된 JAR과 컨테이너 이미지의 SHA-256 기록
- 실행 증거 디렉터리에 Git SHA, config, image digest, stdout/stderr, connector status, offset dump 저장

### P2-1. 재시작 테스트가 crash point를 제어하지 않는다

S1/S2는 재시작 자체는 수행하지만, 다음 상태를 명시적으로 만들지 않는다.

- DML은 메모리 buffer에 있으나 COMMIT을 아직 읽지 않은 상태
- COMMIT은 읽었으나 일부 이벤트만 Kafka에 ack된 상태
- 모든 이벤트는 ack됐으나 source offset flush 전 상태
- sink가 ClickHouse에 썼으나 Kafka consumer offset commit 전 상태

테스트 명칭의 “mid-stream”보다 실제 검증 범위가 좁다. 정상 처리와 재시작 사이에 명시적 barrier를 두고, 로그/offset/topic count 조건을 만족한 뒤 kill하는 fault injection 방식으로 바꿔야 한다.

### P2-2. 실행 결과가 issue comment와 README 중심이고 immutable evidence bundle이 없다

스크립트와 커밋은 남아 있지만, 다음 증거가 기준 커밋에 함께 고정되어 있지 않다.

- 전체 실행 stdout/stderr
- Kafka Connect source offset 전후 dump
- Kafka topic partition별 시작/끝/committed offset
- ClickHouse raw/canonical 결과
- CUBRID 기준 결과
- 컨테이너 image digest와 CUBRID build SHA
- 각 fault가 실제 원하는 시점에 주입됐음을 보여 주는 timestamped log

POC 종결 증거는 사람이 작성한 PASS 설명보다, 한 번의 실행을 재검증할 수 있는 machine-readable bundle이 더 강하다.

### P3-1. 완료 계약 10번의 traceability가 약하다

CUBRID와 ClickHouse가 별도 endpoint로 사용되는 것은 코드에서 확인되지만, #40/#41의 완료 조건 표에는 10번 항목이 직접 매핑되어 있지 않다. 최종 resolution에는 1~10 전체 표를 넣어 누락 없이 닫는 편이 좋다.

---

## 6. 잘된 점

NO-GO 판정과 별개로, 현재 작업에서 유지해야 할 강점은 분명하다.

### 6.1 P0 사실 확인을 설계 결정과 분리했다

`cubrid_log`의 partial image, `all_in_cond`, DCL COMMIT/ABORT, trigger, 대형 트랜잭션 분할, replay 결정성, recovery ABORT를 별도 하네스로 확인했다. 엔진의 실제 동작을 추정으로 덮지 않고 ADR 전제조건을 실측한 접근은 적절하다.

### 6.2 replay 결정성의 핵심 전제를 별도 검증했다

동일 start LSA에서 30,072개 비-TIMER 아이템의 종류·trid·컬럼·순서가 동일함을 확인한 것은 synthetic counter 방식의 중요한 근거다. 배치 경계가 달라도 아이템 순서가 같다는 점까지 구분한 것도 좋다.

### 6.3 정상 E2E oracle은 단순 row count보다 강하다

`run-e2e.sh`는 최종 canonical view를 hard-coded expected state와 byte-level로 비교한다. ABORT 미반영, 다중 UPDATE, insert→delete, delete→insert, PK 변경을 한 흐름에서 확인한 것은 정상 기능 검증으로 충분히 의미가 있다.

### 6.4 토픽 계약과 sink 변환을 실제 출력으로 확정했다

`ExtractNewRecordState`, `add.fields.prefix=""`, deleted 필드 rename/cast, Decimal string, ZonedTimestamp 경로를 실제 Kafka→ClickHouse 결과로 고정했다. 문서만 보고 조합한 설정이 아니라 sink에 도달하는 wire contract를 실측했다는 점이 좋다.

### 6.5 알려진 한계를 숨기지 않았다

savepoint 부분 rollback, write-stop snapshot, fixed schema, JNA process 위험, timezone 의미론, host-side diff의 scale 한계 등을 문서에 남겼다. 향후 제품화에서 “POC에서 몰랐던 문제”로 변질될 가능성을 줄였다.

### 6.6 결정과 산출물의 추적성이 높다

이슈 → ADR → 커밋 → 스크립트로 연결되는 구조가 비교적 명확하다. P0 blocker 수정도 기존 ADR 0004의 “safe replay anchor” 불변식을 강화하는 형태로 반영하기 쉽다.

---

## 7. GO 전 필수 조치

아래 항목은 권고가 아니라 **Issue #30을 닫기 전 필수 조건**으로 보는 것이 타당하다.

### Gate A — P0 anchor 수정

- COMMIT 트랜잭션을 마지막 변경 이벤트 enqueue 완료 전까지 safe replay set에서 제거하지 않는다.
- 현재 COMMIT 트랜잭션을 포함한 가장 오래된 start anchor를 모든 해당 레코드의 source offset으로 사용한다.
- 모든 이벤트 enqueue 뒤에만 anchor를 남은 in-flight 또는 batch end로 전진시킨다.
- 단위 테스트로 offset 불변식을 직접 검증한다.

### Gate B — deterministic partial-publish crash test

- queue/batch를 작게 설정한다.
- 단일 30k+ 트랜잭션을 사용한다.
- 일부 record ack와 source offset flush가 끝난 시점에 JVM kill을 주입한다.
- 재시작 후 full-row keyed diff 0 mismatch를 확인한다.
- interleaved transaction과 네 crash point를 포함한다.

### Gate C — differential checker 강화

- PK를 포함한 canonical full-row digest로 변경한다.
- delimiter/newline/NULL을 충돌 없이 직렬화한다.
- value-swap tamper test를 추가한다.

### Gate D — S4 replay 증거 강화

- lag 0 강제 assert
- reset 전후 committed offset assert
- 전체 topic end까지 재소비됐는지 assert
- raw duplicate 수 및 동일 `_version` assert

### Gate E — 재현성 고정

- connector SHA 고정
- acceptance build에서 test 실행
- JAR/image checksum 기록
- 한 번의 `run-e2e + run-faults` 실행 결과를 immutable evidence bundle로 커밋 또는 artifact 보존

이 다섯 gate가 통과하면 Issue #30은 **Compact POC 범위에서 GO**로 전환해도 된다.

---

## 8. 재판정용 권장 테스트 순서

```text
1. connector anchor unit test
   - current committing txn이 safe anchor에서 빠지지 않는지 확인

2. baseline E2E
   - reset → snapshot → I/U/D/COMMIT/ABORT → exact expected state

3. large-transaction partial-publish kill
   - 30k+ rows, small queue/batch, fast offset flush
   - partial ack 확인 후 worker kill
   - restart → full-row diff 0

4. interleaved transaction kill
   - T1 start → T2 start → T1 commit/publish → kill → resume

5. source task restart / worker restart
   - 명시적 crash point별 수행

6. ClickHouse outage
   - writes continue → restore → lag 0 → keyed full-row diff 0

7. complete topic replay
   - earliest reset 실제 확인
   - end offset까지 drain
   - raw copies 증가 + same version/payload
   - canonical view byte-identical

8. negative tests
   - PK별 값 swap
   - row delete/extra row
   - NULL/empty string
   - delimiter/newline 포함 VARCHAR
   - Decimal scale 차이
```

모든 단계의 환경·commit·offset·결과를 한 디렉터리에 저장하고 마지막에 manifest checksum을 생성하는 것이 좋다.

---

## 9. Issue #42에 남길 수 있는 판정문 초안

```markdown
## Resolution — NO-GO: partial COMMIT publish 시 safe anchor 조기 전진 가능

정상 E2E, snapshot→streaming 전환, SMT/sink 계약, 기본 재시작·ClickHouse 장애·중복
시나리오는 통과했다. 다만 `CubridStreamingChangeEventSource`가 COMMIT DCL 처리 시
해당 `TxnBuffer`를 `inflight`에서 먼저 제거한 뒤 buffered changes를 publish한다.
그 결과 publish 중인 트랜잭션의 SourceRecord offset이 트랜잭션 시작 위치가 아니라
COMMIT batch 또는 더 나중 transaction의 시작 위치로 전진할 수 있다.

bounded ChangeEventQueue/poll batch를 넘는 트랜잭션에서 일부 레코드만 Kafka에 ack되고
해당 source offset이 저장된 뒤 워커가 죽으면, 재시작 위치에서 이전 DML을 재구성할 수 없어
나머지 이벤트가 유실될 수 있다. 기존 #41은 이 partial-publish crash window를 결정적으로
주입하지 않아 blocker를 배제하지 못한다.

따라서 #30은 현 상태 NO-GO다. 다음을 완료한 뒤 재판정한다.

1. COMMIT txn을 전체 enqueue 완료 전까지 safe replay anchor 계산에 포함
2. 30k+ 단일 txn + small queue/batch + partial ack 후 JVM kill E2E
3. keyed full-row differential check로 교체
4. S4 lag/offset/raw duplicate 강제 assert
5. pinned build + test/evidence bundle
```

---

## 10. POC가 GO로 전환된 뒤에도 제품화 전 남는 별도 blocker

아래는 현재 NO-GO의 직접 원인은 아니지만, Compact POC를 제품·알파로 확대하기 전에 별도 지도에서 해결해야 한다.

| 영역 | 현재 상태/위험 |
|---|---|
| savepoint 부분 rollback | 스트림에서 식별되지 않아 phantom change 가능 |
| transaction buffer | 메모리 무제한, spill/상한/backpressure 없음 |
| initial snapshot | 운영자 write-stop 필요, online snapshot/resnapshot 없음 |
| DDL/schema evolution | non-historized fixed schema, DDL 이벤트 미지원 |
| 권한 | POC config가 `dba`와 빈 비밀번호 사용 |
| JNA/native isolation | native library exit/segfault가 Connect worker 전체에 영향 가능 |
| HA/epoch | failover, log branch, epoch rotation 계약 미완성 |
| log retention | anchor가 오래된 txn에 묶일 때 보존 공간·lag 정책 없음 |
| observability | lag, anchor age, in-flight bytes, spill, sink backlog SLO 없음 |
| 성능 | CUBRID OLTP overhead, CDC throughput, end-to-end latency 장시간 측정 없음 |
| time semantics | DATETIME wall-clock과 timezone 계약이 POC 범위 밖 |
| packaging | host LAN IP·로컬 설치 경로·수동 mount에 강하게 결합 |
| CI/release | fork branch unprotected, required checks·tagged artifact 없음 |

이 항목들은 “이번 POC가 실패했다”는 뜻이 아니라, **POC 성공을 제품 준비 완료로 잘못 해석하면 안 된다는 경계**다.

---

## 11. 최종 의견

이번 작업은 방향성과 정상 경로 구현 측면에서는 성공에 가깝다. 특히 CUBRID 로그의 불명확한 부분을 하네스로 실측하고, Debezium 표준 구조와 ClickHouse 공식 sink를 연결해 실제 canonical view까지 관통시킨 점은 다음 단계로 갈 가치가 충분하다.

그러나 CDC 시스템의 마지막 판정 기준은 “정상적으로 보였다”가 아니라 **어느 crash window에서도 저장된 offset보다 앞의 미전달 데이터를 잃지 않는가**다. 현재 커넥터는 COMMIT 트랜잭션을 publish하기 전에 safe anchor 계산 대상에서 제거하므로 이 기준을 만족한다고 볼 수 없다.

따라서 현 시점 판정은 명확히 **NO-GO**다. 다만 blocker의 위치와 수정 방향이 구체적이고, 나머지 수직 경로가 이미 완성되어 있으므로 P0 수정과 재검증이 끝나면 Compact POC 자체는 빠르게 GO로 전환할 수 있는 상태다.

---

## 12. 검토한 주요 자료

- [Issue #30](https://github.com/xmilex-git/workspace/issues/30)
- [Issue #40 — E2E 수직 슬라이스](https://github.com/xmilex-git/workspace/issues/40)
- [Issue #41 — 장애·재시작·중복 검증](https://github.com/xmilex-git/workspace/issues/41)
- [Issue #42 — Go/No-Go 판정](https://github.com/xmilex-git/workspace/issues/42)
- [Issue #43 — replay 결정성](https://github.com/xmilex-git/workspace/issues/43)
- [Issue #44 — recovery ABORT](https://github.com/xmilex-git/workspace/issues/44)
- [`htap-cubrid.md`](https://github.com/xmilex-git/workspace/blob/main/htap-cubrid.md)
- [`htap-poc/e2e/README.md`](https://github.com/xmilex-git/workspace/blob/120bcf45000c2798eaa4da4006f895159537cd91/htap-poc/e2e/README.md)
- [`run-e2e.sh`](https://github.com/xmilex-git/workspace/blob/120bcf45000c2798eaa4da4006f895159537cd91/htap-poc/e2e/run-e2e.sh)
- [`run-faults.sh`](https://github.com/xmilex-git/workspace/blob/120bcf45000c2798eaa4da4006f895159537cd91/htap-poc/e2e/run-faults.sh)
- [`diff-check.sh`](https://github.com/xmilex-git/workspace/blob/120bcf45000c2798eaa4da4006f895159537cd91/htap-poc/e2e/diff-check.sh)
- [`build-connector.sh`](https://github.com/xmilex-git/workspace/blob/120bcf45000c2798eaa4da4006f895159537cd91/htap-poc/e2e/build-connector.sh)
- [`CubridStreamingChangeEventSource.java`](https://github.com/xmilex-git/debezium/blob/37c57b8f75918c8ab9fc98bfc6975f98b53888d3/debezium-connector-cubrid/src/main/java/io/debezium/connector/cubrid/CubridStreamingChangeEventSource.java)
- [`CubridOffsetContext.java`](https://github.com/xmilex-git/debezium/blob/37c57b8f75918c8ab9fc98bfc6975f98b53888d3/debezium-connector-cubrid/src/main/java/io/debezium/connector/cubrid/CubridOffsetContext.java)
- [`CubridConnectorTask.java`](https://github.com/xmilex-git/debezium/blob/37c57b8f75918c8ab9fc98bfc6975f98b53888d3/debezium-connector-cubrid/src/main/java/io/debezium/connector/cubrid/CubridConnectorTask.java)
- [`EventDispatcher.java`](https://github.com/xmilex-git/debezium/blob/37c57b8f75918c8ab9fc98bfc6975f98b53888d3/debezium-core/src/main/java/io/debezium/pipeline/EventDispatcher.java)
- [`CommonConnectorConfig.java`](https://github.com/xmilex-git/debezium/blob/37c57b8f75918c8ab9fc98bfc6975f98b53888d3/debezium-core/src/main/java/io/debezium/config/CommonConnectorConfig.java)
- [`ChangeEventSourceCoordinator.java`](https://github.com/xmilex-git/debezium/blob/37c57b8f75918c8ab9fc98bfc6975f98b53888d3/debezium-core/src/main/java/io/debezium/pipeline/ChangeEventSourceCoordinator.java)
- [`BaseSourceTask.java`](https://github.com/xmilex-git/debezium/blob/37c57b8f75918c8ab9fc98bfc6975f98b53888d3/debezium-core/src/main/java/io/debezium/connector/common/BaseSourceTask.java)
