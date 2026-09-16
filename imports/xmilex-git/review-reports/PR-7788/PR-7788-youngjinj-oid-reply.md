# PR #7788 — youngjinj 리뷰(r3923087647) 검토·답변 초안

**코멘트:** PlcParser.g4:71 (parallel_enable_spec), 2026-09-03. `object` 인자를 받는 Java SP가
PARALLEL_ENABLE 선언만 붙이면 "Stored procedure execute error: Unexpected internal error".
**PR HEAD:** c67e642a9 (워크트리 `wf109-reduced-spec`, 브랜치 CBRD-27299)

## 원인

- `OidValue.createInstance()`(PL 서버)는 OID 인자를 `CUBRIDServerSideOID(con, oid)`로 감싸기 위해
  `DriverManager.getConnection("jdbc:default:connection:")`을 호출한다.
- 선언 SP는 이 PR의 `CUBRIDServerSideDriver.connect()` 게이트에서 SQLException으로 거절되고,
  `createInstance()`는 그 예외를 삼켜(`oidObject = null`) 넘어간다.
  - String 파라미터(`toString()`): `oidObject.getOidString()` NPE → ExecuteThread가 메시지 없는 Throwable로
    "Unexpected internal error" 회신 (리뷰어 재현).
  - CUBRIDOID 파라미터(`toOid()`): null이 그대로 전달 — `oid_is_null`이 'NULL'을 반환하는 **조용한 오답**.
- 이력: sticky 거절이 있던 동안(397851bb5)은 이 지점이 -1379로 실패해 design/test/manual/JIRA 본문에
  "OID 인자/반환형 SP는 선언 시 거절된다(자연 커버리지)"로 기록됐다. sticky 제거(40f5ac1bc) 이후
  삼켜진 예외가 NPE/null로 바뀐 것을 재검증하지 못했다. 즉 40f5ac1bc의 회귀이며, 리뷰어 지적이 맞다.

## 결정

- D1. OID는 값(page|slot|vol)일 뿐이므로 선언 SP에서 **전달·반환·문자열 변환은 허용**하고, 서버 왕복이 필요한
  **역참조(getValues/setValues/getTableName/isInstance/remove/잠금/집합·시퀀스 조작)만 거절**한다.
  - 대안(object 파라미터를 DDL 시점 거절)은 리뷰어 예시 같은 정당한 사용을 막고 PL/CSQL과도 비대칭이라 기각.
  - 비용: `CUBRIDServerSideOID`에 연결 없는 생성자 + `requestHandler()` 가드 12곳. `getConnection()`은 null.
  - 탈출구: 거절로 되돌리려면 `OidValue.createInstance()`의 분기 하나만 제거.
- D2. 스레드 플래그 검사(driver/DBMS_OUTPUT/OidValue 3곳 중복)를 `ExecuteThread.isServerSideSqlForbiddenOnCurrentThread()`로 모음.

## 변경 파일

- `pl_engine/.../jsp/value/OidValue.java` — 선언 SP면 연결 없는 OID 생성 (connect() 미호출)
- `pl_engine/.../jsp/jdbc/CUBRIDServerSideOID.java` — `CUBRIDServerSideOID(SOID)` 생성자, `requestHandler()` 가드
- `pl_engine/.../jsp/ExecuteThread.java` — 정적 헬퍼
- `pl_engine/.../jsp/jdbc/CUBRIDServerSideDriver.java`, `plcsql/builtin/DBMS_OUTPUT.java` — 헬퍼 사용
- JIRA 첨부(`.git_ignored_dir/jira/CBRD-27299/{design,test,manual}.md`, 본문 md) — OID 항목 정정, test T11 추가

## 검증

optdebug 설치본(`~/optdebug/CUBRID-11.5.develop`, PR HEAD c67e642a9), db `pr7788rv`, port 1700. 수정 전은 HEAD jar, 수정 후는 `just incr optdebug`로 재설치한 jar(3f40a9904).

| 질의 (선언 SP) | 수정 전 | 수정 후 | 미선언 SP |
|---|---|---|---|
| `oid_to_str(t_oid)` (String 인자) | Unexpected internal error | `'@577\|1\|1'` | `'@577\|1\|1'` |
| `oid_is_null(t_oid)` (CUBRIDOID 인자) | `'NULL'` (오답) | `'NOT NULL'` | `'NOT NULL'` |
| `oid_str2(t_oid)` (getOidString) | Unexpected internal error | `'@577\|1\|1'` | 동일 |
| `oid_table(t_oid)` (getTableName) | Unexpected internal error | ERROR: cannot execute SQL on the server-side connection: the stored procedure is declared PARALLEL_ENABLE | `'dba.t_oid'` |
| `oid_pass(t_oid) = t_oid` (OID 반환) | NULL | 1 | 1 |
| `count(*) where oid_is_null(t_oid)='NOT NULL'` | 0 | 3000 | 3000 |

수정 전 PL 로그: `NullPointerException at OidValue.toString(OidValue.java:88) <- StoredProcedure.checkArgs`. 미선언 SP 출력은 수정 전후 동일. 서버 생존.
부수 발견: `just incr`가 설치본 conf의 `cubrid_port_id`를 1523으로 되돌림(캠페인 conf 복사) — 재빌드 후 1700 재설정 필요.

---

## 답변 초안 (PR 코멘트)

@youngjinj 재현 감사합니다. 말씀대로 의도한 동작이 아니고, 원인은 OID 인자 마샬링입니다.

PL 서버가 `object` 인자를 `CUBRIDOID`로 감쌀 때(`OidValue.createInstance()`) 내부적으로 `jdbc:default:connection`을 여는데, PARALLEL_ENABLE SP는 이 PR의 `connect()` 거절에 걸리고 그 SQLException을 `createInstance()`가 삼켜서 OID가 null로 남았습니다. String 파라미터면 `getOidString()`에서 NPE가 나서 "Unexpected internal error"로 보이고, `CUBRIDOID` 파라미터면 null이 그대로 전달됩니다(`oid_is_null`이 'NULL'을 반환). sticky 거절이 있던 시점에는 이 자리가 -1379로 실패해서 문서에 "OID 인자 SP는 선언 시 거절"로 적어 두었는데, sticky를 걷어낸 40f5ac1bc 이후 재검증을 빠뜨렸습니다.

OID 값 자체는 데이터라 서버 연결이 필요 없으므로, 선언 SP에서는 연결 없는 `CUBRIDServerSideOID`를 만들어 인자 전달·반환·`getOidString()`은 그대로 동작하게 하고, 서버 왕복이 필요한 역참조(`getValues`/`getTableName`/`setValues`/`remove`/잠금·집합 조작)만 `connect()`와 같은 SQLException으로 거절하도록 고쳤습니다 (3f40a9904). 미선언 SP 경로는 그대로입니다.

검증: 올려 주신 재현이 `'@p|s|v'`를 반환하고, `CUBRIDOID` 인자·반환, `getOidString()`도 미선언 SP와 같은 결과입니다. `getTableName()`을 호출하는 선언 SP는 "cannot execute SQL on the server-side connection: the stored procedure is declared PARALLEL_ENABLE"로 실패합니다. design/test/매뉴얼 문안의 OID 항목도 같이 정정했습니다.
