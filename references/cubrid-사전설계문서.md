# CUBRID 재설계 종합 사전 설계 문서 (통합본)

> 기준: CUBRID 오픈소스 저장소(github.com/CUBRID/cubrid, develop 브랜치, v11.5.x 계열) 실제 소스 트리 및 공식 매뉴얼.
> 목적: "큐브리드를 처음부터 다시 설계한다"는 전제 하에, 설계 전에 확정해야 할 배경지식·컴포넌트 분할·워크로드/동시성 설계·검증 체계를 한 권으로 정의한다.
> 비교 대상: PostgreSQL(이하 PG), Oracle Database(이하 Oracle). 소스에서 직접 확인한 사실은 본문에 (src확인)으로 표기.

---

## 0. 읽는 법과 전체 목차

이 문서는 두 부로 구성된다. **제1부(1~8장)는 "무엇으로 이루어져 있고 무엇을 바꾸면 안 되는가"**, **제2부(9~15장)는 "그 부품들이 부하와 동시성 아래서 어떻게 행동해야 하고 무엇으로 증명하는가"**를 다룬다. 처음 읽는 독자는 1장(불변 조건) → 2장(분할 지도) → 13장(동시성 도메인) → 15장(한 장 요약) 순서의 발췌 독해를 권한다.

| 장 | 제목 | 한 줄 요약 |
|---|---|---|
| **제1부** | **아키텍처 기초** | 컴포넌트 분할과 불변 조건 |
| 1 | 아키텍처 상수 5가지 | 3-tier·클라이언트 컴파일·3-way 빌드·MVCC+Vacuum·ARIES — 바꾸면 다른 DB |
| 2 | 최상위 컴포넌트 분할 | 대영역 [A]~[I] 정의와 프로세스 배치 |
| 3 | 컴포넌트별 상세 | 영역별 책임·핵심 소스·자료구조·설계 확정 사항 |
| 4 | 의존 관계 지도 | 공식 사이클 4개와 중앙 허브 |
| 5 | 쿼리 한 개의 일생 | SQL→결과 12단계 데이터 흐름 |
| 6 | PG/Oracle 비교 | 프로세스·SQL 위치·MVCC·WAL·메모리·HA 6개 축 |
| 7 | 성능 임계도 분류 | Tier-0/1/2 핫패스 지도 |
| 8 | ADR 체크리스트 | 재설계 결정 항목 10개와 동결 권장 목록 |
| **제2부** | **워크로드·동시성·검증** | 부하 아래의 행동 설계와 증명 |
| 9 | 워크로드 축 정의 | OLTP/OLAP의 물리적 본질과 CUBRID의 포지션 |
| 10 | 모듈×워크로드 매트릭스 | 어느 모듈이 어느 쪽 성능을 결정하는가 |
| 11 | OLTP 설계 원칙 | 커밋 경로·락·스냅샷·플랜 재사용·핫스팟·왕복 |
| 12 | OLAP 설계 원칙 | 병렬(px)·스캔·스필·조인·통계·벡터화 |
| 13 | 동시성 도메인 지도 | 격리 vs 공유(등급 S/A/B/C), 경계 관문, px 협조 공유, 간섭 X1~X9 |
| 14 | 유저 시나리오와 검증 | 페르소나 W1~W7 × 테스트 × 합격 기준, 마이크로벤치, 회귀 게이트 |
| 15 | 종합 한 장 요약 | 워크로드×도메인×검증 접합과 설계 진행 순서 |
| 16 | 참고 원천 | 소스·매뉴얼·검증 파일 목록 |

7장(Tier 분류)과 10장(OLTP/OLAP 매트릭스)은 같은 모듈을 다른 축으로 자른 상보 관계다: 7장은 "얼마나 뜨거운가", 10장은 "어느 워크로드에서 뜨거운가"를 답한다.

---

# 제1부 — 아키텍처 기초: 컴포넌트 분할과 불변 조건

---

## 1. 설계 전 확정해야 할 아키텍처 상수 (The Five Invariants)

### 1.1 3-Tier 프로세스 모델: Broker가 1급 시민이다

CUBRID는 처음부터 **드라이버 ↔ 브로커(미들웨어) ↔ DB 서버**의 3계층으로 설계되었다. PG는 클라이언트가 서버(backend)에 직결되고, 커넥션 풀링(pgBouncer)은 외부 3rd-party다. Oracle은 Listener + Shared Server(Dispatcher) 구성이 이와 유사하지만 선택 사항이다. CUBRID에서는 브로커가 **기본이자 필수 경로**다.

```
[App + Driver(JDBC/CCI/ODBC...)]
        │  (브로커 포트, CAS 프로토콜)
        ▼
[cub_broker] ──(shared memory)── [cub_cas × N]   ← 브로커 호스트 (여러 대 가능)
                                     │  (CSS 프로토콜, TCP)
                                     ▼
                    [cub_master] ──▶ [cub_server × DB개수]   ← 서버 호스트
                                          │
                                          ▼
                                  [볼륨 / WAL 로그]
```

- `cub_broker`: 포트 리스닝 + 접속을 CAS에 분배하는 단일 프로세스.
- `cub_cas`: 클라이언트 세션을 실제로 처리하는 워커 **프로세스**(스레드 아님). 한 CAS는 한 시점에 한 클라이언트만 담당 → 크래시 격리.
- `cub_master`: 서버 호스트의 접속 중계자 + HA 하트비트 주체. (PG의 postmaster + Oracle의 listener 역할 혼합)
- `cub_server`: DB 하나당 1개의 **멀티스레드** 서버 프로세스. 스레드는 "브로커 단위"가 아니라 "요청 단위"로 배정된다.

### 1.2 클라이언트 사이드 컴파일 (가장 중요한 특이점)

**파서와 옵티마이저는 서버가 아니라 클라이언트 라이브러리(= CAS, CSQL) 안에서 실행된다.**
소스 전체에 `#if !defined(SERVER_MODE)` 가드로 강제되어 있다.

- SQL → 파스트리(PT_NODE) → 플랜(XASL_NODE) 변환까지 전부 CAS/CSQL 프로세스에서 수행.
- 완성된 XASL을 **바이트 스트림으로 직렬화**해 서버로 전송(`xasl_to_stream.c`), 서버는 역직렬화만 해서 실행(`stream_to_xasl.c`).
- 효과: 파싱/플래닝 CPU 비용이 브로커 호스트로 오프로드되어 서버는 실행·저장에 집중. 브로커 호스트를 수평 확장하면 컴파일 처리량도 확장.
- 대가: XASL 직렬화 포맷이 클라이언트/서버 간 **정확히 일치**해야 함(버전 불일치 = 크래시). 통계정보는 서버에서 클라이언트로 가져와야 플래닝 가능.
- PG/Oracle은 정반대: 파스·플랜·실행이 전부 서버 프로세스 내부에서 일어난다.

### 1.3 One Source, Three Binaries (모드 분기)

같은 소스가 전처리기 가드로 3개 산출물이 된다. 재설계 시에도 모듈 경계를 이 3-way 빌드가 관통한다는 점을 전제해야 한다.

| 가드 | 산출물 | 용도 |
|---|---|---|
| `SERVER_MODE` | `cub_server` | 서버 프로세스 (실행기+트랜잭션+스토리지) |
| `CS_MODE` | `cubridcs` 라이브러리 | 클라이언트측 (파서+옵티마이저+객체계층+네트워크). CAS/CSQL이 링크 |
| `SA_MODE` | `cubridsa` 라이브러리 | Standalone: 클라이언트+서버를 한 프로세스에 (유틸리티, `csql -S`) |

파일 명명 규칙도 이를 따른다: `*_cl.c` = 클라이언트측, `*_sr.c` = 서버측.

### 1.4 MVCC + 전용 Vacuum (PG 계열이되, 로그 구동형)

- 각 행 버전이 `insert MVCCID` / `delete MVCCID`(64-bit 단조증가, 재사용 없음)를 가진다. 가시성 판정은 `mvcc_satisfies_snapshot()` — 스냅샷 = "최저 활성 MVCCID + 활성 트랜잭션 비트배열".
- 구버전은 힙에 남고(PG처럼), **Vacuum 데몬**이 회수한다. 단 PG의 autovacuum이 힙을 스캔하는 것과 달리 CUBRID Vacuum은 **WAL 로그 레코드를 따라가며** 회수 대상을 찾는 로그 구동형이다. (소스 위치가 `src/query/vacuum.c`인 점 주의 — transaction/이 아님)
- Oracle은 Undo 세그먼트 기반 읽기일관성(CR 블록 재구성)이라 Vacuum 자체가 없다. 이 축에서 CUBRID는 PG에 가깝다.

### 1.5 ARIES WAL (Undo+Redo 통합 로그)

- 로그 레코드는 `LOG_LSA`(페이지+오프셋)로 식별, undo/redo 데이터를 함께 담는다. LZ4 압축 지원.
- 복구는 정통 ARIES 3단계: **Analysis → Redo(체크포인트부터) → Undo(loser)**, CLR(보상 레코드)로 반복 undo 방지.
- WAL 규칙: 데이터 페이지 flush 전에 해당 로그가 먼저 디스크에 (페이지가 page LSA 필드를 보유).
- 비교: PG의 WAL은 **redo-only**(undo가 없고 구버전이 힙에 있으므로), Oracle은 redo 로그 + 별도 undo 테이블스페이스로 이원화. CUBRID는 단일 로그에 undo/redo를 함께 담는 교과서적 ARIES라는 점에서 셋 중 가장 "논문형" 구조다.

---

## 2. 최상위 컴포넌트 분할 (대영역 9개)

사용자 예시(BROKER 영역 / SERVER 영역)를 다음 9개 대영역으로 확장한다. 괄호는 실제 소스 트리 매핑.

```
[A] 접속·중계 계층      src/broker/                      cub_broker, cub_cas, shard proxy
[B] SQL 컴파일 계층     src/parser, optimizer, xasl      ★클라이언트측(CS/SA_MODE)
[C] 객체·스키마 계층    src/object, compat               스키마/권한/트리거/워크스페이스/DB_VALUE
[D] 통신 계층           src/connection, communication    CSS 프로토콜, 요청 디스패치, cub_master
[E] 서버 실행 계층      src/query, thread, session       XASL 실행기, 스캔, 함수, 워커풀, Vacuum
[F] 트랜잭션 계층       src/transaction                  MVCC, Lock, WAL, Recovery, Boot, HA복제
[G] 스토리지 계층       src/storage                      버퍼풀, 힙, B+tree, 볼륨/파일, DWB, TDE
[H] 확장 실행기         src/sp, method + pl_engine/      Java 저장프로시저(PL), 메서드 호출
[I] 횡단·운영 계층      src/base, monitor, executables,  에러/메모리/성능계측, csql, 유틸리티,
                        loaddb, heaplayers, cm_common    벌크로더, 관리도구
```

프로세스 배치 관점으로 다시 그리면:

```
브로커 호스트: [A] + (cubridcs = [B]+[C]+[D클라이언트측])
서버 호스트  : cub_master([D]) + cub_server([D서버측]+[E]+[F]+[G]) + cub_pl([H])
공용        : [I] 는 모든 바이너리에 링크되는 기반
```

성능 관점 1차 분류(상세는 7장):
- **[G]·[F]·[E]가 성능의 심장**(핫패스: 버퍼풀 latch, B+tree, WAL append, lock, 실행기 내부 루프).
- **[A]·[D]는 처리량/지연의 관문**(연결 churn, 직렬화 왕복)이되 쿼리당 비용은 상대적으로 작음 — 사용자의 직관("비교적 성능 영향 크지 않음")과 일치하나, 짧은 쿼리 폭주(OLTP) 환경에서는 왕복·직렬화 비용이 지배 요인이 될 수 있음.
- **[B]는 위치가 특이**: 비용 자체는 크지만 서버가 아닌 브로커 호스트에서 소모되고, 서버측 플랜(XASL) 캐시로 반복 비용이 상쇄됨.

---

## 3. 컴포넌트별 상세 설계 영역

각 영역마다 (책임 / 핵심 소스 / 핵심 자료구조·프로토콜 / 설계 시 확정 사항)을 정의한다.

### [A] 접속·중계 계층 — Broker & CAS  (`src/broker/`)

**책임**: 클라이언트 접속 수락, CAS 풀 관리, SQL 요청 수신·응답, 샤딩 라우팅, 접근제어, SQL 로그.

| 서브컴포넌트 | 핵심 소스 | 역할 |
|---|---|---|
| A-1 Broker Dispatcher | `broker.c` | 포트 바인딩, 접속을 유휴 CAS에 배정 |
| A-2 CAS 세션 실행기 | `cas.c`, `cas_execute.c`, `cas_function.c`, `cas_handle.c`, `cas_network.c` | 드라이버 프로토콜 처리, **여기서 [B]를 호출해 SQL을 컴파일**, prepared statement 핸들 관리 |
| A-3 공유메모리 IPC | `broker_shm.c` | `T_SHM_BROKER`, `T_SHM_APPL_SERVER` — 상태/설정/통계. broker↔CAS 간 유일한 통신로 |
| A-4 Shard Proxy | `shard_proxy.c`, `shard_metadata.c` | 선택적 샤딩 계층 (broker→proxy→CAS) |
| A-5 접근제어·로그 | `broker_acl.c`, `broker_log_top.c` 등 | ACL, 슬로우쿼리/SQL 로그 분석 도구 |
| A-6 설정·관리 | `broker_config.c`, `broker_admin*.c`, `broker_monitor.c` | `cubrid_broker.conf` 파싱, 관리 명령 |

**설계 확정 사항**
- CAS = **프로세스**(fork) 유지 여부. 장점: 세션 크래시 격리, 드라이버 메모리 누수 봉쇄. 단점: 프로세스당 메모리, 컨텍스트 스위치. (Oracle Shared Server의 프로세스 풀과 유사한 트레이드오프)
- broker↔CAS는 공유메모리 폴링 기반 — 구조체(`T_BROKER_INFO`, `T_APPL_SERVER_INFO`)가 packed라 정렬(alignment)이 ABI다. 재설계 시 버전드 IPC 스키마 필요.
- CAS 재사용(keep-alive) 정책과 트랜잭션 스티키니스(한 트랜잭션 중 CAS 고정) 규칙.
- 에러 모델이 엔진과 분리되어 있음(`cas_error_log_write()` ≠ `er_set()`) — 통합할지 유지할지.

### [B] SQL 컴파일 계층 — Parser / Optimizer / XASL  (`src/parser, optimizer, xasl` — 클라이언트측)

**책임**: SQL 텍스트 → 실행 가능한 플랜(XASL) 생성. 전 과정이 CS/SA 모드에서만 컴파일된다.

파이프라인(모듈 경계 그대로):
```
SQL → csql_lexer.l → csql_grammar.y(646KB bison) → PT_NODE 트리
    → name_resolution.c(이름해석) → semantic_check.c → type_checking.c(타입추론)
    → view_transform.c(뷰 전개) → [optimizer] → xasl_generation.c → XASL_NODE
    → xasl_to_stream.c(직렬화) ─── 네트워크 ──▶ 서버
```

| 서브컴포넌트 | 핵심 소스 | 역할 |
|---|---|---|
| B-1 Lexer/Grammar | `csql_lexer.l`, `csql_grammar.y` | 전 SQL 구문 정의. bison 재생성이 느려 수정 비용 큼 |
| B-2 의미분석 | `name_resolution.c`, `semantic_check.c`, `type_checking.c` | 스키마 객체 바인딩([C]에 의존), 타입/함수 시그니처 검증 |
| B-3 재작성기 | `optimizer/rewriter/`, `view_transform.c`, `cnf.c` | 뷰 전개, 조건 정규화(CNF), 서브쿼리 변환 |
| B-4 비용기반 플래너 | `query_graph.c`, `query_planner.c`, `plan_generation.c`, `query_bitset.c` | 조인그래프 구성 → 비트셋 기반 상향식 DP 조인순서 열거 → 인덱스/정렬 비용 비교. 통계는 서버에서 fetch(`qo_get_class_info`) |
| B-5 XASL 생성·정의 | `xasl_generation.c` + `src/xasl/*` | PT→XASL 변환. `src/xasl/`는 노드 타입 정의만 모아둔 헤더 모듈 |
| B-6 직렬화 | `query/xasl_to_stream.c`(클라) ↔ `query/stream_to_xasl.c`(서버) | 클라·서버 포맷 완전 일치 계약 |

**핵심 자료구조**: `PT_NODE`(union 기반 링크드리스트 파스트리), `QO_ENV/QO_PLAN/QO_NODE/QO_TERM`(플래너), `XASL_NODE`/`PRED_EXPR`/`REGU_VARIABLE`(플랜).

**설계 확정 사항**
- 클라이언트 컴파일 유지가 대전제(1.2). 이를 유지하면 → 통계 전송 프로토콜, XASL 버저닝, 서버측 XASL 플랜 캐시(파라미터 `max_plan_cache_entries`)와의 캐시 키(쿼리문 해시) 규약을 함께 확정해야 한다.
- PG는 파서·플래너가 서버 내부라 플랜캐시가 백엔드-로컬(prepared stmt)이고, Oracle은 Shared Pool의 Library Cache가 전역 공유 플랜캐시다. CUBRID는 "컴파일=클라, 캐시=서버"라는 제3의 배치 — 재설계 시 이 배치의 무결성(스키마 변경 시 캐시 무효화 전파)이 설계 포인트.

### [C] 객체·스키마 계층  (`src/object/`, `src/compat/`)

**책임**: 스키마(클래스) 관리, 권한, 트리거, 클라이언트측 객체 캐시(워크스페이스), 값 시스템(DB_VALUE), 객체↔디스크 표현 변환. CUBRID가 "객체관계형"인 이유가 이 계층에 있다.

| 서브컴포넌트 | 핵심 소스 | 역할 |
|---|---|---|
| C-1 스키마 매니저 | `schema_manager.c`, `class_object.c`, `schema_template.c` | 클래스(테이블) 정의 CRUD, 상속 포함 |
| C-2 시스템 카탈로그 정의 | `schema_system_catalog_*.cpp`, `schema_information_schema_*.cpp` | `_db_*` 카탈로그 클래스와 INFORMATION_SCHEMA 뷰 설치 |
| C-3 권한 | `authenticate*.c/cpp` | 사용자/GRANT, 권한 캐시 |
| C-4 트리거 | `trigger_manager.c` | 트리거 정의·발화 |
| C-5 워크스페이스 | `work_space.c`, `object_accessor.c`, `object_template.c` | **클라이언트측 객체 캐시**(MOP). 객체를 메모리에 올려 조작 후 flush |
| C-6 변환기 | `transform.c`, `transform_cl.c`, `object_representation.c` | 메모리 객체 ↔ 디스크 레코드(RECDES) 직렬화 |
| C-7 값 시스템·클라 API | `compat/dbtype*.h`, `db_*.c`, `db_json.cpp` | `DB_VALUE` 만능 컨테이너, `db_make_*`/`db_get_*` 공개 C API, JSON 타입 |
| C-8 도메인/프리미티브 | `object_domain.c`, `object_primitive.c` | 타입 도메인, 타입별 비교/직렬화 함수 테이블 |

**설계 확정 사항**
- `DB_VALUE`는 전 모듈이 쓰는 만능 값 컨테이너 — 크기/태그 레이아웃 변경은 전체 ABI 변경과 동급. 최우선 동결 대상.
- 워크스페이스(클라이언트 객체 캐시)와 서버 MVCC 간 일관성 규약(fetch 시점 스냅샷, flush 시점 잠금).
- PG 대응물: 시스템 카탈로그(pg_class 등)+syscache/relcache — 단 PG는 전부 서버측. Oracle 대응물: Data Dictionary + Row Cache. CUBRID만 이 캐시가 클라이언트 라이브러리에 있다.

### [D] 통신 계층  (`src/connection/`, `src/communication/`, master)

**책임**: CAS(클라)↔서버 간 CSS 프로토콜, 요청 디스패치 테이블, cub_master의 접속 중계, HA 하트비트.

| 서브컴포넌트 | 핵심 소스 | 역할 |
|---|---|---|
| D-1 CSS 전송 | `connection/tcp.c`, `connection_cl.c`, `connection_sr.c`, `connection_defs.h` | TCP, `CSS_CONN_ENTRY` 접속 상태(서버는 고정배열 `css_Conn_array`), 부분 send/recv 처리 |
| D-2 요청 디스패치 | `communication/network.h`, `network_sr.c`, `network_interface_sr.c` / `network_interface_cl.c` | 요청ID 상수 목록 → 서버 `net_Requests[]` 핸들러 테이블. 클라 스텁 ↔ 서버 핸들러 쌍으로 RPC 유사 구조 |
| D-3 접속 중계·마스터 | `executables/master.c`, `master_request.c`, `connection/master_connector.cpp` | cub_master가 최초 접속을 받아 cub_server로 릴레이 |
| D-4 HA 하트비트 | `connection/heartbeat.c`, `executables/master_heartbeat.c` | 노드 생사 감시, 페일오버 판단(cubrid heartbeat) |
| D-5 콜백 채널 | `network_callback_{cl,sr}.cpp` | 서버 실행 중 클라이언트(메서드/SP) 역호출용 양방향 채널 |

**설계 확정 사항**
- 새 서버 요청 추가 규약이 3파일 계약(network.h 상수 + net_Requests[] 등록 + 핸들러 구현)으로 굳어져 있음 — 재설계 시 IDL/코드젠으로 대체할지 결정.
- 프로토콜 버저닝·capability 협상(현재 `network_cl.c`의 capability check) 명세화.

### [E] 서버 실행 계층  (`src/query/`, `src/thread/`, `src/session/`)

**책임**: 역직렬화된 XASL 트리 실행, 스캔, 함수 평가, 중간결과 관리, 워커 스레딩, 세션 상태, 그리고 Vacuum 데몬.

| 서브컴포넌트 | 핵심 소스 | 역할 |
|---|---|---|
| E-1 실행기 코어 | `query_executor.c` (~27K줄) | `qexec_execute_mainblock()` — XASL 트리 워커. BUILDLIST/BUILDVALUE/UNION/SCAN_PROC 등 노드 타입별 실행 |
| E-2 스캔 매니저 | `scan_manager.c` | heap/index/list/set/method/hash 스캔의 open→next→close 통일 인터페이스 |
| E-3 값 평가 | `fetch.c` (REGU_VARIABLE 재귀평가), `query_evaluator.c` (PRED_EXPR) | 튜플에서 값 추출·술어 판정. 깊은 식은 스택 위험 |
| E-4 함수 라이브러리 | `string_opfunc.c`(~28K줄), `arithmetic.c`, `numeric_opfunc.c`, `query_opfunc.c`(집계), `query_analytic.cpp`(윈도우) | 내장함수 구현체. `DB_VALUE* → DB_VALUE*` 규약 |
| E-5 조인·해시 | `query_hash_join.c`, `query_hash_scan.c` | 해시조인 |
| E-6 중간결과 | `list_file.c`, `query_manager.c`, `cursor.c` | 리스트파일(임시결과, 메모리→temp볼륨 스필), 결과셋/커서, 쿼리·플랜 캐시 관리 |
| E-7 병렬 실행 | `query/parallel/` (`px_*`) | 병렬 heap scan/hash join/sort 워커 |
| E-8 Vacuum | `query/vacuum.c` | 로그 구동 MVCC 가비지 수거 데몬(1.4) |
| E-9 스레딩 기반 | `thread/thread_worker_pool*`, `thread_daemon*`, `thread_manager`, `critical_section.c` | 요청 단위 워커풀, 데몬 프레임워크, lock-free 해시맵. `THREAD_ENTRY *thread_p`가 전 서버 함수 1번 인자라는 관례의 출처 |
| E-10 세션 | `session/session.c` | 접속 간 유지되는 세션 변수/prepared 상태 |

**설계 확정 사항**
- 실행 모델은 튜플 파이프라인 + 리스트파일 물질화 혼합(Volcano 유사). 재설계 시 벡터화/배치 실행 도입 여부가 최대 성능 결정.
- 리스트파일 I/O가 대형 결과에서 병목 — temp 스필 정책(`temp_file_memory_size_in_pages` 초과 시 디스크)과 함께 설계.
- Vacuum이 실행계층에 있는 이유(스캔·인덱스 삭제 로직 재사용)를 유지할지, PG처럼 스토리지 계층으로 내릴지.

### [F] 트랜잭션 계층  (`src/transaction/`)

**책임**: 동시성(락+MVCC), 내구성(WAL), 복구(ARIES), 서버 부트, 2PC, HA 복제, Flashback. 범위 기준 최대 모듈.

| 서브컴포넌트 | 핵심 소스 | 역할 |
|---|---|---|
| F-1 Lock Manager | `lock_manager.c`(~15K줄), `lock_table.c`, `wait_for_graph.c` | DB→테이블(SCH-S/SCH-M/IS/IX/S/X)→행(S/X) 계층락, 인텐트락, 에스컬레이션, 주기적 wait-for graph 데드락 감지 |
| F-2 MVCC | `mvcc.c`, `mvcc_table.cpp`, `mvcc_active_tran.cpp` | 스냅샷 생성·가시성 판정, 활성 트랜잭션 테이블 |
| F-3 WAL 쓰기 | `log_append.cpp`, `log_manager.c`, `log_page_buffer.c`, `log_compress.c` | 로그 레코드 append, 로그버퍼, 체크포인트(`logpb_checkpoint`), 아카이브, LZ4 |
| F-4 복구 | `log_recovery.c`(~15K줄), `log_recovery_redo_parallel.cpp`, `recovery.c` | ARIES 3단계 + **병렬 redo**. torn 로그페이지 처리 포함 |
| F-5 트랜잭션 제어 | `transaction_sr.c`, `log_tran_table.c`, `transaction_cl.c` | commit/abort/savepoint, 트랜잭션 테이블(tran_index ≠ MVCCID 주의) |
| F-6 Boot | `boot_sr.c` / `boot_cl.c` | 서버 기동 시퀀스(서브시스템 초기화 순서 엄격), DB 생성 |
| F-7 분산·시점복구 | `log_2pc.c`, `flashback.c` | 2단계커밋, 플래시백 |
| F-8 HA 복제 | `replication.c`, `log_applier.c`, `log_writer.c` | 트랜잭션 로그 배송·재적용(copylogdb/applylogdb 계열) — PG 스트리밍 복제/Oracle Data Guard 대응물 |

**설계 확정 사항**
- 락과 latch의 역할 분리 원칙(락=논리적 일관성, latch=물리적 페이지 일관성)은 [G]와의 계약.
- MVCCID 64-bit 단조증가(랩어라운드 없음)는 PG의 32-bit XID freeze 문제를 원천 회피한 설계 — 유지 권장.
- HA는 "로그 재적용을 SQL로 변환 적용"하는 논리복제 성격(`log_applier_sql_log.c`) — 물리 로그십핑(PG/Oracle 물리 standby)로 갈지 여부가 재설계의 큰 갈림길.

### [G] 스토리지 계층  (`src/storage/`)

**책임**: 버퍼풀, 볼륨/섹터/파일 공간관리, 힙(슬롯페이지), B+tree, 오버플로, 카탈로그 영속화, 통계, 외부저장(LOB), 이중쓰기(DWB), 투명암호화(TDE).

핵심 식별자 체계(전 계층 공용 어휘):
```
VPID(볼륨+페이지) → 물리 페이지 주소        OID(볼륨+페이지+슬롯) → 객체 주소
VFID(볼륨+파일)  → 논리 파일               HFID = VFID+헤더페이지 (힙)
BTID = VFID+루트페이지 (B+tree)            RECDES → 레코드 버퍼 기술자
PAGE_PTR → 버퍼풀 페이지 포인터 (pgbuf_* 경유 필수)
```

| 서브컴포넌트 | 핵심 소스 | 역할 |
|---|---|---|
| G-1 Page Buffer(버퍼풀) | `page_buffer.c`(~17K줄) | fix/unfix + READ/WRITE latch, dirty 추적, LRU 교체, flush. 프로토콜: fix→(수정 시 set_dirty)→unfix 짝 맞춤, `PGBUF_WATCHER`로 순서화된 다중 fix |
| G-2 저수준 I/O·내구성 | `file_io.c`, `double_write_buffer.cpp`, `tde.c` | 볼륨 raw I/O, **DWB**(모든 데이터페이지를 DWB에 먼저 써서 partial write 복구), 페이지/로그 암호화 |
| G-3 공간 관리 | `disk_manager.c`(볼륨/섹터 예약), `file_manager.c`(논리 파일 할당) | 10.x부터 볼륨 용도는 permanent/temporary 2분류(구 data/index/generic/temp 분류는 폐기) |
| G-4 힙 | `heap_file.c`(~27K줄), `slotted_page.c`, `overflow_file.c` | 테이블당 힙파일 1개, 슬롯페이지 레이아웃, 멀티페이지 레코드(오버플로), MVCC 버전 체인, 클래스 표현 캐시 |
| G-5 인덱스 | `btree.c`(~37K줄), `btree_load.c`, `btree_unique.cpp`, `external_sort.c` | B+tree 탐색/범위스캔/삽입/MVCC삭제, 벌크 빌드(외부정렬), 유니크 통계 |
| G-6 카탈로그·통계 | `system_catalog.c`, `catalog_class.c`, `statistics_sr.c/_cl.c`, `statistics_ndv.c` | 카탈로그 영속화, 옵티마이저 통계(NDV 포함) — [B]가 소비 |
| G-7 외부저장/LOB | `es*.c` (+ `object/lob_locator.cpp`) | LOB URI API, POSIX/OWFS 백엔드 |
| G-8 기타 구조 | `extendible_hash.c` | 확장해시(내부용) |

**디스크 볼륨 구조(설계 어휘로 확정)**
- 영구 데이터 볼륨: 힙파일·B+tree파일·시스템파일 저장. 재시작/크래시 후에도 유지.
- 임시 볼륨: 정렬/중간결과 전용, 재시작 시 초기화.
- 로그 볼륨: **활성 로그 1개 + 아카이브 로그 N개 + 백그라운드 아카이빙 로그 1개**(아카이브 생성 전 임시 기록).
- 컨트롤 파일(volinfo): 전체 볼륨 이름/위치/ID, 백업 정보, 로그 정보 — 재시작 시 최초로 읽는 파일.
- DWB 파일: partial write 복구용(크기: `double_write_buffer_size`). PG의 full_page_writes, Oracle/InnoDB의 doublewrite에 대응.

**설계 확정 사항**
- "모든 영구 페이지 접근은 버퍼풀 경유, file_io 직접 접근 금지" 원칙과 latch 순서 규약(부모→자식)이 계층의 헌법.
- 페이지가 page LSA를 보유하고 flush가 WAL 규칙을 지키는 것 — [F]와의 핵심 계약.
- 온디스크 포맷(슬롯 레이아웃, 레코드 디스크립터) 변경은 복구 레코드·TDE·check/dump 코드까지 동반 수정 — 포맷 버저닝 체계 필수.

### [H] 확장 실행기 — 저장 프로시저/메서드  (`src/sp/`, `src/method/`, `pl_engine/`)

| 서브컴포넌트 | 핵심 소스 | 역할 |
|---|---|---|
| H-1 PL 서버 | `pl_engine/`(Java, Gradle), `sp/pl_sr_jvm.cpp`(JNI) | 별도 Java PL 프로세스 + JNI 브리지. Java SP / PL/CSQL 컴파일·실행(`pl_compile_handler`, `pl_executor`) |
| H-2 SP 카탈로그·세션 | `sp/sp_catalog.cpp`, `pl_session.cpp`, `pl_query_cursor.cpp` | 프로시저 메타데이터, PL 실행 스택, 서버로의 역쿼리 커서 |
| H-3 메서드 호출 | `method/*` | 쿼리 실행 중 메서드/SP 호출을 [D-5] 콜백 채널로 클라이언트/PL에 위임 |

**설계 확정**: 실행 중 "서버→PL→다시 서버 쿼리"의 재진입 계약(트랜잭션 공유, 커서 수명)이 가장 까다로운 부분. Oracle의 내장 PL/SQL VM과 달리 프로세스 분리형(안정성 우선)을 유지할지 결정.

### [I] 횡단·운영 계층  (`src/base/`, `monitor/`, `executables/`, `loaddb/`, `heaplayers/`, `cm_common/`)

| 서브컴포넌트 | 핵심 소스 | 역할 |
|---|---|---|
| I-1 기반 | `base/error_manager.c`(음수 에러코드+er_set, C에러모델·예외금지), `base/memory_*`, `area_alloc.c`, `perf_monitor.c`, `environment_variable.c` | 에러/메모리/성능계측/환경. `db_private_alloc`, `free_and_init` 규약 |
| I-2 커스텀 할당기 | `heaplayers/` (lea_heap 등, 3rd-party) | 엔진 내장 malloc 계층 |
| I-3 성능 통계 | `monitor/*` | 통계 수집·등록 프레임워크(perf_monitor의 후속 구조) |
| I-4 실행 파일 | `executables/server.c`(cub_server), `master.c`, `csql*.c`, `commdb.c`, `compactdb*.c`, `unloaddb.c`, `checksumdb.c`, `migrate.c` | 서버/마스터 엔트리포인트, 대화형 CSQL, 관리 유틸리티 |
| I-5 벌크 로더 | `loaddb/*` (bison/flex 문법, `load_server_loader.cpp`) | 서버측 병렬 벌크로드 |
| I-6 매니저 공통 | `cm_common/` | CUBRID Manager(별도 서브모듈) 공용 유틸 |

CSQL 위치 정리(사용자 예시의 "CAS CSQL BROKER 영역" 관련): CSQL은 브로커를 거치지 않는 **직결 클라이언트**다. `csql -C`는 cubridcs(CS_MODE)로 서버에 접속, `csql -S`는 cubridsa(SA_MODE)로 서버 없이 단독 구동. 즉 CSQL은 [A]가 아니라 [B]+[C]+[D] 라이브러리의 소비자이며, CAS와 형제 관계다.

---

## 4. 컴포넌트 간 의존 관계 (계약의 지도)

소스 트리 내부 문서가 명시한 의존 그래프를 설계 계약 관점으로 옮기면:

```
        [B]parser ◀──▶ optimizer ──▶ xasl ──▶ [E]query
             │             │           │           │
             ├─▶ [C]object ├─▶ object  ├─▶ object  ├─▶ [C]object
             └─▶ [G]stats  └─▶ stats   │           ├─▶ [G]storage ◀──▶ [F]transaction
                                       │           └─▶ [F]transaction
   기반: [I]base, [C]compat(DB_VALUE) — 거의 전 모듈이 의존 (아래 방향 의존만 허용)
```

강결합(양방향 사이클)으로 공식화된 지점 — 재설계 시 인터페이스로 끊을지, 의도적 사이클로 인정할지 결정 필요:
1. **storage ↔ transaction**: 스토리지가 MVCC/락을 쓰고, 트랜잭션(복구)이 페이지/B+tree를 조작.
2. **query ↔ storage**: 실행기가 스토리지를 스캔하고, 스토리지가 실행기 콜백을 역호출.
3. **parser ↔ optimizer**: 파서가 옵티마이저를 부르고, 옵티마이저가 파서 타입을 참조.
4. **object가 중앙 허브**: parser/optimizer/query/xasl/method/sp 전부가 스키마 헤더를 include.

---

## 5. 데이터 흐름: 쿼리 한 개의 일생 (SELECT 기준)

```
 1. 드라이버 → cub_broker 접속 → 유휴 cub_cas 배정                     [A]
 2. CAS: prepare 요청 수신 → 파스(PT_NODE) → 의미분석                   [B]
      · 이름해석이 스키마 필요 → 워크스페이스 캐시 or 서버 fetch        [C][D]
 3. CAS: 통계 fetch → 비용기반 플래닝(QO_PLAN) → XASL 생성              [B][G-6]
 4. CAS: XASL 직렬화 → CSS로 서버 전송 (서버 XASL 캐시 히트 시 2~3 생략) [B-6][D]
 5. cub_server: 워커 스레드 배정 → stream_to_xasl 역직렬화               [E-9][B-6]
 6. 트랜잭션 컨텍스트: 스냅샷 확보(MVCC), 필요 락 획득                   [F-2][F-1]
 7. qexec_execute_mainblock: 스캔 open → heap/btree 페이지 fix(latch)    [E][G-1,4,5]
      · 각 행: mvcc_satisfies_snapshot 가시성 판정 → 술어평가 → 값fetch  [F-2][E-3]
 8. 중간결과 리스트파일(메모리→temp볼륨 스필), 정렬/집계/조인            [E-6][G-3]
 9. (DML이면) WAL append 선행 → 페이지 수정 → dirty 마킹                 [F-3][G-1]
10. 결과 튜플 스트림 → CAS → 드라이버 프로토콜로 변환 → 앱               [D][A-2]
11. commit: 로그 flush(내구성 확정) → 락 해제 → MVCCID 종결              [F-3][F-1]
12. (백그라운드) Vacuum이 로그를 따라 죽은 버전 회수, 체크포인트, DWB flush [E-8][F-3][G-2]
```

---

## 6. PG / Oracle 과의 구조 비교 — 설계 참고표

### 6.1 프로세스·스레드 모델

| 축 | CUBRID | PostgreSQL | Oracle |
|---|---|---|---|
| 접속 수락 | cub_broker(+cub_master 중계) | postmaster | Listener |
| 세션 실행 단위 | CAS 프로세스(브로커측) + 서버는 요청단위 스레드 | 커넥션당 backend 프로세스 | Dedicated 프로세스 or Shared Server 풀 |
| 서버 본체 | DB당 1개 멀티스레드 cub_server | 프로세스 군집 + 공유메모리 | 인스턴스(다수 백그라운드 프로세스)+SGA |
| 커넥션 풀링 | **내장**(브로커/CAS 재사용) | 외장(pgBouncer 등) | Shared Server/DRCP로 내장 가능 |
| 시사점 | 접속 폭주 격리는 CUBRID가 기본 제공. 대신 계층 1개만큼 왕복 추가 | 단순하나 대량접속에 취약 | 유연하나 구성 복잡 |

### 6.2 SQL 처리 위치 (3사 중 CUBRID만 다른 축)

| 단계 | CUBRID | PG | Oracle |
|---|---|---|---|
| Parse/Rewrite/Plan | **클라이언트(CAS/CSQL)** | 서버(backend) | 서버 |
| 플랜 캐시 | 서버측 XASL 캐시(전역) | backend-로컬(prepared) | Shared Pool Library Cache(전역) |
| 실행 | 서버 | 서버 | 서버 |
| 시사점 | 컴파일 CPU를 브로커 계층으로 수평확장 가능. 대신 직렬화 계약·통계 왕복·캐시 무효화 전파가 추가 설계 비용 | — | — |

### 6.3 MVCC / Undo / 공간 회수

| 축 | CUBRID | PG | Oracle |
|---|---|---|---|
| 버전 위치 | 힙 내 다중버전(insert/delete MVCCID) | 힙 내 다중버전(xmin/xmax) | 최신본만 힙, 과거는 Undo로 재구성 |
| 회수 | Vacuum 데몬(**WAL 로그 구동**) | Autovacuum(힙 스캔 구동) | 불필요(Undo 순환) |
| ID 랩어라운드 | 없음(64-bit) | 있음(32-bit → freeze 필요) | 해당 없음 |
| 시사점 | PG 계열의 단순성 + 로그구동으로 스캔비용 절감. 단 Vacuum 지연 시 힙/인덱스 비대는 PG와 동일한 리스크 | | |

### 6.4 로깅·복구·내구성

| 축 | CUBRID | PG | Oracle |
|---|---|---|---|
| 로그 내용 | ARIES: undo+redo 단일 로그, CLR | redo-only WAL | Redo 로그 + 별도 Undo 세그먼트 |
| 복구 | Analysis→Redo(병렬 지원)→Undo | Redo 재생만(undo 불필요) | Roll-forward 후 롤백 |
| Torn page 방어 | DWB(이중쓰기 버퍼) | full_page_writes(체크포인트 후 첫 수정 전체페이지 로깅) | DB가 페이지 검증+복구, (InnoDB식 DWB와는 다름) |
| 아카이빙 | 활성로그→아카이브(+백그라운드 아카이브) | WAL 아카이빙 | ARCn 아카이버 |

### 6.5 메모리 구조 대응표

| CUBRID | PG | Oracle |
|---|---|---|
| Page Buffer(서버) | shared_buffers | Buffer Cache(SGA) |
| 로그 페이지 버퍼 | WAL buffers | Redo Log Buffer |
| XASL 캐시 | (prepared/백엔드 로컬) | Library Cache |
| 워크스페이스(클라 객체캐시) | relcache/syscache(서버측) | Row Cache(서버측) |
| 리스트파일 메모리→temp | work_mem→temp files | PGA→TEMP tablespace |
| broker/CAS 공유메모리 | 해당 없음 | 대응물 없음(굳이 찾으면 Dispatcher 큐) |

### 6.6 복제·HA

| 축 | CUBRID | PG | Oracle |
|---|---|---|---|
| 방식 | 트랜잭션 로그 배송+**SQL 변환 재적용**(log_applier) | 물리 스트리밍(WAL) + 논리복제 | Data Guard 물리/논리 |
| 감시/절체 | cub_master + heartbeat 내장 | 외장(Patroni 등) | Observer/FSFO |
| 시사점 | 논리 재적용은 이기종·부분복제에 유리하나 물리복제 대비 적용 지연·경합에 취약 — 재설계 시 물리 로그십핑 트랙 추가가 유력 후보 | | |

---

## 7. 성능 임계도 분류 (설계 우선순위)

**Tier-0 핫패스 — 사이클 단위 설계 필요 (전체 성능의 결정자)**
- [G-1] 버퍼풀 fix/unfix·latch 경합, LRU — 접근당 나노초 싸움
- [G-5] btree.c 탐색/삽입 (~37K줄이 괜히 아님), [G-4] 힙/슬롯페이지
- [F-3] WAL append 직렬화 지점(모든 DML의 병목), 로그버퍼·그룹커밋
- [F-1] 락 테이블 경합·에스컬레이션, [F-2] 스냅샷 생성 비용
- [E-1~3] 실행기 내부 루프(행당 함수호출 수), REGU_VARIABLE 재귀평가

**Tier-1 준임계 — 워크로드에 따라 지배 가능**
- [A] CAS 배정/재사용(연결 churn 심한 웹 OLTP), 드라이버↔CAS 왕복
- [B-6][D] XASL 직렬화/역직렬화·네트워크 왕복(짧은 쿼리 다발 시), 플랜캐시 적중률
- [E-6] 리스트파일 temp 스필, [G-5] external sort, [E-7] 병렬 실행 스케일
- [E-8] Vacuum 지연(장기적으로 힙·인덱스 비대 → Tier-0을 오염), 체크포인트·DWB flush 폭주

**Tier-2 비임계 — 정확성·운용성 우선, 성능은 후순위**
- [B-1~5] 파싱·플래닝 1회 비용(캐시로 상쇄, 브로커 호스트로 격리)
- [I] 유틸리티(compactdb/unloaddb/checksumdb), loaddb(오프라인 성격), 매니저, 모니터링
- [H] SP/PL (호출 경계 비용은 있으나 빈도 설계 문제)

사용자 예시의 직관 검증: "BROKER/CAS/CSQL 쪽은 성능 영향이 비교적 작다"는 **플래닝 비용 관점에선 맞다**(클라측 격리+캐시). 단, 접속 churn·왕복 지연 관점에선 Tier-1이므로 "무시 가능"은 아니고 "서버 핫패스와 독립적으로 확장 가능한 축"으로 정의하는 것이 정확하다.

---

## 8. 재설계 결정 체크리스트 (Architecture Decision Records 후보)

| # | 결정 항목 | 현재 CUBRID | 대안(참고) | 판단 기준 |
|---|---|---|---|---|
| ADR-1 | SQL 컴파일 위치 | 클라이언트(CAS) | 서버(PG/Oracle식) | 브로커 수평확장 가치 vs 직렬화 계약 유지비 |
| ADR-2 | CAS 실행 단위 | 프로세스 | 스레드/코루틴 | 크래시 격리 vs 메모리·스위칭 비용 |
| ADR-3 | XASL 직렬화 | 수제 스트림, 완전일치 계약 | 버저닝된 IDL/스키마 | 롤링 업그레이드 요구 여부 |
| ADR-4 | Vacuum 구동원 | WAL 로그 구동 데몬 | 힙스캔(PG) / Undo(Oracle) | 회수 지연 허용치, 로그 보존 결합도(13.5 X1 연동) |
| ADR-5 | HA 복제 | 로그→SQL 논리 재적용 | 물리 페이지/WAL 십핑 | 재적용 지연, 이기종 필요성 |
| ADR-6 | 실행 모델 | 튜플 단위 + 리스트파일 물질화 | 벡터화 배치 실행(12.6 상세) | OLAP 비중, 함수호출 오버헤드 |
| ADR-7 | 요청 디스패치 | 상수표+핸들러 3파일 계약 | 코드젠 RPC | 요청 추가 빈도 |
| ADR-8 | 에러 모델 | C 에러코드(er_set), 예외 금지 | — (동결 권장) | 전 코드베이스 관통 규약 |
| ADR-9 | 값 컨테이너 | DB_VALUE 단일 태그드 유니온 | — (동결 권장) | ABI 안정성 |
| ADR-10 | Torn write 방어 | DWB 파일 | full-page WAL(PG식) | 쓰기 증폭 vs 로그 팽창 |

**동결 권장(변경 시 CUBRID의 정체성/호환성이 깨지는 것)**: 1장의 5대 상수, DB_VALUE, OID/VPID/VFID 식별자 체계, 온디스크 볼륨·로그 구조, 카탈로그 클래스 체계.

---

# 제2부 — 워크로드·동시성 도메인·검증 설계

> 제1부의 대영역 [A]~[I] 분할을 전제로 3대 질문에 답한다.
> ① 각 모듈은 OLTP/OLAP 중 어느 쪽 성능을 결정하며, 어떤 최적화 기법이 유효한가.
> ② 서로 다른 쿼리가 동시에 실행될 때 **함께 쓰는(공유) 영역**과 **격리되어 도는 영역**은 정확히 어디인가 — 그리고 한 쿼리의 병렬(intra-query parallel, px) 실행은 어떤 별개의 공유 모델인가.
> ③ 유저 관점의 워크로드 시나리오별로 어떤 테스트를 어떤 합격 기준으로 통과해야 하는가.

---

## 9. 워크로드 축의 정의

### 9.1 두 워크로드의 물리적 본질

같은 엔진이라도 두 워크로드가 소모하는 자원의 "모양"이 다르다. 설계는 이 프로파일 차이에서 출발한다.

| 자원 축 | OLTP (짧은 트랜잭션 다발) | OLAP (긴 분석 쿼리 소수) |
|---|---|---|
| 지배 지표 | 지연시간 분포(p99/p999), 커밋 TPS, 접속 처리량 | 쿼리 경과시간, 스캔 대역폭, 병렬 확장비 |
| I/O 패턴 | 랜덤 소량 읽기/쓰기, fsync 빈발 | 순차 대량 읽기, temp 대량 쓰기 |
| CPU 패턴 | 경로 셋업 비용·동기화 대기 지배 | 내부 루프(행당 비용)·메모리 대역폭 지배 |
| 메모리 | 핫셋 상주(버퍼 히트율), 스냅샷/락 메타 | 정렬·해시 작업 메모리, 스필 관리 |
| 동시성 | 수백~수천 세션의 미세 경합 | 소수 쿼리의 워커 다발 + OLTP와의 간섭 |
| 실패 모드 | 꼬리지연 폭발, 데드락, 락 에스컬레이션 | 스필 폭주, 버퍼 오염, vacuum 지평선 정체 |

### 9.2 CUBRID의 태생적 포지션

CUBRID는 공식적으로 "웹 서비스 OLTP에 최적화"를 표방하며, 브로커/CAS 계층·요청 단위 서버 스레딩·크래시 격리는 전부 이 선언의 산물이다. 따라서 재설계의 기본 자세는 **"OLTP를 훼손하지 않는 선에서 OLAP 능력을 확장"**이며, 그 확장 수단이 이미 소스에 들어온 병렬 실행 프레임워크(`src/query/parallel/`, 이하 px)다. (src확인: px는 SCAN·HASH_JOIN·SORT·SUBQUERY·INDEX_BUILD 5개 병렬 유형과 페이지 수 기반 병렬도 자동계산 `compute_parallel_degree()`를 정의)

---

## 10. 모듈 × 워크로드 영향 매트릭스

영향도: ●●● 지배적 / ●● 유의미 / ● 간접·조건부 / ─ 무관.

| 모듈 | OLTP | OLAP | OLTP에서 보는 것 | OLAP에서 보는 것 |
|---|:-:|:-:|---|---|
| [A] Broker/CAS | ●●● | ● | 접속 churn 흡수, CAS 배정 지연, keep-alive 재사용률 | 결과셋 스트리밍 대역폭 정도 |
| [B] 컴파일(파서/옵티마이저) | ●● | ●●● | **캐시 적중이 전부** — 미스 시에만 비용, 그것도 브로커 호스트 CPU | **플랜 품질이 전부** — 조인순서/카디널리티 오차가 10²~10³배 차이 유발 |
| [B-6] XASL 직렬화 | ●● | ● | 짧은 쿼리 다발 시 왕복+직렬화가 고정세 | 실행시간 대비 무시 가능 |
| [C] 객체/스키마/워크스페이스 | ●● | ● | 스키마·권한 캐시 적중, DDL 시 무효화 폭풍 | 카탈로그 읽기 정도 |
| [D] 통신 | ●● | ● | 요청당 왕복 수, 소켓 처리 | 대량 fetch 스트리밍 |
| [E-1~3] 실행기 코어 | ●● | ●●● | 플랜 인스턴스화·스캔 open 셋업 비용, 포인트룩업 fast-path | **행당 명령수** — 튜플단위 해석 실행의 한계, 벡터화 후보 1순위 |
| [E-4] 함수 라이브러리 | ● | ●● | 소량 호출 | 행×열만큼 호출 — 호출 규약(DB_VALUE 왕복) 비용 누적 |
| [E-5~6] 해시조인/리스트파일 | ● | ●●● | 거의 안 탐 | 스필 여부가 성능 절벽. `sort_buffer_size × max_clients` 상한 주의(src확인: conf 주석) |
| [E-7] 병렬(px) | ─ | ●●● | (오히려 OLTP 워커 기아 리스크로 관리 대상) | 병렬도 산정, 워커 스케줄, 큐 경합 |
| [E-8] Vacuum | ●●(간접) | ●●(간접) | 지연 시 힙·인덱스 비대→랜덤I/O 증가 | **롱쿼리가 지평선을 붙잡아** vacuum 정체 유발(가해자) |
| [F-1] Lock Manager | ●●● | ● | 락 테이블 경합, 에스컬레이션 임계, 데드락 감지 주기 | 벌크 DML 아니면 한산(MVCC 읽기는 무락) |
| [F-2] MVCC/스냅샷 | ●●● | ●● | 트랜잭션당 스냅샷 생성 비용(활성 트랜잭션 수에 비례하는 구조 금지) | 롱스냅샷 유지 비용, 지평선 문제 |
| [F-3] WAL | ●●● | ●● | **커밋 경로의 유일 직렬화 지점**. group commit(src확인: `group_commit_interval_in_msecs`, `async_commit` 파라미터 존재) | 벌크 시 로그량 — no-logging 벌크 경로(px INDEX_BUILD가 이미 이 성격) |
| [F-4] 복구 | ●(RTO) | ● | 체크포인트 간격 vs 복구시간, 병렬 redo(src확인: `log_recovery_redo_parallel`) | 좌동 |
| [G-1] 버퍼풀 | ●●● | ●●● | 핫페이지 latch, 히트율, victim 탐색 비용 | **오염 방지**: 대량 스캔이 핫셋을 밀어내지 않게 — CUBRID는 이미 private LRU(세션/트랜잭션별 격리 리스트+quota) + shared LRU + zone1/2/3 구조(src확인: page_buffer.c) |
| [G-2] file_io/DWB/TDE | ●● | ●● | fsync 경로, DWB 배치 flush | 순차 read 경로가 DWB와 무관함 유지 |
| [G-4] 힙/슬롯페이지 | ●●● | ●●● | 삽입 분산(빈공간 탐색 bestspace), 행 잠금 단위 | 스캔 시 페이지당 행 밀도, MVCC 버전 스킵 비용 |
| [G-5] B+tree | ●●● | ●● | **우측 리프 핫스팟**(단조 증가 키), 루트~상위 latch, split 전파 | 범위스캔 리프 체인, 벌크 빌드(외부정렬) |
| [G-6] 통계 | ● | ●●● | 수집이 OLTP를 방해하지 않는 샘플링 | NDV(src확인: statistics_ndv.c)·히스토그램(src확인: optimizer/histogram/) 정확도 = 플랜 품질의 원료 |
| [H] SP/PL | ●● | ● | 호출 경계(JNI·프로세스 왕복) 고정비 | 루프 내 SP 호출 안티패턴 탐지 |
| [I] serial 등 공용객체 | ●●● | ─ | 시퀀스 = 대표적 단일 핫 오브젝트(캐시 배치 필수) | ─ |

읽는 법: OLTP 열의 ●●●만 모으면 "커밋 경로 + 락 + 버퍼 latch + B+tree 핫스팟 + 브로커"이고, OLAP 열의 ●●●만 모으면 "플랜 품질 + 실행기 내부루프 + 스필 + 병렬 + 버퍼 오염 방지"다. 두 집합의 교집합(버퍼풀, 힙)이 HTAP 간섭 설계의 핵심 전장이 된다.

---

## 11. OLTP 성능 설계 원칙 (기법 카탈로그)

### 11.1 커밋 경로 — 유일 직렬화 지점의 통치
- WAL append는 전 트랜잭션이 지나는 단일 관문. 설계 순서: 로그버퍼 append의 임계구역 최소화 → **group commit**(대기 커밋 일괄 fsync, `group_commit_interval_in_msecs`) → flush 데몬과 커밋 대기자의 핸드셰이크(깨우기 폭풍 방지).
- `async_commit`은 내구성-지연 트레이드오프 스위치로 존재(src확인) — 기본 off를 전제로 하되, 재설계 시 "커밋 등급"(sync/group/async)을 세션 단위 계약으로 명세화할 가치.
- 측정 규약: 커밋 TPS를 fsync 횟수로 나눈 **배치율**을 1급 지표로.

### 11.2 잠금·latch — 경합의 분산
- 락 테이블 해시 파티셔닝, 소유자 리스트의 캐시라인 분리(false sharing 방지).
- lock-free 기반 구조 재사용: `thread/thread_lockfree_hash_map`(src확인) — 신규 공유 맵은 이 계열을 1순위 후보로.
- 에스컬레이션 임계는 "행락 수천 개 = 정상 OLTP"를 오인하지 않도록 워크로드 프로파일과 함께 캘리브레이션.
- 데드락 감지는 주기 실행(wait-for graph) — 주기를 줄이면 감지 빠르나 스캔 비용 증가: p99 목표에서 역산.

### 11.3 스냅샷 비용 — O(활성 트랜잭션) 금지
- 스냅샷 = 최저 활성 MVCCID + 활성 비트배열. 트랜잭션 시작마다 복사 비용이 활성 수에 선형이면 고동시성에서 그 자체가 핫스팟. 재설계 원칙: 스냅샷 획득은 **획득자 측 O(1)에 근접**(세대 카운터/공유 스냅샷 재사용)하도록.

### 11.4 플랜 재사용 경로 — "컴파일 0회"가 정상 상태
- 정상 OLTP의 쿼리는 XASL 캐시 적중 + CAS statement 핸들 재사용으로 [B]를 전혀 타지 않아야 한다. 캐시 미스는 배포 직후·DDL 직후에만 발생하는 이벤트로 정의.
- 따라서 설계 대상은 캐시 자체보다 **무효화 전파**: DDL → 카탈로그 버전 → 서버 XASL 캐시 무효화 → CAS 핸들 재프리페어의 원자적 순서.

### 11.5 핫스팟 객체 — 구조가 아니라 데이터가 만드는 병목
- 단조 증가 키의 B+tree 우측 리프, serial(시퀀스), 집계 카운터 행. 기법: serial 캐시 구간 발급, 리프 예약 분할(right-most split 최적화), 핫 행의 그룹 갱신 유도.
- 이것들은 마이크로벤치가 아닌 **시나리오 테스트**(14장 W1)로만 드러난다.

### 11.6 왕복 최소화
- 드라이버↔CAS↔서버의 3홉 구조상, 문장당 왕복 수가 p99의 바닥을 정한다. prepare/execute/fetch 배칭, autocommit 시 execute+commit 융합 요청이 설계 후보.

## 12. OLAP 성능 설계 원칙 (기법 카탈로그)

### 12.1 병렬 실행 프레임워크(px)의 통치 규칙
- 구성(src확인): 전역 `px_worker_manager_global`(풀·병렬도 배분) + 쿼리별 worker manager + `px_thread_safe_queue`(작업 분배) + 유형별 구현(px_scan/, px_hash_join/, px_query_execute/, px_sort).
- 설계 확정 사항: ① 병렬도 산정식(`compute_parallel_degree`: 유형+페이지수+힌트)의 상한이 **OLTP 워커풀과 분리된 예산**에서 나오게 할 것(기아 방지, 13.5의 X3). ② 워커 간 분배 단위(페이지 range/morsel)와 스큐 대응(작업 훔치기). ③ 병렬 결과 병합 지점의 재직렬화 최소화.

### 12.2 스캔 — 대역폭과 오염 방지의 양립
- 순차 프리페치(다음 extent 미리 읽기)로 디스크 대역폭 포화가 목표.
- 오염 방지는 이미 구조 존재: 대량 스캔 페이지를 private LRU/하위 zone에 격리해 shared 핫셋 보호(src확인). 재설계 시 이 정책을 "스캔 크기 임계 → zone 배정 규칙"으로 명문화.

### 12.3 스필 계층 — 절벽을 경사로로
- 메모리(sort buffer, `temp_file_memory_size_in_pages`) → temp 볼륨의 2단 스필. 위험(src확인 conf 주석): sort buffer가 클라이언트 수만큼 곱해지는 상한 — 재설계 시 **전역 작업메모리 예산 + 쿼리별 grant** 방식(요청-승인)으로 전환 검토.
- external sort는 run 생성 크기·merge fan-in이 파라미터: temp I/O를 순차화하는 것이 본질.

### 12.4 조인·집계
- 해시조인 파티셔닝(스필 시 grace/hybrid), 해시테이블 build의 병렬 공유(px_hash_join).
- 집계는 그룹 수 추정(NDV) 실패 시 스필 폭주 — 12.5와 직결.

### 12.5 통계·플랜 품질 — OLAP의 절반은 옵티마이저
- NDV(statistics_ndv)·히스토그램(optimizer/histogram) 정확도, 샘플링 비율, 갱신 트리거(변경량 임계) 설계.
- 플랜 회귀 게이트(14.5) 없이는 옵티마이저 개선이 불가능하다 — 테스트 자산이 설계의 일부.

### 12.6 실행기 현대화(8장 ADR-6 상세)
- 튜플단위 재귀 평가(fetch.c의 REGU_VARIABLE)는 OLAP에서 행×식 노드만큼의 함수호출 — 배치(벡터) 평가로의 전환은 [E]와 [B-6](XASL 표현) 동시 개정이 필요한 대수술. "신규 노드 타입만 벡터화, 기존은 유지"의 점진 경로를 사전 설계에 포함.

---

## 13. 동시성 도메인 지도 — 무엇이 공유되고 무엇이 격리되는가

### 13.0 세 가지 실행 상황의 구분

동시성 설계는 아래 세 상황을 섞으면 안 된다. 공유의 "성격"이 다르기 때문이다.

```
(a) 단독 실행      : 경합 없음. 격리 도메인의 순수 비용(할당·셋업)만 보인다.
(b) 쿼리 간 동시   : 서로 모르는 쿼리들이 공유 도메인에서 '경쟁적 공유'(contention).
(c) 쿼리 내 병렬(px): 한 쿼리의 워커들이 상태를 '협조적 공유'(cooperation).
                      px 워커도 (b)의 공유 도메인에는 남들과 똑같이 경쟁자로 참여한다.
```

### 13.1 격리 도메인 — 쿼리/트랜잭션/세션이 단독 소유하는 것

경합이 원천 부재한 영역. 설계 목표는 동기화가 아니라 **할당 효율·지역성·수명 규율**.

| 소유 단위 | 격리 자원 | 소스 근거 | 설계 규율 |
|---|---|---|---|
| 워커 스레드 | `THREAD_ENTRY`(에러 컨텍스트, 자원 트래커), 스레드 프라이빗 힙(`db_private_alloc`) | thread/, base/ | 요청 종료 시 잔여 자원 0 — 트래커로 강제 |
| 트랜잭션 | 트랜잭션 디스크립터(LOG_TDES 슬롯), 세이브포인트 체인, 획득 락 리스트(엔트리는 소유) | log_tran_table.c | 슬롯 배열은 공유, 슬롯 내부는 단독 — 경계 명시 |
| 쿼리 실행 인스턴스 | XASL 실행상태(unpack info, val_list, SCAN_ID들), 리스트파일 **내용**, sort run, 커서 위치 | query/, xasl/ | 페이지는 공유 풀에서 빌리되 내용 접근은 단독 |
| 세션 | 세션 변수, prepared 상태(session.c), CAS 측 statement 핸들 | session/, broker/ | 세션 소멸·페일오버 시 회수 계약 |
| CAS 프로세스 | 파서 컨텍스트(PARSER_CONTEXT), 워크스페이스(객체 캐시), 드라이버 프로토콜 상태 | parser/, object/work_space.c | **프로세스 격리** — 타 세션과 주소공간 자체가 분리(크래시 봉쇄의 근원) |
| 버퍼풀 내 지분 | **private LRU 리스트 + quota** — 트랜잭션/세션별 최근 페이지가 남의 리스트를 밀어내지 않음 | page_buffer.c (src확인) | "공유 모듈 내부에 격리를 심은" 모범 사례 — 재설계의 일반 원칙으로 승격 |

원칙 P1: **격리 가능한 것은 전부 격리한다.** 공유는 비용이므로, 공유 도메인에 남는 것은 "물리적으로 하나일 수밖에 없는 것"뿐이어야 한다.

### 13.2 공유 도메인 — 경합 등급별 지도

| 등급 | 자원 | 성격 | 완화 기법 |
|---|---|---|---|
| **S (직렬화 지점)** | WAL append 포인트 | 전 DML이 통과하는 단일 순서 결정점 | 임계구역 최소화, group commit, (장기) 로그 파이프라인화 |
| **A (고경합 핫)** | 핫 페이지 latch(B+tree 상위·우측 리프, 카탈로그 페이지), 락 테이블 버킷, LRU victim 탐색, MVCC 활성 테이블(스냅샷 획득/커밋 갱신), temp 공간 할당자, serial 객체 | 다수 쓰기 경쟁 | 파티셔닝, lock-free 맵, victim 후보 분산(zone3), 스냅샷 O(1)화(11.3), serial 캐시 발급 |
| **B (read-mostly)** | XASL 플랜 캐시, filter pred 캐시, 카탈로그/통계, 클래스 표현 캐시(heap), 세션 테이블, ACL | 읽기 지배 + 드문 무효화 | RW 분리, 버전 스왑(무효화 시 포인터 교체), DDL 전파 순서 규약(11.4) |
| **C (백그라운드 공유)** | vacuum 작업 데이터·워커, flush/checkpoint 데몬, 데드락 감지기, px 전역 워커풀, perfmon 집계 | 포그라운드와 자원 경쟁 | I/O·CPU 예산제(스로틀), 포그라운드 우선 백오프, per-thread 카운터 후 집계 |

원칙 P2: 공유 도메인의 각 항목은 **"보호 대상 불변식 + 허용 동시성 수준 + 완화 기법"** 3요소가 명세된 계약서를 가져야 한다. (예: "락 테이블 버킷 — 불변식: 소유자/대기자 리스트 정합. 동시성: 버킷 단위 병행. 기법: 해시 파티셔닝 N=코어수×k")

### 13.3 경계 프로토콜 — 격리가 공유로 진입하는 문

격리 도메인의 코드는 아래 관문을 통해서만 공유 자원을 만진다. 관문 규약이 곧 동시성 정확성의 전부다.

```
버퍼풀 관문 : pgbuf_fix(latch등급) ↔ pgbuf_unfix — 짝 맞춤 강제, latch 순서(부모→자식)
락 관문     : lock_object(계층: DB→테이블 인텐트→행) — 트랜잭션 종료 일괄 해제
로그 관문   : log_append(undo/redo) — 페이지 수정과 원자적 순서(WAL 규칙)
메모리 관문 : 공유 풀 할당(temp 페이지, 리스트파일 페이지) — quota/grant 경유
스케줄 관문 : 워커풀 태스크 제출 — 예산(포그라운드/백그라운드/px) 분리
```

원칙 P3: 관문 밖에서 공유 상태를 직접 만지는 코드는 존재해서는 안 된다(정적 검사·트래커로 강제). 제1부 [G] 스토리지 계층의 "file_io 직접 접근 금지"가 이 원칙의 스토리지판이다.

### 13.4 (c) 쿼리 내 병렬의 협조적 공유 — px의 별도 규약

| px 공유물 | 공유 방식 | 경합과의 차이 |
|---|---|---|
| 작업 분배 큐 | `px_thread_safe_queue`(생산자=코디네이터, 소비자=워커) | 경쟁이 아니라 분배 — 큐 비움이 목표 |
| 스캔 범위 | 페이지 구간 단위 할당 | 스큐 시 재분배(훔치기) 설계 필요 |
| 해시테이블(build측) | 워커 공동 구축 | 파티션별 소유로 잠금 회피가 정석 |
| 병렬도 예산 | 전역 worker manager가 쿼리 간 배분(src확인: global/per-query 분리) | (b)의 자원 조정 문제로 승격 — 13.5의 X3 |
| 인터럽트 | `px_interrupt` — 취소 전파 | 부분 실패 시 전 워커 정리 계약 |

원칙 P4: px 워커는 트랜잭션 문맥(스냅샷·락 소유)을 **코디네이터와 공유**하되 THREAD_ENTRY·프라이빗 힙은 각자 소유 — "트랜잭션은 하나, 스레드 문맥은 여럿"의 이중 구조를 명문화할 것.

### 13.5 간섭 시나리오 카탈로그 (설계 단계에서 이름 붙여두는 사고들)

| # | 시나리오 | 메커니즘 | 방어 설계 | 검증(6장) |
|---|---|---|---|---|
| X1 | 롱쿼리가 vacuum 지평선 고정 | 최저 활성 MVCCID 정체 → 죽은 버전 누적 → 힙/인덱스 비대 | 지평선 나이 관측지표화, 스냅샷 등급(문장 스냅샷 권장) | W4 |
| X2 | 대량 스캔의 버퍼 오염 | 스캔 페이지가 핫셋 축출 | zone/private LRU 배정 규칙(12.2) | W4 |
| X3 | px의 OLTP 워커 기아 | 병렬 워커가 풀 점유 | 워커 예산 분리 + 병렬도 상한 동적화 | W4 |
| X4 | DDL의 캐시 무효화 폭풍 | 플랜/스키마 캐시 일괄 무효화 → 재컴파일 쇄도 | 버전 스왑 + 점진 재프리페어 | W1+DDL |
| X5 | 체크포인트/flush 폭주 | dirty 일괄 flush가 포그라운드 I/O 잠식 | 증분 체크포인트, flush 스로틀 | W1 지속부하 |
| X6 | 커밋 폭주 시 fsync 세례 | group commit 미작동 구간 | 배치율 지표 + 대기/깨움 설계(11.1) | W1 |
| X7 | 작업메모리 곱셈 폭발 | sort buffer × 동시 클라이언트 | 전역 예산+grant(12.3) | W3×동시 |
| X8 | 핫 시퀀스/우측 리프 | 단조 키 삽입 집중 | 11.5 기법 | W1 삽입형 |
| X9 | 백그라운드 vacuum I/O 간섭 | 회수 I/O가 포그라운드와 충돌 | C등급 예산제(13.2) | W6 소크 |

---

## 14. 유저 도메인 시나리오와 검증 설계

### 14.1 워크로드 아키타입 (유저 페르소나)

| ID | 아키타입 | 대표 유저 스토리 | 지배 모듈 |
|---|---|---|---|
| W1 | 웹 OLTP (게시판·커머스 주문) | "점심 피크에 초당 수천 주문, 응답 100ms 체감 유지" | A, D, F, G-1/4/5 |
| W2 | 정산·결제 (강일관·감사) | "장애 후 재기동해도 승인된 결제는 단 1건도 증발 금지" | F-3/4, G-2(DWB) |
| W3 | 야간 배치·리포팅 | "새벽 4시간 창 안에 일 마감 집계 완료" | B, E-5/6/7, G-6 |
| W4 | HTAP 혼합 | "운영 중 실시간 대시보드 — 주문 지연 없이" | X1~X3 전부, G-1 |
| W5 | 대량 적재·이관 | "타 DB에서 수억 행 이관 + 인덱스 재구축을 주말 내" | I-5(loaddb), px INDEX_BUILD, F-3 |
| W6 | 상시 운영·HA | "패치·장애 시 수십 초 내 절체, 평시 복제 지연 체감 0" | F-8, D-4, F-4 |
| W7 | 멀티테넌트·샤드 | "테넌트 하나의 폭주가 이웃을 침범 금지" | A-4, A-5, 5.2 예산제 |

### 14.2 시나리오별 테스트·합격 기준 매트릭스

합격 기준은 절대 수치가 아니라 **상대·구조 기준**으로 명세한다(절대치는 기준 하드웨어에서 캘리브레이션 후 고정).

| 시나리오 | 테스트 방법 | 핵심 SLI | 합격 기준(형태) |
|---|---|---|---|
| W1 정상부하 | sysbench oltp_read_write / oltp_point_select, TPC-C류(tpmC), YCSB A/B | p50/p99/p999, TPS, 커밋 배치율 | p99 ≤ k×p50(k 고정, 예: 5) 유지, 코어 2배 시 TPS ≥ 1.7배(스케일링 커브 제출), 배치율 ≥ 목표 |
| W1 접속폭주 | 초당 수천 connect/disconnect 폭주기 | 접속 수립 p99, CAS 재사용률, 거절율 | CAS 풀 소진 시 "지연 증가"로 degrade(에러 폭발 금지), 재사용률 ≥ 목표 |
| W1 핫스팟 | 단조 PK 삽입 100%, serial 경쟁, 단일 행 카운터 | 삽입 TPS vs 스레드 수 | 스레드 증가 구간에서 TPS 역전(붕괴) 없음 |
| W2 내구성 | 부하 중 kill -9 / 전원단절 에뮬 / torn-page 주입(base/fault_injection 활용, DWB 검증) | 커밋-확인 트랜잭션 생존율, 복구 성공률 | 생존율 100%(예외 0), torn 주입 100% 복원 |
| W2 복구시간 | 체크포인트 간격별 크래시→재기동 | RTO | RTO가 "마지막 체크포인트 이후 로그량"에 선형(계수 문서화), 병렬 redo 배율 ≥ 목표 |
| W2 격리정합 | Hermitage류 격리 시나리오 뱅크, 데드락 폭풍 | 이상현상 검출 수, 데드락 해소 시간 | 선언 격리수준에서 금지 이상현상 0, 데드락은 감지주기+ε 내 100% 해소 |
| W3 단독 | TPC-H/DS류 쿼리셋 | 쿼리 경과, 스필 바이트, 병렬 효율 | 병렬도 d에서 speedup ≥ α·d(α 문서화), 메모리 2배 시 스필 단조 감소 |
| W4 혼합 | CH-benCHmark류(TPC-C + 분석 동시) | OLTP p99 열화율, 지평선 나이, 히트율 변화 | 분석 가동 시 OLTP p99 열화 ≤ β%(예: 15), 지평선 나이 상한 유지, 핫셋 히트율 하락 ≤ γ%p |
| W5 적재 | loaddb + 병렬 인덱스 빌드 | 적재 MB/s, 빌드 시간, 동시 OLTP 영향 | 적재 중 W1 p99 열화 ≤ 임계, no-logging 경로 검증(크래시 시 재실행 안전) |
| W6 HA | 부하 중 강제 절체 반복, 장기 소크(24~72h) | 절체 시간, 복제 지연, 자원 누수 | 절체 ≤ 목표초 100회 연속, 지연 정상수렴, RSS/FD/temp 사용량 정상상태(우상향 금지), vacuum 적체 0 수렴 |
| W7 격리 | 테넌트별 폭주 주입 | 이웃 테넌트 p99 | 폭주 테넌트 외 p99 열화 ≤ 임계(예산제 증명) |

### 14.3 모듈 단위 마이크로벤치 (스케일링 커브 제출 의무)

각 항목은 "1스레드 절대치 + 1→N스레드 커브"를 산출물로 한다. 커브의 무릎(knee)이 곧 13.2 등급 판정의 실측 근거다.

| 대상(공유 도메인) | 마이크로벤치 | 소유 영역 |
|---|---|---|
| WAL append | 고정 크기 레코드 append/s, fsync 배치율 | F-3 |
| 버퍼풀 | fix/unfix ops/s(히트 100%), victim 탐색 지연, 오염 시나리오 히트율 | G-1 |
| B+tree | 포인트 탐색/삽입 ops/s, 우측 리프 집중 삽입 | G-5 |
| 락 매니저 | acquire/release ops/s, 충돌율별 커브 | F-1 |
| 스냅샷 | 활성 트랜잭션 수 10~10⁴별 획득 지연(11.3 검증) | F-2 |
| 플랜 캐시 | 적중 조회/s, 무효화 폭풍 재컴파일 소요 | B/E-6 |
| px 큐/풀 | 태스크 분배 처리량, 스큐 시 완료편차 | E-7 |
| temp 할당 | 동시 스필 시 할당 지연 | G-3/E-6 |

### 14.4 정확성 계열(성능과 분리 집행)
- 격리·가시성: 스냅샷 경계 케이스 뱅크(자기갱신 가시성, 삽입-삭제 교차, 인덱스-힙 정합).
- 크래시 일관성: 임의 시점 kill 매트릭스(로그 flush 전/중/후 × 페이지 flush 전/중/후), 복구 후 checksumdb/체크 유틸 통과.
- 결정 규약 검증: 5.3 관문 짝맞춤(fix/unfix, alloc/free) 누수 0 — 자원 트래커를 릴리스 빌드 테스트에서도 주기 가동.

### 14.5 회귀 게이트 (재설계 기간의 안전벨트)
- 성능 회귀: 6.2/6.3 전 지표에 대해 기준선 대비 허용 편차(예: 지연 +5%, 처리량 −3%) 초과 시 머지 차단.
- 플랜 회귀: 고정 스키마+통계 스냅샷에서 쿼리셋의 플랜 지문(연산자 트리 해시) diff — 변경은 명시 승인제.
- 장기 게이트: 소크에서 "정상상태 수렴"(vacuum 적체, temp, RSS) 미달 시 차단.

### 14.6 기존 테스트 자산의 승계
- 저장소 내: unit_tests/(Catch2), CI의 SQL 테스트(10병렬)·shell 테스트(50병렬)(src확인: 루트 문서), base/fault_injection.
- 생태계: CTP(cubrid-testtools)와 cubrid-testcases 뱅크 — 재설계 시 14.2 매트릭스의 W1/W2 정합 케이스를 이 뱅크 위에 매핑해 재작성 비용을 최소화한다.

---

## 15. 종합 — 한 장 요약

```
             OLTP 지배 ◀──────────────────────────▶ OLAP 지배
공유 도메인   WAL append · 락테이블 · 스냅샷 ·     px 전역 워커풀 · temp 할당 ·
(경합 설계)   핫 latch · 플랜캐시 무효화            통계 갱신 · 스필 예산
              └ 기법: 배치(group commit)·파티셔닝·  └ 기법: 예산제·grant·
                lock-free·버전스왑                    zone 격리·스로틀
격리 도메인   CAS 프로세스 · THREAD_ENTRY ·         쿼리 실행상태 · sort run ·
(비용 설계)   private heap · private LRU quota       리스트파일 내용 · px 워커 문맥
              └ 기법: 지역성·수명 트래킹             └ 기법: 대용량 할당 경로 최적화
검증          W1·W2 (+X4~X6·X8)                     W3·W5 (+X2·X3·X7)
교차 전장     G-1 버퍼풀 · G-4 힙 · E-8 Vacuum  →  W4 HTAP + X1~X3가 최종 시험대
```

설계 순서 권고: ① 13.2 공유 도메인 계약서 작성 → ② 14.3 마이크로벤치로 현행 무릎 실측 → ③ 11·12장 기법을 무릎이 낮은 순서로 적용 → ④ 14.2 시나리오 게이트로 봉인. 이 루프가 "사전 고려"를 "검증 가능한 설계"로 바꾸는 절차다.

---

## 16. 참고 원천

- 소스: `github.com/CUBRID/cubrid` develop 브랜치 — 특히 각 디렉터리의 `AGENTS.md`(모듈별 아키텍처 요약이 저장소에 내장되어 있음: 루트, `src/`, `src/storage/`, `src/transaction/`, `src/query/`, `src/broker/`, `src/parser/`, `src/optimizer/`, `src/connection/`, `src/communication/` 등), `src/storage/docs/*.md`(버퍼/힙/B+tree/카탈로그 심화 문서)
- 매뉴얼: cubrid.org/manual — Introduction(프로세스·볼륨 구조), Database Management(볼륨/로그/DWB), System Parameters(client/server 파라미터 구분)
- 비교 배경: PostgreSQL 공식 문서(아키텍처/WAL/Vacuum), Oracle Database Concepts(메모리·프로세스 구조)
- 제2부(9~15장) 검증에 추가로 직접 확인한 소스: `src/storage/page_buffer.c`(private/shared LRU·zone 1/2/3·quota 구조), `src/query/parallel/`(px 프레임워크 파일 구성), `src/base/system_parameter.c`(`group_commit_interval_in_msecs`, `async_commit`), `conf/cubrid.conf`(sort buffer × max_clients 상한 주석)
