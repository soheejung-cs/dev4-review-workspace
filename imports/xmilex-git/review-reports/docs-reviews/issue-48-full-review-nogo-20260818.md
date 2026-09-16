# workspace 이슈 #48 관련 작업 전수 리뷰

- 대상 이슈: [xmilex-git/workspace#48](https://github.com/xmilex-git/workspace/issues/48)
- 제목: **HTAP 제품화 지도: CUBRID 공식 Debezium→Kafka→ClickHouse 지원 + 기술지원 매뉴얼**
- 검토 기준일: **2026-08-18**
- 주요 커넥터 기준선: `xmilex-git/debezium-connector-cubrid` 커밋 `387df7ab1343770bbe1571d760df108ba78be005`
- 관련 범위: `workspace#47`, `#49`~`#73`, CUBRID 엔진 CDC 변경, standalone Debezium CUBRID 커넥터, 스냅샷·스트리밍·HA·DDL·타입·권한·성능·기술지원 문서

---

## 1. 최종 판정

### 1.1 공식 제품 지원 기준

**NO-GO**

이슈 #48이 선언한 목표는 단순 POC가 아니라 다음 상태다.

> CUBRID가 Debezium→Kafka→ClickHouse HTAP 파이프라인을 공식 지원하고, 엔진 보강과 커넥터가 제품 품질로 수용되며, 안전 성능 특성과 기술지원 매뉴얼까지 완성되는 것.

그러나 현재 결과물은 연구·아키텍처·기능 검증 수준에서는 상당히 완성도가 높지만, 고객 데이터를 대상으로 하는 **공식 제품 지원**을 선언하기에는 다음 P0 문제가 남아 있다.

1. DROP/RENAME 및 CDC lag 상황에서 DDL halt가 우회되고 데이터가 조용히 누락될 수 있음
2. 시간 타입 wire 형식과 커넥터 decoder 계약 불일치 가능성
3. CUBRID `TIMESTAMP`와 `DATETIME` 의미가 하나로 합쳐져 시간값이 변형될 수 있음
4. 비 UTF-8 데이터베이스에서 문자열이 조용히 훼손될 수 있음
5. initial/interrupted snapshot의 barrier와 streaming 사이에 HA node identity 공백이 있음
6. CDC wire 경로의 권한 검사가 서버 보안 경계가 아니라 클라이언트측 검사에 머무름

### 1.2 제한된 사내 POC 기준

**CONDITIONAL GO**

다음 조건을 강제로 고정한다면 제한된 POC 또는 내부 파일럿 후보로는 사용할 수 있다.

- 단일 노드 또는 HA failover가 발생하지 않는 환경
- CUBRID 데이터베이스 문자셋 UTF-8
- CUBRID server/session timezone과 Kafka Connect JVM timezone 모두 UTC
- CDC 포트를 localhost 또는 폐쇄된 관리망으로 완전 격리
- captured table에 DROP/RENAME 금지
- include list 변경 금지
- 정확히 고정된 엔진/커넥터 커밋 조합만 사용
- 장애·DDL·HA 발생 시 자동 복구가 아니라 즉시 중단 후 전체 resnapshot
- 고객 공식 지원이나 일반 배포물로 표현하지 않음

### 1.3 Gate 요약

| 검토 범위 | 판정 |
|---|---:|
| 연구·아키텍처 완성도 | GO |
| 제한된 사내 POC | CONDITIONAL GO |
| 고객사 파일럿 | NO-GO |
| CUBRID 공식 1.0 지원 | NO-GO |
| 이슈 #48 현재 `closed/completed` 상태 | 부적절 — reopen 권고 |

---

## 2. 리뷰 범위와 검증 한계

### 2.1 검토한 범위

- `workspace#48` 본문과 종결 코멘트
- 연결된 결정·조사·구현 티켓 `#47`, `#49`~`#73`
- standalone 커넥터의 최종 기준선 `387df7ab`
- rollback, transaction buffer, DDL halt, online/blocking snapshot, HA halt, relation dictionary, owner adoption, pure Java wire client, unsupported type guard
- `docs/support-scope.md`
- `docs/setup-guide.md`
- `docs/snapshot.md`
- `docs/type-support.md`
- 단위 테스트, E2E, fault campaign, 성능 측정에 대한 이슈 기록
- CUBRID CDC 엔진 변경과 커넥터의 프로토콜 계약

### 2.2 검증 한계

이슈 기록상 최종 엔진 기준선으로 언급된 `2f24ddc0b`와 일부 최종 엔진 브랜치는 현재 조회 가능한 공개 기준선에서 완전하게 재현되지 않았다.

따라서 다음은 독립적으로 재실행하지 못했다.

- 최종 엔진 + 최종 순수 Java 커넥터 exact pair 빌드
- 최종 artifact를 사용한 전체 E2E 및 fault campaign
- 최종 artifact의 extraction throughput 재측정
- 최종 protocol wire dump와 엔진 소스의 byte-identical 비교

이 한계는 단순한 리뷰 환경 문제가 아니라, 공식 제품 릴리스에 필요한 **canonical tag, artifact, protocol manifest, evidence bundle이 아직 없다는 증거**이기도 하다.

---

## 3. Gate별 세부 판정

| Gate | 판정 | 평가 |
|---|---:|---|
| committed-only 발행 | GO | COMMIT까지 transaction별 버퍼링하고 ABORT에서 폐기하는 기본 구조는 적절함 |
| savepoint·statement rollback | GO | 기존 `LOG_SYSOP_END(ABORT)`를 재사용한 rewind marker 설계가 우수함 |
| 정상 재시작 replay | GO | anchor와 counter 결정성을 설계와 테스트로 잘 고정함 |
| 동일 노드 online snapshot | GO | barrier 뒤 RR view를 재확립하여 구조적 유실 창을 제거함 |
| blocking snapshot | 조건부 GO | 동작은 성립하지만 retention과 in-flight transaction 상호작용이 남음 |
| DDL fail-closed | NO-GO | DROP/RENAME 및 lag 상황에서 조용히 skip되는 경로가 존재함 |
| HA halt | NO-GO | snapshot barrier와 streaming 사이 node identity 공백이 존재함 |
| 타입 정합성 | NO-GO | 문자셋·시간 형식·TIMESTAMP 의미론에서 P0 문제가 있음 |
| 권한·보안 경계 | NO-GO | CDC wire 경로에 서버측 사용자 인증·인가 강제가 없음 |
| 최종 성능 증빙 | 미확정 | 기록된 주요 extraction 수치가 최종 순수 Java 구현 기준이 아님 |
| 릴리스 재현성 | NO-GO | canonical engine SHA/tag/artifact와 protocol manifest가 없음 |
| 기술지원 runbook | 조건부 NO-GO | include-list 변경·부분 snapshot 절차가 내부적으로 충돌함 |

---

# 4. P0 — 출시를 막아야 하는 문제

## P0-1. DROP/RENAME 및 CDC lag 상황에서 DDL halt가 우회되고 데이터가 조용히 유실될 수 있음

### 4.1 문제 구조

relation dictionary는 안정적인 과거 relation identity가 아니라, CDC가 로그를 추출하는 시점의 현재 class 정보를 사용한다.

테이블이 이미 DROP됐다면 엔진이 owner/table을 빈 값으로 전달할 수 있고, 커넥터는 이를 다음과 같이 처리한다.

- `classoid → null`로 relation dictionary에 저장
- 해당 classoid의 DML은 counter만 증가시키고 발행하지 않음
- 해당 classoid의 DDL은 captured table로 인식하지 못해 halt하지 않음
- batch 종료 시 anchor가 앞으로 전진
- 재시작해도 누락된 구간을 다시 처리하지 않음

### 4.2 재현 가능한 유실 시나리오

1. `dba.t_order`에 UPDATE가 커밋됨
2. 커넥터가 해당 로그를 읽기 전에 `DROP TABLE dba.t_order`가 실행됨
3. relation 이름 조회 시 class가 이미 사라져 빈 owner/table이 전달됨
4. UPDATE 이벤트가 발행되지 않음
5. DROP DDL에서도 halt하지 않음
6. anchor가 DROP 이후로 전진함
7. ClickHouse에는 과거 데이터가 남고 운영자는 오류를 받지 못함

RENAME도 유사하다.

1. old name으로 DML이 기록됨
2. 추출 전에 RENAME 실행
3. relation이 new name으로 조회됨
4. 기존 `table.include.list`와 일치하지 않음
5. RENAME DDL과 관련 DML이 비캡처 대상으로 취급될 수 있음

### 4.3 영향

- silent data loss
- ClickHouse에 stale row 잔존
- DDL halt라는 제품 계약 위반
- 장애가 JMX나 task FAILED로 노출되지 않을 수 있음
- 재시작으로 복구되지 않음

### 4.4 필수 수정

- 빈 relation owner/table을 정상 상태로 취급하지 말고 즉시 non-retriable fail-fast
- 서버측 extraction filter를 통과한 TABLE DDL은 현재 이름의 `captured.test()` 결과와 무관하게 halt
- relation identity를 다음 중 하나로 변경
  - 세션 시작 시 구성한 `owner.table ↔ classoid` 고정 매핑
  - 로그 기록 시점의 owner/table identity
  - stable relation ID와 session target ID 조합
- `DROP`, `RENAME`, `TRUNCATE`, `ALTER`를 현재 카탈로그 조회 결과와 분리해 판정
- anchor가 DDL 이전에 고정되는지 다시 증명

### 4.5 필수 테스트

- DML → DROP, extractor lag
- DML → RENAME, extractor lag
- 커넥터 정지 중 DROP 후 재시작
- 커넥터 정지 중 RENAME 후 재시작
- empty relation announce
- 같은 classoid에 대해 이름 변경
- DDL 직전·직후 crash
- DDL 이후 restart deterministic re-halt

---

## P0-2. 시간 타입 wire 형식과 decoder 계약이 일치하지 않을 가능성

### 4.6 관찰된 불일치

조회 가능한 CUBRID CDC 엔진 구현은 시간 타입을 다음과 같은 ISO/24시간 형식으로 pack하는 것으로 보인다.

- DATE: `YYYY-MM-DD`
- TIME: `HH24:MI:SS`
- TIMESTAMP: `YYYY-MM-DD HH24:MI:SS`
- DATETIME: `YYYY-MM-DD HH24:MI:SS.FF`

반면 최종 커넥터 decoder와 type corpus는 다음 형식을 주요 계약으로 사용한다.

- DATE: `MM/dd/yyyy`
- TIME: `hh:mm:ss AM/PM`
- TIMESTAMP/DATETIME: `hh:mm:ss AM/PM MM/dd/yyyy`
- 일부 TIMESTAMP에만 ISO fallback

따라서 현재 증거만으로는 다음 둘 중 하나다.

1. 조회한 엔진 소스가 canonical이면 DATE/TIME streaming 파싱이 실패할 수 있음
2. 커넥터 fixture가 canonical이면 실제 최종 테스트 엔진과 현재 엔진 기준선이 다름

어느 경우든 공식 지원에 필요한 exact engine–connector wire 계약이 고정되지 않은 상태다.

### 4.7 영향

- DATE/TIME 이벤트에서 connector task FAILED
- 일부 타입만 fallback해 타입별 동작 불일치
- 테스트 fixture가 실제 최종 릴리스 엔진을 대표하지 않을 수 있음
- protocol skew가 silent corruption 또는 parser failure로 나타날 수 있음

### 4.8 필수 수정

- temporal wire 형식을 프로토콜 명세에 byte 단위로 고정
- 엔진 source와 Java decoder가 동일 명세를 참조하도록 구성
- 최종 릴리스 엔진으로 raw wire fixture 재생성
- fixture metadata에 다음을 포함
  - engine commit/tag
  - connector commit/tag
  - build ID
  - DB charset
  - server/session timezone
  - packet SHA-256
- CI에서 C reference harness와 Java parser 결과를 교차 검증
- DATE/TIME에도 명확한 compatibility parser 또는 단일 canonical parser 적용

---

## P0-3. `TIMESTAMP`와 `DATETIME` 의미가 하나로 합쳐져 시간값이 변형될 수 있음

### 4.9 CUBRID 타입 의미

CUBRID에서 두 타입은 의미가 다르다.

- `TIMESTAMP`
  - Unix time 기반 instant 성격
  - session timezone에 따라 표시 시각이 달라질 수 있음
- `DATETIME`
  - timezone 없는 wall-clock 날짜·시간
  - 표시된 local date-time 자체가 값의 핵심

### 4.10 현재 문제

커넥터는 두 타입을 모두 JDBC `TIMESTAMP` 계열로 다루고, 공통적으로 UTC `Z` 문자열로 변환한다.

이 방식은 한 타입에는 맞아도 다른 타입에는 맞지 않는다.

- `TIMESTAMP`는 session timezone을 고려해 원래 instant를 복원해야 함
- `DATETIME`은 임의 timezone을 부여해 instant로 바꾸면 안 됨
- server/session/worker timezone이 다르면 몇 시간 이동할 수 있음

### 4.11 예시

실제 instant가 `1970-01-01T00:00:01Z`이고 CUBRID session timezone이 KST라면 화면에는 `1970-01-01 09:00:01`로 보일 수 있다.

이 표시 숫자에 단순히 `Z`를 붙이면:

- 올바른 값: `00:00:01Z`
- 잘못 변환된 값: `09:00:01Z`

결과적으로 9시간 이동한다.

### 4.12 필수 수정

- catalog `typeName`으로 TIMESTAMP와 DATETIME을 끝까지 분리
- TIMESTAMP
  - server/session timezone을 명시적으로 획득
  - 올바른 `Instant`로 변환
  - Kafka에는 UTC instant로 발행
- DATETIME
  - timezone 없는 `LocalDateTime` 의미 유지
  - sink에서도 instant와 별도 타입/계약으로 처리
- 최소 임시 정책
  - server timezone UTC
  - session timezone UTC
  - Connect JVM timezone UTC
  - 불일치하면 startup fail-fast
- 테스트
  - UTC ↔ Asia/Seoul
  - DST 적용 timezone
  - TIMESTAMP와 DATETIME 동일 표시값 비교
  - snapshot과 streaming의 byte/value parity

---

## P0-4. 비 UTF-8 데이터베이스에서 문자열이 조용히 훼손될 수 있음

### 4.13 문제

CUBRID는 UTF-8 외에도 EUC-KR, ISO-8859-1 등의 데이터베이스 문자셋을 지원한다.

CDC 엔진은 문자열을 raw bytes로 전달하지만, 커넥터는 문자열 byte 배열을 UTF-8로 해석한다.

따라서 EUC-KR 데이터베이스에서 한글이 들어오면 다음이 가능하다.

- replacement character 발생
- 잘못된 문자열 발행
- 예외 없이 silent corruption
- snapshot JDBC 결과와 streaming 결과 불일치

### 4.14 필수 결정

#### 방안 A — 1.0 UTF-8 only

- startup 시 DB charset 조회
- UTF-8이 아니면 기동 거부
- `support-scope.md`와 `setup-guide.md`에 필수 조건 명시
- UTF-8 이외의 charset을 넣은 negative test 추가

#### 방안 B — 다중 charset 지원

- START_SESSION reply 또는 별도 protocol field에 source codeset 포함
- Java client가 해당 Charset으로 decode
- JDBC snapshot charset과 streaming charset parity 확인
- EUC-KR, ISO-8859-1 fixture 및 E2E 추가

### 4.15 권고

1.0에서는 UTF-8 only가 현실적이다. 중요한 것은 묵시적 가정이 아니라 **startup fail-fast로 강제**하는 것이다.

---

## P0-5. snapshot barrier와 streaming 사이에 HA node identity 공백이 있음

### 4.16 문제 시나리오

초기 snapshot은 노드 A에서 barrier LSA를 캡처하고 HA state를 확인한다.

그러나 barrier offset에 source node identity가 기록되지 않거나, streaming 시작 시 처음으로 identity를 stamp하면 다음 시나리오가 가능하다.

1. 노드 A에서 barrier 캡처
2. snapshot 수행 또는 중단
3. VIP/DNS가 노드 B로 전환
4. streaming이 B에서 시작
5. 기존 offset에 node identity가 없어 mismatch를 검출하지 못함
6. A의 barrier LSA를 B의 로그 좌표계에 적용하거나 서로 다른 노드의 snapshot/stream을 혼합

interrupted snapshot에서 기존 barrier를 재사용하는 경우도 동일하다.

### 4.17 영향

- snapshot과 streaming의 source node가 달라질 수 있음
- 다른 노드의 LSA 좌표계를 잘못 적용
- 중복·유실·오염
- HA halt가 존재해도 initial handover 구간은 보호하지 못함

### 4.18 필수 수정

- barrier 캡처 시 다음을 offset에 함께 기록
  - node/instance identity
  - DB creation identity
  - 실제 접속 endpoint
  - HA state
  - protocol version
- interrupted snapshot에서 barrier 재사용 전 identity 비교
- snapshot JDBC connection과 barrier CDC connection이 동일 실제 서버인지 검증
- streaming 시작 시 barrier identity와 live identity 재검증
- mismatch 시 anchor를 전진시키지 않고 non-retriable halt

### 4.19 필수 테스트

- barrier=A, streaming=B
- snapshot 중 failover
- interrupted snapshot 후 다른 master에서 재시작
- VIP는 동일하지만 실제 노드 변경
- backup/restore로 creation time이 동일한 slave
- hostname 변경 없이 backend만 교체되는 구성

---

## P0-6. CDC wire 경로의 권한 검사가 서버 보안 경계가 아님

### 4.20 현재 구조

순수 Java client는 CDC 포트로 직접 CSS protocol을 사용한다.

사용자 ID와 비밀번호를 이용한 SELECT 권한 검사는 커넥터 내부의 JDBC authorization gate에서 수행된다.

그러나 CDC 서버 자체가 동일 identity를 인증하고 extraction target을 권한 기준으로 강제하지 않으면, 별도 raw client가 JDBC gate를 우회해 CDC protocol을 직접 사용할 가능성이 있다.

### 4.21 영향

- per-table SELECT가 실제 서버 보안 경계가 아님
- CDC 포트 접근자가 raw log 또는 다른 테이블 데이터에 접근할 수 있는 위험
- 네트워크 노출 시 데이터 기밀성 문제
- 공식 보안 기능으로 표현할 수 없음

### 4.22 필수 수정

#### 장기 해법

- CDC START_SESSION에 인증 identity 또는 authenticated session token 포함
- 서버가 extraction target별 권한 검사
- REVOKE 정책과 재접속 정책 정의
- TLS 또는 인증된 transport 적용
- protocol replay 방지

#### 임시 제한 지원

서버측 강제가 구현되기 전까지는 최소 다음을 강제해야 한다.

- CDC 포트 localhost bind 또는 전용 관리망 bind
- firewall allowlist
- mTLS proxy 또는 TLS tunnel
- 일반 사용자망 접근 금지
- unauthenticated raw client 접근 차단 테스트
- 문서에 명시:
  - JDBC SELECT gate는 운영 편의 검사이지 서버 보안 경계가 아님
  - CDC 포트 격리가 필수임

### 4.23 판정

사내 폐쇄망 POC에서는 조건부 수용 가능하지만, 일반 고객 환경의 공식 지원 기능으로는 P0다.

---

# 5. P1 — 제품 승인 전에 반드시 해결해야 하는 문제

## P1-1. canonical engine–connector 릴리스 쌍이 없음

### 문제

커넥터는 hard-coded protocol opcode와 packet layout을 사용하며 서버와 lockstep 출하를 전제로 한다.

그러나 현재 다음이 없다.

- canonical engine release tag
- canonical connector release tag
- protocol version
- capability bitmap
- compatibility manifest
- JDBC driver digest
- Connect image digest
- 최종 E2E evidence bundle
- rollback/upgrade 가능한 artifact archive

### 필수 산출물

- `cubrid-<version>-cdc1` tag
- `debezium-connector-cubrid-<version>` tag
- `protocol-version.json`
- engine/connector/JDBC/container SHA-256
- SBOM
- release notes
- compatibility matrix
- immutable CI evidence artifact

---

## P1-2. `table.include.list` 변경 runbook이 내부적으로 충돌함

### 문제

문서에는 다음 두 주장이 공존한다.

- include list 변경은 offset counter 좌표를 바꾸므로 전체 resnapshot 필요
- 새 테이블 추가 시 해당 테이블만 blocking snapshot 가능

현재 `_version`이 필터링된 stream의 deterministic counter에 의존한다면, include-list 변경은 기존 테이블의 이후 counter에도 영향을 줄 수 있다.

따라서 부분 추가를 지원하려면 별도 증명이 필요하다.

### 권고

1.0에서는 다음 하나만 허용하는 것이 안전하다.

1. source connector 정지
2. sink write 중지
3. source offset 삭제
4. sink shadow table 준비
5. 전체 initial snapshot
6. diff-check
7. atomic swap
8. streaming 재개

부분 테이블 추가는 post-1.0으로 미루고 다음을 별도 설계한다.

- filter-independent source version
- table별 epoch
- add/remove target fault campaign
- blocking snapshot과 live stream version ordering

---

## P1-3. target resolution과 PK에 대한 startup fail-fast 부족

### 문제

문서는 PK를 요구하지만 코드가 다음을 모두 fatal로 검사하는지는 불분명하거나 부족하다.

- include-list의 모든 항목이 실제 테이블로 resolve되는지
- 대상 테이블 수가 0인지
- PK가 존재하는지
- PK 컬럼이 nullable인지
- PK 타입이 지원되는지
- owner/table 대소문자 정규화 후 중복되는지
- sink ORDER BY key 생성이 가능한지

### 수정 요구

startup validation에서 다음을 모두 검사한다.

- 모든 include entry가 정확히 1개 relation으로 resolve
- resolve 실패는 warning이 아니라 fatal
- captured table 0개는 fatal
- PK 없음은 fatal
- nullable PK는 fatal 또는 명시적 미지원
- unsupported PK type은 fatal
- duplicate normalized target은 fatal
- sink mapping 누락은 배포 검증 단계에서 fatal

---

## P1-4. 미래 enum과 protocol 변화 일부가 fail-open

### 문제

미지의 top-level item이나 packet 구조가 parser failure로 끝나는 것은 적절하다.

그러나 일부 내부 enum은 다음처럼 처리될 수 있다.

- unknown DML type → warning 후 skip
- unknown DCL type → COMMIT이 아니므로 abort처럼 폐기
- unknown DDL type → halt

unknown DML/DCL을 skip 또는 abort 취급하면 protocol skew가 silent data loss로 변한다.

### 수정 요구

- unknown DML type → protocol mismatch halt
- unknown DCL type → protocol mismatch halt
- START_SESSION에 version/capability 협상
- enum position이 아니라 고정 protocol value 사용
- 구버전/신버전 mismatch negative test
- expected/actual opcode와 packet length를 오류 메시지에 포함

---

## P1-5. 성능 수치가 최종 순수 Java 구현을 대표하지 않음

### 기록된 수치

이슈 기록상 대표 수치는 대략 다음과 같다.

- supplemental logging overhead
  - bulk 약 +2.4%
  - single 약 +0.5%
- extraction 정상 약 26~28k events/s
- sustained 약 3k events/s에서 lag ≤ 약 1.2s
- snapshot 약 72.6k rows/s 수준의 기록

### 문제

주요 extraction 수치는 최종 pure Java wire client, relation dictionary, owner-aware schema, unsupported type guard가 모두 적용된 최종 artifact 기준이 아니다.

따라서 다음처럼 해석해야 한다.

- supplemental logging overhead: 엔진 쓰기 경로 참고자료
- extraction throughput: 최종 제품 수치로 사용 불가
- snapshot throughput: 최종 제품 수치로 사용 불가
- memory/GC: 최종 측정 없음
- reconnect catch-up: 최종 artifact 측정 필요

### 재측정 항목

- pure Java extraction peak events/s
- sustained workload lag
- catch-up rate
- heap usage
- transaction size별 memory
- GC pause
- long transaction
- update-heavy workload
- snapshot rows/s
- ClickHouse sink backlog
- connector restart recovery
- DDL/HA halt 직전·직후 overhead

---

## P1-6. CI가 제품 release gate 수준이 아님

### 현재 한계

- 일부 style/format/revapi/enforcer gate skip
- live CUBRID integration이 매 commit 공식 gate인지 불명확
- 최종 exact pair의 immutable fault evidence 없음
- release packaging 설치 테스트 부족
- 장시간 soak test 없음
- charset/timezone/HA/DDL lag matrix 없음

### 필요한 CI 계층

1. Unit
2. Wire fixture
3. C reference parity
4. Live engine integration
5. Kafka Connect integration
6. ClickHouse differential
7. Crash campaign
8. Snapshot fault campaign
9. DDL lag campaign
10. HA handover campaign
11. Charset/timezone matrix
12. Version mismatch negative tests
13. Packaging install smoke
14. Nightly soak
15. Release evidence bundle

---

## P1-7. transaction buffer 기본값과 abandon 정책이 제품 기본값으로 위험함

### 문제

현재 기본값이 무제한이면 long transaction 하나가 heap과 anchor를 장시간 붙잡을 수 있다.

반대로 threshold/retention을 켜면 초과 transaction을 abandon하고 다운스트림 유실을 수용한다.

또한 crash 시 in-memory abandoned transaction skip set이 사라져 suffix 일부가 재등장할 수 있는 위험도 문서화되어 있다.

### 권고

- `transaction.max.bytes` 추가
- disk spill 또는 source backpressure 설계
- strict mode
  - 임계치 초과 시 abandon하지 않고 task FAILED
  - anchor 고정
  - 운영자 resnapshot
- 유실형 abandon은 `unsafe` 옵션으로 분리
- 공식 고객 기본값에서는 unsafe mode 금지
- memory admission 계산 제공
- max transaction workload guide 제공

---

## P1-8. blocking snapshot과 retention timer 충돌

### 문제

blocking snapshot은 streaming을 batch boundary에서 pause하지만 기존 in-flight transaction buffer는 메모리에 남을 수 있다.

snapshot이 오래 걸리면 wall-clock 기준 retention age가 계속 증가한다.

재개 후 retention 검사에서 snapshot 때문에 오래 기다린 transaction이 abandon될 수 있다.

### 수정 선택지

- intentional pause 시간을 retention age에서 제외
- in-flight transaction이 0이 될 때까지 blocking snapshot 승인 지연
- retention-enabled 구성에서 blocking snapshot 금지
- pause 진입 시 transaction age clock freeze
- 테스트
  - open transaction + 장시간 blocking snapshot
  - snapshot 도중 connector restart
  - retention threshold 직전 pause

---

## P1-9. 공식 배포물 형태가 아님

### 현재 상태

- plain JAR 중심
- runtime companion dependency를 운영자가 수동 배치
- standalone 개발 저장소
- incubating 표현
- Debezium Alpha 기준선
- 공식 image/plugin archive와 checksum 부재
- CUBRID org 및 공식 manual 편입 미완료

### 제품화를 위한 요구

- release-grade Debezium 기준선
- plugin archive 또는 공식 Connect image
- dependency lock
- JDBC driver 배포·라이선스 결정
- SBOM
- checksum/signature
- upgrade/downgrade 지침
- CUBRID org 이관
- 공식 CUBRID manual RST 편입
- supported version matrix

---

# 6. P2 — 후속 품질 개선

| 항목 | 개선 내용 |
|---|---|
| packet bounds 검사 | `OrReader`와 packet parser에 길이·범위·overflow 검사를 명시적으로 추가 |
| identifier 지원 | ASCII unquoted identifier only인지 공식 범위를 정하고, quoted/non-ASCII 지원 여부 결정 |
| snapshot column quoting | SELECT column name을 DB identifier 규칙에 맞게 quote |
| protocol diagnostics | 오류에 protocol version, opcode, packet length, engine build ID 포함 |
| stale comments | 순수 Java 전환 후 남아 있는 JNA 관련 Javadoc·주석 제거 |
| 문서 표현 | 구버전 서버가 항상 명시적 version error를 준다는 과도한 표현 수정 |
| metrics persistence | DDL/HA/abandon 마지막 원인을 외부 로그·알림 시스템에 영속화 |
| evidence 보관 | 성능 raw data, 환경 manifest, 실행 스크립트, 로그를 immutable artifact로 저장 |
| config validation | connector validation API 단계에서 include-list, charset, timezone, protocol 호환성 사전 검사 |
| fuzz test | wire parser와 log item parser에 malformed packet fuzzing 추가 |
| resource lifecycle | connect/finalize/read timeout, half-open socket, cancellation 경로 장기 테스트 |
| observability | source LSA, anchor LSA, lag, buffer bytes, last successful commit timestamp 제공 |

---

# 7. 잘한 부분

## 7.1 부분 rollback 설계

새 WAL record를 추가하기보다 기존 `LOG_SYSOP_END(ABORT)`를 해석해 다음을 모두 포착하려 한 결정은 좋다.

- user savepoint rollback
- statement rollback
- internal statement top operation abort

DML에 orderable record LSA key를 싣고 `ROLLBACK_TO`에서 transaction buffer를 rewind하는 구조도 명확하다.

이 선택은 WAL format 변경을 피하면서 phantom event 문제를 해결하려는 합리적인 설계다.

## 7.2 anchor 불변식

COMMIT transaction을 모든 이벤트 enqueue 전까지 in-flight set에서 제거하지 않아, partially acknowledged commit도 전체 재생 가능한 anchor를 유지하도록 한 수정은 중요하다.

다음 불변식을 테스트로 고정한 점이 좋다.

> transaction의 마지막 change record가 enqueue되기 전에는 어떤 record도 해당 transaction의 first DML 이후 anchor를 갖지 않는다.

## 7.3 online snapshot loss window 수정

metadata query가 barrier 이전 REPEATABLE READ view를 열 수 있다는 문제를 단순 추론에 그치지 않고 negative control로 재현한 점이 좋다.

barrier 캡처 직후 기존 transaction을 commit해 pre-barrier view를 폐기하고, 실제 scan view를 barrier 이후에 열도록 한 수정은 설득력이 있다.

## 7.4 relation announcement counter 제외

relation dictionary는 재연결마다 다시 전달되는 session event이므로 deterministic event counter에 포함하면 안 된다.

이를 TIMER와 유사하게 counter에서 제외한 판단은 RMT `_version` replay convergence 관점에서 정확하다.

## 7.5 미지원 타입 fail-fast

미지원 타입을 조용히 통과시키지 않고 startup allow-list로 막은 점은 제품화에 필요한 접근이다.

특히 다음 silent 문제를 인식하고 차단하려 한 점이 좋다.

- collection/JSON의 NULL화
- MONETARY의 잘못된 double 해석
- BIT/TZ 계열 파싱 불일치
- 미래 unknown type

## 7.6 pure Java wire client

JNA와 native library dependency를 제거한 것은 큰 개선이다.

- Connect worker JVM native crash 위험 감소
- CUBRID 설치 mount 불필요
- `LD_LIBRARY_PATH` 제거
- wire fixture 기반 테스트 가능
- protocol code를 Java에서 직접 검증 가능

## 7.7 알려진 위험을 문서에 노출

transaction abandon, HA resnapshot, DDL halt, lockstep 출하, unsupported type 등의 제약을 숨기지 않고 문서화한 점은 좋다.

다만 공식 지원을 선언하려면 문서화에 그치지 않고 startup guard와 server-side enforcement가 추가되어야 한다.

---

# 8. 해결 작업 권장 순서

## Phase 1 — 즉시 P0 수정

### 1순위: relation/DDL fail-closed

- empty relation fatal
- DROP/RENAME lag 재현
- TABLE DDL은 현재 카탈로그 이름과 무관하게 halt
- stable relation identity 도입
- anchor re-halt 검증

### 2순위: temporal protocol 확정

- 실제 최종 엔진 wire dump 재채취
- DATE/TIME/TIMESTAMP/DATETIME byte contract 확정
- TIMESTAMP와 DATETIME 분리
- UTC/KST matrix

### 3순위: charset guard

- DB charset 조회
- 1.0 UTF-8 only 결정
- non-UTF-8 startup fail-fast
- snapshot/streaming charset parity test

### 4순위: snapshot HA identity

- barrier offset에 node identity 기록
- interrupted snapshot identity 검증
- barrier→stream handover mismatch test

### 5순위: CDC 포트 보안

- 서버측 인증·인가 설계
- 또는 최소한 port isolation 강제 및 raw client denial 환경 구성

## Phase 2 — 릴리스 재현성

- canonical engine branch/tag
- canonical connector tag
- protocol version
- capability bitmap
- JDBC/image digest
- full evidence bundle

## Phase 3 — 운영 안정성

- target/PK startup validation
- include-list 변경 정책 단일화
- strict transaction buffer mode
- blocking snapshot + retention 해결
- 최종 artifact 성능 재측정

## Phase 4 — 공식 배포

- CUBRID org 이관
- plugin/image packaging
- SBOM/checksum
- 공식 manual 편입
- upgrade/downgrade 및 compatibility matrix

---

# 9. 다시 닫을 수 있는 완료 조건

## 9.1 정확한 릴리스 기준선

- [ ] engine release tag 존재
- [ ] connector release tag 존재
- [ ] protocol version/capability 명세 존재
- [ ] JDBC SHA 고정
- [ ] Connect image digest 고정
- [ ] ClickHouse sink 구성 버전 고정
- [ ] SBOM 및 checksum 존재

## 9.2 silent corruption 방지

- [ ] empty relation은 halt
- [ ] unknown DML/DCL은 halt
- [ ] 미지원 charset은 halt
- [ ] timezone 계약 불일치는 halt
- [ ] unresolved target은 halt
- [ ] no-PK table은 halt
- [ ] unsupported type은 halt
- [ ] engine/connector mismatch는 startup 단계에서 halt

## 9.3 필수 fault test

- [ ] DML → DROP with lag
- [ ] DML → RENAME with lag
- [ ] connector down 동안 DROP
- [ ] connector down 동안 RENAME
- [ ] barrier node A → streaming node B
- [ ] snapshot 중 failover
- [ ] interrupted snapshot 후 failover
- [ ] UTF-8/EUC-KR/ISO-8859-1
- [ ] UTC/Asia-Seoul timezone matrix
- [ ] TIMESTAMP/DATETIME 차등
- [ ] blocking snapshot + active transaction + retention
- [ ] protocol version mismatch
- [ ] unauthenticated raw CDC client denial
- [ ] include-list add/remove crash campaign

## 9.4 최종 artifact 성능

- [ ] pure Java extraction peak
- [ ] sustained lag
- [ ] catch-up rate
- [ ] heap 및 GC
- [ ] long transaction memory
- [ ] snapshot throughput
- [ ] update-heavy workload
- [ ] restart recovery
- [ ] supplemental logging overhead
- [ ] ClickHouse physical amplification

## 9.5 배포·운영

- [ ] 공식 plugin archive 또는 Connect image
- [ ] installation smoke
- [ ] upgrade/rollback guide
- [ ] port security guide
- [ ] 단일하고 모순 없는 resnapshot runbook
- [ ] 공식 CUBRID manual 편입
- [ ] compatibility matrix
- [ ] immutable test evidence bundle

---

# 10. 최종 종합 의견

이 프로젝트는 실패작이 아니다.

오히려 다음 영역은 높은 수준으로 정리되어 있다.

- committed-only transaction buffering
- partial rollback rewind
- replay anchor invariant
- online snapshot consistency
- blocking snapshot wiring
- DDL/HA fail-fast의 기본 구조
- owner-aware schema discovery
- relation dictionary의 replay counter 처리
- unsupported type startup guard
- pure Java CDC protocol client
- 기술지원 관점의 제약 문서화

다만 현재 `closed/completed` 표시는 **연구·아키텍처·POC 지도의 완료**와 **고객 대상 공식 제품 지원 완료**를 동일하게 취급한 것이다.

특히 아래 문제는 고객 데이터의 정합성 또는 기밀성에 직접 영향을 준다.

- relation/DDL fail-open
- temporal wire/semantic mismatch
- charset 무검증
- snapshot HA identity 공백
- CDC port server-side security 부재

따라서 최종 결론은 다음과 같다.

> **이슈 #48은 아키텍처·POC 지도 완료로는 GO지만, 본문에 선언한 공식 제품화 destination 기준으로는 NO-GO다. 이슈를 reopen하고 P0 문제를 해결한 뒤, 정확히 고정된 최종 engine–connector release artifact로 전체 fault·정합성·성능 검증을 다시 수행해야 한다.**

---

## 11. 권장 reopen 제목

**[REOPEN/P0] HTAP 공식 제품화 잔여 차단 문제 — DDL identity·temporal/charset·snapshot HA·CDC security·release reproducibility**

## 12. 권장 후속 티켓 묶음

1. `[P0] CDC relation identity fail-closed — DROP/RENAME lag silent loss 제거`
2. `[P0] Temporal wire contract — DATE/TIME 형식 고정 + TIMESTAMP/DATETIME 의미 분리`
3. `[P0] Source charset contract — UTF-8 startup guard 또는 codeset negotiation`
4. `[P0] Snapshot barrier node identity — initial/interrupted snapshot HA handover 차단`
5. `[P0] CDC session server-side authentication/authorization 또는 강제 network isolation`
6. `[P1] Protocol version/capability negotiation + canonical release manifest`
7. `[P1] Include-list/PK/target startup validation`
8. `[P1] Final pure-Java artifact full E2E/fault/performance evidence`
9. `[P1] Transaction buffer strict mode + blocking snapshot retention 정합성`
10. `[P1] Official packaging, SBOM, CUBRID org transfer, manual integration`
