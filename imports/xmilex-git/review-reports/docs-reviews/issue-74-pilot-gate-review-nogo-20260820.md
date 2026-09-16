# workspace 이슈 #74 사내 파일럿 게이트 재리뷰

- 대상: [xmilex-git/workspace#74](https://github.com/xmilex-git/workspace/issues/74)
- 제목: **HTAP 제품화 2차 지도: P0 silent-corruption 차단 해소 → 사내 파일럿 게이트**
- 검토 기준일: **2026-08-20 (KST)**
- 판정 범위: **이슈 #74가 선언한 “사내 파일럿 가능” 게이트**
- 판정 방식: 이전 #48 리뷰에서 확인한 P0 항목을 구현·원격 소스·테스트 증거·운영 전제조건과 일대일 대조

---

# 최종 판정: NO-GO

이슈 #74에서 수행한 설계와 개별 수정의 방향은 전반적으로 좋다. 특히 relation identity fail-closed, UTF-8 가드, snapshot barrier node identity 보강은 이전 리뷰의 핵심 문제를 정확히 겨냥했다.

그러나 **현재 GitHub에서 재현·검토 가능한 상태**를 기준으로 보면 사내 파일럿 게이트를 통과했다고 판정할 수 없다. 가장 중요한 이유는 다음과 같다.

1. temporal P0 본체를 해결했다고 기록한 커넥터 커밋 `26d30ed`가 원격 저장소에 존재하지 않는다.
2. temporal·파티션 엔진 수정 커밋 `d2e106aff`, `bdbeaf3f1`도 원격 저장소에서 조회되지 않는다.
3. 현재 공개된 커넥터 `main`은 `67d0a0d`이며, 여전히 `TIMESTAMP`와 `DATETIME`을 동일한 `ZonedTimestamp` 계약으로 처리하는 이전 P0 코드다.
4. 지도 종착 티켓 #81이 open·댓글 0건으로, 최종 커밋 쌍에서의 전 스위트 재실행·파일럿 전제조건 집약·최종 게이트 판정이 수행되지 않았다.
5. CDC 포트 격리 증거는 실제 파일럿 서버의 firewall allowlist를 검증하지 않으며, 체크인된 증거가 이슈에 적힌 DELETE before-image 노출 주장과도 일치하지 않는다.
6. #83에서 새로 발견된 “DB당 CDC source connector 1개” 제약 등 필수 운영 전제조건이 최종 지원 문서에 반영되지 않았다.

이는 공식 1.0용 P1을 요구해서 내린 판정이 아니다. **#74 자체가 요구한 P0 해소와 사내 파일럿 종착 조건이 아직 완결되지 않았기 때문에 NO-GO다.**

---

## 1. 판정 기준과 현재 공개 기준선

### 1.1 이슈 #74의 자체 완료 조건

#74의 destination은 다음과 같다.

> #48 리뷰가 확인한 P0 silent-corruption 차단 항목을 해소하고, 단순 제한 문서가 아니라 기동 가드·fail-fast로 강제된 상태에서 사내 파일럿 게이트를 통과한다.

현재 #74는 다음 상태다.

- 상태: **open**
- 하위 이슈: **12개 중 10개 완료, 83%**
- 미완료:
  - [#81 종착 — P0 해소 검증 + 사내 파일럿 게이트 재판정·전제조건 문서](https://github.com/xmilex-git/workspace/issues/81)
  - [#86 TZ 계열 4종 지원](https://github.com/xmilex-git/workspace/issues/86) — 파일럿 게이트 비필수

#86은 지원 대상에서 계속 fail-fast한다면 게이트 차단 사유가 아니다. 그러나 **#81은 바로 이 지도의 종착 게이트이므로 미완료 자체가 핵심 차단 사유**다.

### 1.2 원격에서 실제 조회되는 기준선

| 저장소 | 이슈 기록상 최종 기준선 | 2026-08-20 원격에서 확인된 상태 |
|---|---|---|
| workspace | `79083ac57fdbf37e50dcd7960dcc20e5438f9b01` | 확인됨 |
| debezium-connector-cubrid | `26d30ed` | **커밋 조회 불가** |
| debezium-connector-cubrid `main` | — | `67d0a0dccfaa6760dbdacda98e1d00cbb2893bec` |
| CUBRID engine | `d2e106aff` → `bdbeaf3f1` | **두 커밋 모두 조회 불가** |
| CUBRID 원격 HTAP 브랜치 | `htap/cdc-select-privilege` 주장 | 검색 가능한 브랜치는 `htap/cdc-relation-dictionary`뿐 |

workspace에는 로컬 빌드·실측 결과가 기록돼 있지만, 그 결과를 만든 최종 제품 소스가 원격 저장소에 없다. 따라서 다음을 독립적으로 검증할 수 없다.

- `26d30ed`가 실제로 #77·#78·#82 수정까지 포함하는지
- `26d30ed`의 `CubridTemporal`, typeName 분기, JDBC UTC 고정 구현이 설명과 일치하는지
- `d2e106aff`의 partition root 매핑과 representation decode 수정이 안전한지
- `bdbeaf3f1`의 temporal wire v2 구현이 문서와 byte 단위로 일치하는지
- 세 커밋의 정확한 부모 관계와 lockstep 조합

특히 #85 완료 코멘트는 `26d30ed`의 기준선을 `387df7a`로 표현한다. 이것이 단순한 프로젝트 최초 기준선 표기인지, 실제 커밋 부모인지 확인할 커밋 그래프가 없다. 실제 부모가 `387df7a`라면 #77·#78·#82 수정이 빠질 수 있으므로, **`67d0a0d`가 `26d30ed`의 조상인지 반드시 확인돼야 한다.**

---

## 2. 이전 P0 항목별 재검토

| 이전 P0 | 수행 결과 | 현재 게이트 상태 |
|---|---|---|
| P0-1 DROP/RENAME·lag relation identity | 커넥터 수정은 원격 코드로 확인 | 엔진 최종 코드 미공개, 원래 장애의 실 E2E 부족 |
| P0-2 temporal wire 계약 | wire v2 명세와 실측 증거 작성 | 최종 엔진·커넥터 소스 미공개 |
| P0-3 TIMESTAMP/DATETIME 의미 분리 | 로컬 완료 기록 존재 | 공개 `main`에는 이전 P0 코드가 그대로 존재 |
| P0-4 비 UTF-8 silent corruption | UTF-8-only startup guard 구현 확인 | 구현 수준에서 해소 확인 |
| P0-5 snapshot barrier node identity | barrier stamp와 anchored fail-closed 확인 | 구현 확인, 잔여 운영 제약은 종착 문서 미완료 |
| P0-6 CDC 포트 보안 | 위험성 문서화 및 대리 실험 수행 | 실제 firewall gate 및 증거 정합성 미충족 |
| 최종 파일럿 종착 | #81에서 수행 예정 | **미수행** |

---

# 3. 차단 이슈

## BLOCKER-1. temporal P0 수정이 원격 제품 소스에 존재하지 않는다

[#85 완료 기록](https://github.com/xmilex-git/workspace/issues/85#issuecomment-5350432191)은 다음을 구현했다고 설명한다.

- `TIMESTAMP` → 진짜 instant인 `ZonedTimestamp`
- `DATETIME` → offset 없는 ISO-8601 문자열
- strict wire v2 parser `CubridTemporal`
- 모든 JDBC 접속에서 `SET TIME ZONE 'UTC'`
- 비 UTC JVM 테스트
- 단위 114/114, E2E PASS, diff 0 mismatch

설계와 증거의 방향은 타당하다. 그러나 커넥터 커밋 `26d30ed`가 원격 저장소에 없고, 원격 `main`은 여전히 `67d0a0d`다.

현재 공개 코드에는 이전 P0가 그대로 남아 있다.

- [`CubridLogValueDecoder.java@67d0a0d`](https://github.com/xmilex-git/debezium-connector-cubrid/blob/67d0a0dccfaa6760dbdacda98e1d00cbb2893bec/src/main/java/io/debezium/connector/cubrid/CubridLogValueDecoder.java)
  - `Types.TIMESTAMP`를 단일 경로로 처리
  - AM/PM 포맷을 먼저 파싱하고 `Timestamp.valueOf()`로 fallback
- [`CubridValueConverters.java@67d0a0d`](https://github.com/xmilex-git/debezium-connector-cubrid/blob/67d0a0dccfaa6760dbdacda98e1d00cbb2893bec/src/main/java/io/debezium/connector/cubrid/CubridValueConverters.java)
  - 모든 JDBC `TIMESTAMP` 타입을 `ZonedTimestamp`로 변환
  - 주석에도 `DATETIME/TIMESTAMP`를 worker JVM default zone 기반 wall-clock passthrough로 설명
- [`support-scope.md@67d0a0d`](https://github.com/xmilex-git/debezium-connector-cubrid/blob/67d0a0dccfaa6760dbdacda98e1d00cbb2893bec/docs/support-scope.md)
  - 여전히 “시간 타입은 wall-clock passthrough, 워커 UTC 운영 규율”이라고 기술

즉, 사용자가 GitHub의 현재 `main`을 받아 빌드하면 이전 리뷰의 temporal P0가 해소되지 않는다. 로컬 실측 파일만으로는 파일럿 배포 기준선을 승인할 수 없다.

### 추가 발견: lockstep mismatch가 항상 fail-loud하지 않는다

wire v2 문서는 “구 엔진 v1 ↔ 신 커넥터 조합은 strict parser에서 요란하게 실패한다”고 설명한다. 신 커넥터가 구 AM/PM 텍스트를 거부하는 것은 타당하다.

그러나 반대 방향인 **신 엔진 v2 ↔ 현재 공개된 구 커넥터**는 항상 실패하지 않는다.

현재 공개된 decoder는 AM/PM 파싱 실패 후 `java.sql.Timestamp.valueOf(value)`를 호출한다. 따라서 wire v2의 다음 문자열은 정상 파싱된다.

- `2026-01-02 03:04:05`
- `2026-01-02 03:04:05.600`

그 결과 v2 엔진과 구 커넥터가 연결돼도 `TIMESTAMP`/`DATETIME`만 있는 스키마에서는 즉시 실패하지 않을 수 있다. 특히 `DATETIME`은 다시 UTC `Z`가 붙은 instant처럼 발행되어 기존 의미 오류가 조용히 지속될 수 있다.

버전 필드가 없는 lockstep 정책을 유지하려면 최소한 **배포 가능한 exact pair를 고정하고 잘못된 조합을 설치 단계에서 차단**해야 한다. 현재는 그 pair 자체가 원격에 없다.

---

## BLOCKER-2. 엔진의 핵심 P0 수정이 로컬 worktree에만 있다

[#83 완료 기록](https://github.com/xmilex-git/workspace/issues/83#issuecomment-5339053181)은 엔진 `d2e106aff`에서 다음을 수정했다고 한다.

- encoding-safe ALTER를 `CDC_INDEX`로 재분류
- partition DDL 분류
- partition classoid를 root로 매핑
- root와 partition representation 계보 혼동으로 발생한 오디코딩·스트림 오염 수정

[#84 완료 기록](https://github.com/xmilex-git/workspace/issues/84#issuecomment-5339308711)은 `bdbeaf3f1`에서 다음을 수정했다고 한다.

- dead ISO temporal format 활성화
- LTZ `TZR` → `TZH:TZM`
- CDC daemon timezone을 UTC로 고정

두 구현은 P0 차단의 핵심이다. 특히 #83에서 새로 발견한 representation decode 버그는 잘못 구현하면 row decode와 후속 item parsing 전체를 오염시킬 수 있는 고위험 변경이다.

그러나 두 커밋과 `htap/cdc-select-privilege` 브랜치가 원격에서 조회되지 않는다. 따라서 코드 diff, 테스트, 호출 경로, 오류 처리, 메모리 안전성, 기존 CDC/flashback 영향 범위를 독립적으로 리뷰할 수 없다.

사내 파일럿이라도 실제 데이터에 적용할 엔진 패치가 “로컬 설치본에만 존재”하는 상태는 게이트 통과 상태가 아니다.

---

## BLOCKER-3. 종착 게이트 #81이 수행되지 않았다

[#81](https://github.com/xmilex-git/workspace/issues/81)은 이 지도의 종착 티켓이며 다음을 완료 조건으로 둔다.

1. P0 silent-corruption 방지 체크리스트 전수 확인
2. `support-scope.md`에 사내 파일럿 전제조건 절 신설
3. E2E·fault campaign 전 스위트 재실행 및 기준선 커밋 쌍 기록
4. 사내 파일럿 GO/NO-GO 판정 기록
5. 공식 1.0 잔여 P1 정리 및 #48 후속 안내

현재 #81은 open이고 댓글이 없다. 따라서 각 구현 티켓에서 개별적으로 실행한 테스트는 존재하지만, **최종 엔진·커넥터 조합 하나를 고정해 모든 회귀를 다시 통과시킨 증거가 없다.**

특히 #85 증거에는 다음만 명시돼 있다.

- 단위 114/114
- `run-e2e.sh`
- `diff-check.sh`
- temporal Kafka 실물 확인

최종 pair에서 다음이 함께 재실행됐다는 통합 증거는 없다.

- crash/fault S1–S4
- snapshot fault SN1–SN4
- blocking snapshot BS1
- owner collision
- transaction replay/counter determinism
- savepoint·statement rollback
- relation empty/rename/drop lag
- charset negative/parity
- HA barrier/stream node mismatch
- partition DDL·representation decode
- CDC 포트 실제 allowlist denial

서로 다른 커밋에서 개별 PASS한 결과를 합산해 최종 pair가 PASS했다고 간주할 수 없다. #83·#84·#85는 엔진 protocol과 decode 경로를 연속으로 변경했기 때문에 최종 통합 재실행이 특히 필요하다.

---

## BLOCKER-4. CDC 포트 격리 검증이 실제 파일럿 보안 게이트가 아니다

[#79](https://github.com/xmilex-git/workspace/issues/79#issuecomment-5337969932)은 CDC 포트가 서버 보안 경계가 아니며 OS/network 계층 격리가 필수라고 정확히 판단했다. 이 판단 자체는 맞다.

그러나 체크인된 [`run-port-isolation-denial.sh`](https://github.com/xmilex-git/workspace/blob/79083ac57fdbf37e50dcd7960dcc20e5438f9b01/htap-poc/e2e/run-port-isolation-denial.sh)는 파일럿 환경의 방화벽을 검증하는 gate test가 아니다.

### 4.1 실제 firewall allowlist를 시험하지 않는다

DENIAL 경로는 다음 대상에 접속한다.

- `127.0.0.1:1` — 리스너가 없는 포트
- `192.0.2.1:1523` — RFC 5737 TEST-NET 주소

이는 “접근할 수 없으면 연결되지 않는다”는 일반 사실을 보여줄 뿐이다. 다음 핵심 조건은 검증하지 않는다.

- 허용된 Connect 워커 → 실제 CUBRID CDC 포트 연결 성공
- 허용되지 않은 별도 호스트 → **같은 실제 CUBRID CDC 포트** 연결 실패
- 적용된 `nftables`/`iptables`/보안그룹 규칙
- 일반 사용자망에서 실제 서버 IP:CDC_PORT가 차단되는지

### 4.2 실패해도 스크립트가 성공 종료할 수 있다

스크립트는 `set -uo pipefail`만 사용하고 `set -e`가 없다.

- RISK 연결이 실패하면 `[FAIL]`을 출력하지만 종료하지 않는다.
- DENIAL 결과가 예상과 다르면 `[WARN]`만 출력한다.
- 민감 DML이 실제로 보였는지 assert하지 않는다.
- 마지막에 항상 완료 문구를 출력한다.

따라서 CI나 수동 gate에서 실패를 놓칠 수 있다.

### 4.3 체크인된 증거가 이슈 설명과 일치하지 않는다

[`issue-79-raw-attach.txt`](https://github.com/xmilex-git/workspace/blob/79083ac57fdbf37e50dcd7960dcc20e5438f9b01/htap-poc/e2e/evidence/issue-79-raw-attach.txt)의 실제 내용은 다음과 같다.

- 접속 사용자: `dba`
- `CONNECT rc=0`은 확인됨
- `leaked-secret`, `pii-row` 문자열이 없음
- DELETE before-image가 없음
- 관측된 transaction은 `ROLLBACK_TO` 후 `ABORT`

반면 #79 완료 코멘트는 UPDATE 값 `leaked-secret`, INSERT/DELETE 값 `pii-row`, DELETE before-image 노출을 실증했다고 기록한다. 체크인된 증거로는 이 주장을 뒷받침할 수 없다.

스크립트가 `CREATE TABLE IF NOT EXISTS` 뒤에 고정 PK `1`을 INSERT하고 오류를 `|| true`로 삼키기 때문에 재실행 시 duplicate key 등으로 transaction이 abort될 수 있다. 현재 증거가 바로 그 형태다.

### 4.4 필요한 실제 gate

- 허용 호스트에서 실제 서버 CDC 포트 연결 성공
- 비허용 호스트에서 동일 서버·동일 포트 연결 실패
- firewall/ACL 설정 snapshot 보관
- 비-DBA 또는 권한이 없는 raw client 시나리오도 별도 검증
- 테스트 데이터는 매번 고유 PK 사용 또는 사전 정리
- UPDATE·DELETE before-image를 반드시 assert
- 하나라도 불일치하면 non-zero exit

현재 증거는 CDC 포트의 위험성을 확인하는 연구 자료로는 유효하지만, **파일럿 환경이 실제로 안전하게 격리됐음을 승인하는 증거는 아니다.**

---

## BLOCKER-5. 원래 P0 재현 시나리오의 실제 E2E가 부족하다

#82의 커넥터 구현은 코드 수준에서 이전 문제를 잘 차단한다.

- empty/half-empty relation announce fail-fast
- announce 대상이 include list 밖이면 fail-fast
- include-list 모든 테이블을 기동 시 실재·schema load 검증
- TABLE ALTER/DROP/RENAME/TRUNCATE 무조건 halt
- halt 시 batch-end anchor 전진 방지

원격 코드와 단위 테스트에서 이 방향을 확인했다.

그러나 #82의 라이브 E2E는 다음만 수행했다.

- 존재하지 않는 include entry 기동 거부
- `ALTER TABLE` halt 및 재시작 re-halt
- resnapshot 후 일반 E2E

이전 P0의 정확한 장애 경로인 다음 시나리오는 synthetic unit 수준에 머문다.

- committed DML → extractor lag → DROP → empty relation
- committed DML → extractor lag → RENAME → include mismatch
- 커넥터 정지 중 DROP 후 재시작
- 커넥터 정지 중 RENAME 후 재시작

이 경로는 엔진의 extraction-time relation resolution과 커넥터의 처리 순서가 함께 작동해야 하므로, 커넥터 단위 테스트만으로 충분하지 않다. 최종 엔진 pair에서 실제 로그·anchor·task state·ClickHouse diff를 확인해야 한다.

---

## BLOCKER-6. 필수 파일럿 전제조건 문서가 불완전하고 일부 내용이 서로 모순된다

현재 원격 `support-scope.md`에는 charset, HA 잔여 갭, 포트 격리 등의 내용이 추가됐다. 하지만 #81이 요구한 **단일 사내 파일럿 전제조건 절**은 없다.

또한 현재 문서는 #85의 새 temporal 계약과 모순된다.

- 현재 원격 문서: wall-clock passthrough, 워커 UTC 운영 규율
- #85 완료 주장: TIMESTAMP는 true instant, DATETIME은 offset-less ISO, worker default zone 불개입

#83에서 새로 확인된 다음 운영 조건도 아직 커넥터 문서에 없다.

- **DB당 CDC source connector 1개만 허용**
  - 신규 START_SESSION이 기존 세션을 대체
  - 같은 DB에 connector 2개가 붙으면 서로 세션을 뺏어 crash-loop 가능
- 엔진 재설치 후 `cubrid.conf`가 덮여 `supplemental_log=1`이 사라질 수 있으므로 재확인 필요
- 커넥터의 유휴 JDBC 세션이 read lock을 잡아 계획되지 않은 ALTER가 대기할 수 있으므로 계획 DDL은 커넥터 정지 후 수행
- JDBC snapshot 경로와 CDC barrier 경로가 같은 실제 서버를 바라보도록 broker `databases.txt`를 고정
- exact engine/connector full SHA pair

운영자가 현재 공개 문서만 따라 파일럿을 구성하면 시간 계약을 잘못 이해하거나, connector를 두 개 띄워 crash-loop를 만들거나, 재설치 후 supplemental log가 꺼진 상태로 운용할 수 있다.

---

# 4. 잘 진행된 부분

## 4.1 relation identity fail-closed 방향은 정확하다

[`67d0a0d`](https://github.com/xmilex-git/debezium-connector-cubrid/commit/67d0a0dccfaa6760dbdacda98e1d00cbb2893bec)의 핵심 수정은 이전 P0의 본질을 제대로 해결한다.

- empty relation을 정상적인 `null` route로 보관하지 않음
- relation dictionary를 include-list 부분집합으로 강제
- restart 시 missing/renamed table을 bootstrap에서 잡음
- DDL halt 판정에서 현재 schema lookup을 제거
- TABLE DDL이 도착하면 unconditionally halt
- 오류에 원인·조치·runbook pointer를 포함

이 부분은 단순 문서 제약이 아니라 코드 fail-fast로 강제됐다는 점에서 좋다.

## 4.2 UTF-8-only 가드는 제품적으로 안전한 선택이다

[`274a974`](https://github.com/xmilex-git/debezium-connector-cubrid/commit/274a97406e111c2d6dd01ca6bd3ead58124504c0)은 다음을 명확하게 강제한다.

- `SELECT charset FROM db_root`
- UTF-8 charset id만 allow
- EUC-KR·ISO-8859-1·미지 codeset은 startup fail-fast
- 실제 EUC-KR DB negative smoke와 한글 parity 검증

다중 charset을 어설프게 지원하기보다 1.0 범위를 UTF-8로 고정한 것은 적절하다.

## 4.3 snapshot barrier node identity 보강은 이전 공백을 정확히 막았다

[`fc6bbce`](https://github.com/xmilex-git/debezium-connector-cubrid/commit/fc6bbce8f868bd4f2c9c65a16d533994c3c8e3fd)는 다음을 구현한다.

- barrier 시점에 node identity stamp
- interrupted snapshot anchor 재사용 전 identity 비교
- anchored offset인데 node가 없으면 fail-closed
- streaming 시작 시 barrier node와 live node 불일치 검출

VIP + 동일 creation time 등 구조적으로 검출 불가능한 잔여 갭도 숨기지 않고 문서 제약으로 남긴 점이 좋다.

## 4.4 temporal wire v2 설계 자체는 이전 계약보다 명확하다

[wire v2 명세](https://github.com/xmilex-git/workspace/blob/79083ac57fdbf37e50dcd7960dcc20e5438f9b01/docs/htap-cdc-wire-v2.md)는 다음을 잘 구분한다.

- TIMESTAMP: UTC wall-clock → true instant
- DATETIME: zone-less local date-time
- DATE/TIME: zone-less
- TZ 계열: numeric offset 포함
- DB charset 미표기 한계와 UTF-8 가드 연계
- strict parser와 byte-exact 포맷

#85의 체크인된 실측 증거도 epoch=1, KST 서버, 비 UTC JVM, snapshot/streaming parity를 다룬 점은 좋다. 문제는 설계가 아니라 **그 구현 소스가 원격에 고정되지 않은 상태**다.

## 4.5 구현 중 잠복 partition decode 문제까지 찾은 점은 가치가 크다

#83에서 root classoid로 route하되 실제 record decode는 소유 partition class의 representation을 사용해야 한다는 문제를 발견한 것은 중요한 성과다. 이 문제는 조용한 오디코딩과 후속 item stream 붕괴로 이어질 수 있으므로 발견 자체가 매우 유의미하다.

다만 바로 그만큼 위험한 변경이므로, 로컬 worktree가 아니라 원격 커밋으로 코드 리뷰와 회귀 검증이 가능해야 한다.

---

# 5. 이번 판정에서 차단 사유로 사용하지 않은 항목

#74가 공식 1.0 P1을 명시적으로 다음 지도로 미뤘으므로, 다음 항목은 이번 NO-GO의 직접 근거로 사용하지 않았다.

- Maven Central·공식 plugin archive·SBOM
- CUBRID org 이관과 Debezium upstream 기증
- final artifact 성능 재측정
- branch protection·release-grade CI
- protocol manifest 정식 제품화
- transaction buffer spill/strict mode
- blocking snapshot retention 충돌
- unknown DML/DCL fail-fast 전면 강화
- 공식 CUBRID manual RST 편입
- #86 TZ 4종 신규 지원

즉, 이 보고서는 P1을 끌어와 기준을 높인 것이 아니다. **P0 구현 배포본과 사내 파일럿 종착 증거가 아직 완성되지 않았다는 이유만으로 NO-GO를 판정한다.**

---

# 6. GO로 전환하기 위한 최소 조치

## 6.1 최종 소스를 원격에 고정

1. 커넥터 `26d30ed` 또는 그 후속 통합 커밋을 원격 branch에 push
2. 엔진 `d2e106aff`, `bdbeaf3f1`을 포함한 branch를 원격에 push
3. 다음 조상 관계 확인
   - `67d0a0d` ⊆ 최종 커넥터
   - `d2e106aff` ⊆ `bdbeaf3f1`
   - 기존 `2f24ddc0b`의 권한·relation·node facts 수정 ⊆ 최종 엔진
4. full 40자리 SHA를 #74·#81·support-scope에 기록
5. 최소한 사내 파일럿용 immutable tag 또는 고정 branch 생성

## 6.2 현재 공개 문서와 최종 코드를 일치시킴

- wall-clock passthrough 설명 삭제
- TIMESTAMP true instant / DATETIME zone-less 계약 반영
- `SET TIME ZONE 'UTC'`가 모든 JDBC connection에서 강제됨을 명기
- TZ 4종은 #86 전까지 startup fail-fast 유지
- DB당 source connector 1개 제한
- JDBC/CDC same-server 구성
- UTF-8 DB only
- fixed include list + 변경 시 전체 resnapshot
- 실제 firewall allowlist
- 엔진 재설치 후 `supplemental_log=1` 재확인
- 계획 DDL 시 connector stop 절차
- exact engine/connector pair

## 6.3 실제 P0 fault test 실행

- DML → DROP, extractor lag
- DML → RENAME, extractor lag
- connector-down → DROP → restart
- connector-down → RENAME → restart
- empty/half-empty relation announce
- DDL 직전·직후 crash 및 deterministic re-halt
- barrier node A → streaming node B
- interrupted snapshot을 다른 node에서 재개
- EUC-KR startup denial
- 비 UTC JVM + KST server temporal matrix
- TIMESTAMP/DATETIME 동일 표시값 차등
- partition root route + post-ALTER new partition representation decode

## 6.4 실제 포트 격리 gate로 교체

- 실제 server IP와 CDC port를 사용
- 허용 Connect host에서는 연결 성공
- 비허용 별도 host에서는 동일 endpoint 연결 실패
- firewall rule snapshot 첨부
- UPDATE/DELETE before-image 존재를 assert
- 실패 시 반드시 non-zero exit
- 기본 테스트 계정은 DBA가 아닌 제한 계정으로 구성

## 6.5 최종 pair 전 스위트 재실행

동일한 published full SHA pair로 다음을 한 번에 실행하고 evidence bundle을 남긴다.

- unit test
- normal E2E + diff-check
- S1–S4
- SN1–SN4
- BS1
- owner collision
- cp1–cp5 crash campaign
- partial rollback
- charset negative/parity
- relation DROP/RENAME lag
- HA barrier identity
- partition DDL
- temporal v2 matrix
- real port isolation

## 6.6 종착 처리

- #81에 전제조건·exact pair·전체 결과·최종 판정을 기록
- #81 close
- #74 본문에 최종 pair와 evidence index 반영
- #74 close

#86은 미지원 TZ 타입이 계속 startup fail-fast 대상이라면 이후 별도로 진행해도 된다.

---

# 7. 최종 결론

**NO-GO**

개별 구현은 이전 리뷰를 진지하게 반영했고, relation·charset·snapshot identity 영역에서는 실제 개선이 확인된다. temporal wire v2 설계와 로컬 실측도 방향이 좋다.

그러나 현재 공개된 커넥터에는 temporal P0가 그대로 남아 있고, 최종 커넥터·엔진 커밋은 원격에 존재하지 않는다. 종착 #81도 미수행이며, 포트 격리 증거는 실제 파일럿 방화벽을 검증하지 않고 체크인된 raw evidence도 완료 코멘트의 before-image 주장과 일치하지 않는다.

따라서 **현재 상태에서 사내 파일럿을 시작하면 안 된다.** 위 최소 조치를 완료하고, 원격에 고정된 exact pair로 최종 통합 gate를 다시 통과한 뒤 재판정해야 한다.
