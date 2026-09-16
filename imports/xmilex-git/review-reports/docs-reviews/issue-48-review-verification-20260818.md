# #48 전수 리뷰(NO-GO) P0 주장 코드 검증 — 2026-08-18

원 리뷰: [issue-48-full-review-nogo-20260818.md](issue-48-full-review-nogo-20260818.md)

리뷰가 "재현 불가"라고 한 최종 기준선 쌍을 로컬에서 직접 검증했다.

- 커넥터: `/home/cubrid/htap-cdc/debezium-connector-cubrid` @ `387df7a` (리뷰 기준선과 동일)
- 엔진: `~/dev/cubrid` @ `2f24ddc0b` "CDC: node facts in the START_SESSION reply (workspace#70)" — **로컬 존재, 리뷰의 검증 한계(§2.2) 해소**

## 판정 요약

| 리뷰 주장 | 판정 |
|---|---|
| P0-1 relation/DDL fail-open | **CONFIRMED — 리뷰보다 심각** |
| P0-2 temporal wire 형식 불일치 | **REFUTED** (단, 계약 명세 부재·dead code 지적은 유효 → P1급) |
| P0-3 TIMESTAMP/DATETIME 의미 융합 | **CONFIRMED** (의도·문서화된 설계지만 가드 없음) |
| P0-4 비 UTF-8 silent corruption | **CONFIRMED** |
| P0-5 snapshot HA node identity 공백 | **CONFIRMED** (의도적 설계, 테스트로 고정됨) |
| P0-6 CDC 서버측 보안 부재 | 사실이나 기지(旣知) — ADR 0011 D11에 한계로 명기됨 |

## P0-1 — CONFIRMED (리뷰보다 심각)

- `LogItemParser.java:86-94` — CDC_RELATION의 empty owner+table을 `""`로 수용 (주석: "class already dropped at extraction time").
- `CubridStreamingChangeEventSource.java:315-319` — empty announce → `classoid → null` 저장 ("known, unroutable").
- `CubridStreamingChangeEventSource.java:322-345` — DML: `state.counter++` 후 `tableId == null || !captured.test(tableId)` 이면 무로그·무메트릭 `continue`. fail-fast는 미고지(`containsKey`) 경우뿐.
- `CubridStreamingChangeEventSource.java:404-412` — DDL halt 판정이 relation 사전 조회 + 스키마 멤버십에 의존 → dropped 테이블의 DROP DDL 자체가 halt를 우회.
- `CubridStreamingChangeEventSource.java:441-450` — 아무것도 버퍼되지 않았으므로 batch 종료 시 anchor 전진, 재시작 복구 불가.
- `CubridRelationDictionaryTest.java:115-125` — `emptyNamesAnnounceMarksTheClassoidUnroutableInsteadOfErroring` 테스트가 이 동작을 **의도로 고정**.

리뷰가 못 본 추가 구멍:

1. empty 가드가 `isEmpty() && isEmpty()` (AND) — half-empty announce(`owner=""`)는 null 마커도 없이 가짜 `TableId(null,"",...)`가 되어 동일한 silent skip.
2. `captured` predicate가 include list가 아니라 **기동 시 bootstrap된 스키마 맵**(`CubridConnectorTask.java:100-108`) — dropped 테이블은 `readUserTableIds()`에 없으므로 이름이 정상 announce돼도 제2의 silent skip 경로.
3. `CubridCdcAuthorization.java:61-77` — DBA 세션이면 미해석 extraction 테이블에 경고조차 없음.

## P0-2 — REFUTED

- 엔진 `log_manager.c:14100-14108` (`cdc_put_value_to_loginfo`)의 ISO `TO_CHAR` format 상수는 **dead code**: `lang_set_flag_from_lang(NULL, false /*has_user_format*/, ...)` (`:14111-14112`) → `date_to_char`의 `no_user_format = !has_user_format` (`string_opfunc.c:16587`) 분기가 format 인자를 무시하고 `db_date_to_string` 계열 기본 형식 사용.
- 기본 형식은 고정 미국식 AM/PM (`db_date.c:3956` `MM/DD/YYYY`, `:3997` `hh:mm:ss AM/PM` — locale 비의존 sprintf).
- 커넥터 `CubridLogValueDecoder.java:108-113`의 AM/PM 패턴, 실녹취 픽스처 `src/test/resources/wire/cdc-session.hexlog`(`06:37:30.277 PM 08/18/2026` 등), corpus 테스트가 전부 일치 — **wire 계약은 실제로 정합**.
- 유효한 잔여 지적: (a) 엔진 dead code ISO 상수와 `CubridLogValueDecoder.java:27`의 ISO 주장 javadoc이 서로·실물과 모순 — 이번 리뷰 오탐의 직접 원인, (b) byte 단위 wire 계약 명세 문서 부재.

## P0-3 — CONFIRMED

- 엔진: TIMESTAMP는 `db_timestamp_decode_ses`(`string_opfunc.c:16824`) → CDC 데몬 전용 분기 `tz_support.c:4685-4688`(`is_cdc_daemon` → `tz_Region_system` = `server_timezone` 파라미터)로 **server_timezone 기준 렌더**, offset 미포함.
- 커넥터: `CubridConnection.java:174-180`이 TIMESTAMP/DATETIME/TZ 4종을 전부 `Types.TIMESTAMP`로 융합, `CubridValueConverters.java:41-52`는 typeName 미참조, `:36`에서 `ZoneOffset.UTC` 스탬프. corpus 테스트 스스로 epoch-0 TIMESTAMP → `09:00:01`(KST 렌더)을 검증 — 그대로 `Z`가 붙으면 9시간 이동.
- worker JVM 타임존이 UTC가 아니면 `Timestamp.toInstant()` 재해석으로 추가 이동. UTC 3축(server/session/JVM) 요구는 `docs/setup-guide.md:136`·`support-scope.md:111-112`에 운영 지침으로만 존재, **기동 가드 없음**.

## P0-4 — CONFIRMED

- `CubridLogValueDecoder.java:129-131`, `OrReader.java:111-114`, `OrWriter.java:72`, `CssConnection.java:59` — 컬럼 값·owner/table 이름·DDL 문·요청측 전부 무조건 UTF-8.
- 엔진측도 확인: VARCHAR/CHAR는 DB codeset raw bytes를 charset 표기 없이 pack (`log_manager.c:14212-14221`), CDC_RELATION에도 charset 메타데이터 없음.
- charset config·기동 가드·문서 요구사항 전무 (`CubridConnectorConfig.java` 공개 필드 4종뿐; `support-scope.md` "알려진 제약 전체 목록" 11종에 charset 항목 없음).
- 추가: snapshot(JDBC 드라이버 decode)과 streaming(무조건 UTF-8)의 decode 규칙 불일치.
- 부수 발견(엔진): CHAR는 `or_pack_string_with_length`로 NUL 미포함 pack, VARCHAR는 NUL 포함 — 비대칭.

## P0-5 — CONFIRMED

- offset `node` 키(`SourceInfo.java:27`, 형식 `<hostname>@<creationMillis>` — `HaNodeGuard.java:43-45`)의 유일한 stamp 지점은 streaming 접속 경로 `CubridStreamingChangeEventSource.java:207-211`.
- snapshot barrier 캡처(`CubridSnapshotChangeEventSource.java:156`)는 state 축만 검사하고 같은 세션의 node facts를 버림; `:171`의 offset은 `sourceNode=null` → `CubridOffsetContext.java:97-99`가 키 자체를 생략.
- `CubridSnapshotChangeEventSource.java:153-155` 주석이 의도로 문서화, `CubridHaHaltGuardTest.java:133`이 "NODE_KEY 없는 offset 통과"를 고정 — interrupted snapshot을 다른 노드에서 재개해도 검출 불가.

## P0-6 — 기지의 한계

코드 신규 검증 없음. ADR 0011 D11이 "강제력은 클라이언트측 수준" 한계를 이미 명기했고 #48 맵 Not-yet-specified에 "서버측 강제 = 엔진팀 별건"으로 기록되어 있다. 리뷰의 기여는 "문서화된 한계"를 "파일럿 필수 조건(포트 격리)+실증 테스트"로 격상하라는 요구.

## 종합

P0 6건 중 5건 실재(1건은 리뷰 추정보다 악화), 1건(P0-2) 오탐. **리뷰의 NO-GO 판정은 타당**하다. 단 P0-3·P0-5·P0-6은 숨은 버그가 아니라 결정·문서화된 한계이므로, 수정의 성격은 "버그 픽스"가 아니라 "계약의 fail-fast 강제"다. 후속 맵: HTAP 제품화 2차 지도 (P0 차단 해소 → 사내 파일럿 게이트).
