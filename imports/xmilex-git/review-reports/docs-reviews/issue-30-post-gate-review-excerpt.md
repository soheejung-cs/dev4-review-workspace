# Issue #30 후속 외부 리뷰 발췌 — §7 새로 확인되거나 남아 있는 위험

> Gate A~D 해소(#45, #46) 이후 수행된 후속 외부 리뷰의 §7 발췌.
> 전문은 확보되지 않아 발췌만 보존한다. #42 최종 판정(GO)의 입력으로 사용됐고,
> 각 항목의 처분은 `docs/htap-poc-future-work.md`에 기록되어 있다.
> 보존일: 2026-08-17.

---

## 7. 새로 확인되거나 남아 있는 위험

### 7.1 P0 — 제품화 선결: savepoint·문장 실패 rollback이 CDC에 반영되지 않음

가장 중요한 잔여 문제다. s06뿐 아니라 s10 실측에서 다음이 확인됐다.

- 트랜잭션 중 멀티행 INSERT의 일부가 수행된 뒤 PK 위반으로 문장이 실패
- 애플리케이션이 오류를 잡고 트랜잭션 전체를 ROLLBACK하지 않은 채 COMMIT
- CUBRID 내부 문장 rollback으로 취소된 DML이 supplemental stream에는 보상 없이 남음
- ClickHouse에는 커밋되지 않은 팬텀 행이 생기거나 더 높은 `_version`으로 실존 행이 오염됨

따라서 다음 애플리케이션 패턴은 현재 커넥터에서 안전하지 않다.

```text
BEGIN
  statement A 성공
  statement B 일부 수행 후 실패
  애플리케이션이 예외를 catch
  statement C 수행
COMMIT
```

트랜잭션 전체가 ABORT되면 버퍼를 폐기하므로 안전하지만, 문장 실패 후 COMMIT하는 일반적인 retry/duplicate-ignore 패턴은 안전하지 않다.

이 문제는 커넥터가 현재 event stream만 보고 해결할 수 없다. CUBRID supplemental log에 savepoint rollback·문장 rollback·보상 이벤트 또는 이에 준하는 트랜잭션 내부 rollback 경계가 노출되어야 한다.

**판정 영향**

- Compact POC: 알려진 미지원 워크로드로 명시하고 GO 가능
- 임의 애플리케이션 트래픽: P0 blocker
- 제품화: 엔진 보강 전 NO-GO

### 7.2 P1 — 대형·장기 트랜잭션의 무제한 메모리 버퍼

현재 트랜잭션별 DML은 COMMIT까지 메모리에 보관되며 명시적인 상한·spill·backpressure 정책이 없다. 30K는 통과했지만 수백만 행, 장기 미커밋, 다수 동시 트랜잭션에서는 Connect worker OOM 또는 지연 증폭 가능성이 있다.

제품화 전 최소한 다음이 필요하다.

- txn별·전체 buffered bytes limit
- disk spill 또는 source-side pause/backpressure
- max transaction age와 운영 경보
- 재연결 시 in-flight state 처리 규칙
- OOM 이전 fail-fast 및 재처리 가능 상태 유지

### 7.3 P1 — 성능 타당성 미증명

이번 POC는 정합성 실험이다. 다음은 아직 측정되지 않았다.

- CUBRID supplemental logging overhead
- Debezium/JNA extraction throughput
- Kafka 및 sink 처리량
- 정상·장애 후 catch-up lag
- ClickHouse `FINAL` 조회 비용
- update-heavy workload에서 RMT physical amplification
- CUBRID 직접 분석 대비 ClickHouse 실제 성능 이득

따라서 "HTAP 경로가 동작한다"는 결론은 가능하지만 "분석이 빨라진다", "제품으로 투자할 가치가 있다"는 결론은 아직 불가능하다.

### 7.4 P1 — 고정 스키마와 운영 snapshot

현재는 다음 전제가 있다.

- DDL/schema evolution 미지원
- owner/schema 단순화
- 초기 snapshot 중 운영자 write stop
- snapshot `max.threads=1`
- online snapshot token 없음
- offset만 삭제하는 운영 금지, snapshot과 반드시 결합

제품화에서는 schema barrier, rebuild, online/incremental snapshot, resnapshot 운영 절차가 필요하다.

### 7.5 P1 — HA와 epoch는 자리만 예약됨

`_version`에 epoch 비트가 예약되어 있지만 실제 CUBRID HA failover, split-brain, source switch, old primary event 재등장은 검증하지 않았다. 단일 source·단일 node POC에서는 문제없지만 HA 제품 계약으로 확장할 수 없다.

### 7.6 P2 — JNA와 worker blast radius

현재 방식은 도달 가능성을 우선해 `libcubrid_log`를 JNA로 감싼다. 다음 운영 부담이 있다.

- 네이티브 라이브러리 crash/exit가 Connect worker 전체에 영향
- CUBRID 설치본 전체 마운트와 `CUBRID`, `LD_LIBRARY_PATH` 의존
- upstream Debezium 제출 난이도
- CUBRID 버전과 `.so` ABI 결합

POC에서는 수용 가능하나 제품화 때 worker 격리 또는 순수 Java 포팅을 재검토해야 한다.
