# PR #7788 — hyunikn 리뷰 답변 초안 (v2)

**PR:** [CUBRID/cubrid#7788](https://github.com/CUBRID/cubrid/pull/7788)
**리뷰:** hyunikn, 2026-08-25 (review id 5016005611, top-level review body — 인라인 코멘트 없음)
**방향 (사용자 결정, 2026-08-25):** 키워드는 `PARALLEL_ENABLE` 유지, PL/CSQL **함수 형태는 지원**하겠다고 답변.
**인용 좌표:** PR HEAD `397851bb5` (워크트리 `wf109-reduced-spec`)

## 조사 결과 (답변 근거)

### F1. PL/CSQL 기반 FUNCTION 형태는 존재한다

- 문법: `CREATE FUNCTION ... IS/AS ... LANGUAGE PLCSQL` — `csql_grammar.y:3070` (`PT_SP_FUNCTION`),
  body의 lang은 `:11742` (`SP_LANG_PLCSQL`). SYS_REFCURSOR 반환은 PL/CSQL 함수 전용이라는
  분기(`:3090`)까지 있어 함수 형태가 1급 기능이다.
- PL/CSQL 컴파일러도 함수/프로시저를 구분한다: `ParseTreeConverter.java:2908` `isSpFunc = (ctx.PROCEDURE() == null)`.

### F2. 현재 PR은 PL/CSQL이면 함수/프로시저 구분 없이 전부 막는다

`jsp_cl.cpp:1082-1091` — `PT_NODE_SP_PARALLEL_ENABLE && lang == SP_LANG_PLCSQL`이면
`ER_SP_PARALLEL_ENABLE_NOT_SUPPORTED`(-1379). `sp_type` 판정(`:1110`)보다 앞에 있어 FUNCTION도 걸린다.

### F3. 순수 계산 PL/CSQL 함수는 지금 게이트 아래에서 그대로 병렬 실행 가능하다

- PL/CSQL 컴파일러는 연결이 필요할 때만 `connectionRequired`를 세우고, 그때만 생성 코드 서두에
  `DriverManager.getConnection("jdbc:default:connection::")`을 방출한다 (`JavaCodeWriter.java:110`, `:295`).
- 사칙연산·지역변수만 쓰는 본문은 `connectionRequired=false` — 연결 시도 자체가 없어
  이번 PR의 connect() 거절 게이트와 충돌하지 않는다. **hyunikn이 든 "사칙연산 암호화" 예시는 실제로 성립한다.**
- 실행 배관은 언어 무관: 적격 판정은 선언 비트 단독(`px_sp_eligibility.hpp:38`),
  시그니처 비트 채움은 카탈로그 directive 기반(`jsp_cl.cpp:2211`), PL/CSQL도 결국 생성된 Java로
  같은 ExecuteThread에서 돈다. Java 전용 가정 없음.

### F4. 개방 시 추가로 필요한 것 (변경 범위)

| 항목 | 내용 | 규모 |
|---|---|---|
| DDL 완화 | `jsp_cl.cpp:1082-1091` 검사를 PROCEDURE x PLCSQL만 거절로 축소 (sp_type 판정 뒤로 이동) | ~5줄 |
| msgcat | -1379 문구를 "PL/CSQL 프로시저는 미지원"으로 조정 (en/ko) | 2줄 |
| MessageBuffer 동기화 | `DBMS_OUTPUT`은 연결 없이 세션 공유 `MessageBuffer`에 도달 (`SpLib.java:615` -> `Context.java:171`). lazy init만 `synchronized`이고 `put`/`putLine`/`getLine`은 비동기화 `StringBuilder`/`LinkedList`(`MessageBuffer.java:44-45`) — px 워커 동시 호출 시 레이스. 메서드 동기화 | ~10줄 |
| 검증 | 순수 계산 PL/CSQL 함수의 병렬 채택 + 빌트인/정적 SQL/커서 거절 + DBMS_OUTPUT 동시 호출 + CTP plcsql 회귀 | 테스트 |
| 매뉴얼 | PARALLEL_ENABLE 절에 PL/CSQL 함수 항목 추가 | 문서 |

주의점 (거절 의미론은 Java SP와 동일하게 유지):

- PL/CSQL 빌트인 함수(`SUBSTR`, `CHR`, `TO_CHAR`, `CAST` 등 `SymbolStack.java:215-509` 목록)는
  이름만 빌트인이고 실제로는 `select <func>(...) from dual`을 default connection으로 실행한다
  (`JavaCodeWriter.java:880` -> `SpLib.java:352`). 커서(`ParseTreeConverter.java:1393`),
  시퀀스(`:978`), 정적/동적 SQL, `COMMIT`/`ROLLBACK`(`JavaCodeWriter.java:1524`, `:2607`)도 동일.
  이들은 지금 게이트가 실행 시점에 `ER_SP_PARALLEL_ENABLE_NO_SQL`로 거절한다 — Java SP와 같은 계약.
- `pl_executor.cpp:64`가 PLCSQL이면 `transaction_control`을 항상 true로 보내지만, COMMIT이
  `conn.commit()`으로 컴파일되어 연결 거절에 함께 막히므로 추가 장치 불요.

### F5. (후속 여지) PL/CSQL은 검증된 선언이 가능하다

PL/CSQL은 CREATE 시점에 서버가 컴파일하고(`jsp_cl.cpp:1199` `plcsql_transfer_file`),
컴파일러가 `connectionRequired`를 이미 계산한다. 다만 현재 `CompileInfo` 와이어에는
`sqlDataAccess`만 실리고(`CompileInfo.java:92`) `connectionRequired`는 안 실리며,
`sql_data_access`는 빌트인 호출을 올리지 않아(NO_SQL 유지) 대용이 안 된다.
`compile_response`에 bool 1개를 추가하면(서버-PL서버 간, 동일 설치본) 런타임 에러 대신
**DDL 시점 거절**이 가능 — Java SP는 본문 불투명이라 불가능한, PL/CSQL만의 더 나은 계약.
이건 이번 PR에 얹을지 후속으로 뺄지 선택지.

---

## 답변 초안 (PR 코멘트 1건으로 게시)

---

@hyunikn 리뷰 감사합니다.

**1. 키워드**

`PARALLEL_ENABLE`은 Oracle `CREATE FUNCTION`의 절 이름을 그대로 가져온 것입니다.
CUBRID PL이 `AUTHID`, `DETERMINISTIC`, `DBMS_OUTPUT`을 Oracle 어휘로 맞춰 온 흐름에 얹은
선택이라(`ParseTreeConverter.java`의 DBMS_OUTPUT 주석에도 "to ease migration from Oracle"이라고
적혀 있습니다) 이대로 유지하고 싶습니다. `PARALLELIZABLE`을 쓰는 엔진은 제가 아는 범위에서는
없고, 다른 계열은 PostgreSQL `PARALLEL SAFE/RESTRICTED/UNSAFE`, DB2 `ALLOW/DISALLOW PARALLEL`
정도로 알고 있습니다.

**2. PL/CSQL**

말씀이 맞습니다. 확인해 보니 사칙연산·지역변수만 쓰는 PL/CSQL 함수는 생성 코드에
default connection 취득 자체가 없어서(`connectionRequired`가 설 때만 `JavaCodeWriter`가
`DriverManager.getConnection`을 방출합니다) 지금의 거절 게이트와 충돌하지 않고, 실행 배관도
언어를 가정하지 않아 그대로 병렬 실행이 됩니다. 말씀하신 간단한 암호화 같은 활용이 실제로
성립하므로, **PL/CSQL 함수 형태는 지원하도록 수정하겠습니다** — DDL 검사를 프로시저 형태만
거절하도록 좁히고, 함수는 선언을 허용하겠습니다.

같은 계약은 유지됩니다: PL/CSQL 빌트인 함수(`SUBSTR` 등)는 내부적으로
`select ... from dual`을 default connection으로 실행하고, 커서·시퀀스·정적/동적 SQL·
`COMMIT`/`ROLLBACK`도 모두 그 연결을 지나므로, 이런 본문은 Java SP와 동일하게
`ER_SP_PARALLEL_ENABLE_NO_SQL`로 거절됩니다. 부수 수정으로 `DBMS_OUTPUT`이 쓰는 세션 공유
`MessageBuffer`는 연결 없이 도달 가능해 병렬 워커 동시 호출에 대비한 동기화를 함께 넣겠습니다.

프로시저 형태를 계속 거절하는 이유는 질의 식에 들어갈 수 없어 병렬 경로에 실릴 일이 없고,
CALL 단독 실행이라 선언이 의미를 갖지 않기 때문입니다.

덧붙이면 PL/CSQL은 CREATE 시점에 서버가 컴파일하므로, 컴파일러가 이미 계산하는
`connectionRequired`를 컴파일 응답에 실으면 SQL이 필요한 본문을 실행 시점 에러가 아니라
DDL 시점에 거절하는 것도 가능합니다. Java SP는 본문이 불투명해 선언을 무검증 신뢰할 수밖에
없지만 PL/CSQL은 검증된 선언이 가능한 셈인데, 이건 범위가 조금 있어 이번 PR에 넣을지 후속으로
뺄지 의견 주시면 따르겠습니다.

---

## 체크

- 인용 좌표 전부 PR HEAD `397851bb5`에서 직접 확인.
- 티켓: [PR #7788 리뷰 대응 — PL/CSQL FUNCTION에 PARALLEL_ENABLE 개방 결정·범위 확정](https://github.com/xmilex-git/workspace/issues/127)
- 게시는 사용자 확인 후.
