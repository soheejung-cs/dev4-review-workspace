# PR Body Tone Guide

PR 본문을 작성할 때 반드시 따르는 문체 기준. 기준점은 **PR #6911** — 아래 예제 4가 이 스타일의 정수다.
원천 자료: [`pr-corpus/`](./pr-corpus/) (작성자의 hand-written PR 76개, #4432–#6911).

## 골격

```markdown
<http://jira.cubrid.org/browse/CBRD-XXXXX>

### Purpose

(한국어 산문. 왜 + 무엇을 바꾸는지. 소형 수정은 1문장, 대형 변경도 최대 3문단.)

### Implementation

(선택 섹션. 파일/함수 단위로 무엇을 바꿨는지.)

### Remarks

(예외 섹션. 원칙적으로 쓰지 않는다.)
```

- `### Purpose` — **항상 필수**. 배경(기존 방식과 문제점) → 개선 방향 순의 산문.
- `### Implementation` — **선택**. 변경이 여러 파일/모듈에 걸치거나, 새 파일·구조가 생기거나, Purpose만으로 코드 리뷰 진입이 어려울 때만. 한두 함수 수정 수준이면 생략한다.
- `### Remarks` — 리뷰어가 반드시 알아야 할 제약/후속 작업이 있을 때만 예외적으로.
- **Purpose 한 문장짜리 6줄 PR이 정상이다.** 억지로 섹션을 채우지 않는다.

## 규칙

1. **짧게.** 소형 수정(한두 함수)은 Purpose 1~3문장. 대형 구조 변경도 #6911(약 20줄)을 넘지 않는다.
2. **합니다체** 기본. ("~합니다", "~개선합니다", "~수정합니다")
3. **필러 금지.** "본 PR은...", "전반적으로", "필요에 따라", "다양한", "효과적으로", "안정성 개선" 같은 말을 쓰지 않는다. 사실만 쓴다.
4. **코드 식별자는 영어 그대로.** 함수/파일/매크로/브랜치명은 백틱 또는 원문 표기. 번역·의역하지 않는다.
5. **용어 gloss는 필요할 때만** 괄호 한 구절로: `PART_FTAB(일부 사용 중인 섹터)`, `Partial Sector (64페이지 단위)`. 모든 내부 용어에 강제하지 않는다.
6. **diff에 없는 내용을 쓰지 않는다.** 추측, 과장, 성능 수치 홍보 금지. 측정하지 않은 효과를 주장하지 않는다.
7. **Implementation은 파일/함수 단위.** `파일명`: 무엇을 어떻게 바꿨는지 한 문장씩. 번호 목록 또는 파일별 나열.

## 셀프 체크 (제출 전)

- Purpose가 3문단을 넘으면 줄인다.
- diff에 없는 내용이 한 줄이라도 있으면 지운다.
- crash fix 한 줄짜리 수정인데 본문이 10줄이면 잘못 쓴 것이다.

## 예제 (원문 그대로)

### 예제 1 — 소형 crash fix: Purpose 한 문장 (#6900, +6/-0)

```markdown
<http://jira.cubrid.org/browse/CBRD-26596>

### Purpose

qlist_count 관련 core dump가 발생해 이를 수정합니다.
```

### 예제 2 — 소형 동작 변경: Purpose 한 문장 (#6682, +2/-1)

```markdown
<http://jira.cubrid.org/browse/CBRD-26413>

### Purpose

select 대상에 lock이 필요한 테이블에 대하여, memoize를 비활성화한다.
```

### 예제 3 — 중형 개선: Purpose + Implementation (#6308, +28/-1)

```markdown
<http://jira.cubrid.org/browse/CBRD-26171>

### Purpose

이전 heap scan 개선 작업([CBRD-25037](http://jira.cubrid.org/browse/CBRD-25037))에서 mvcc_snapshot을 간단히 확인하는 코드를 추가했으나, parallel query 실행 중 snapshot_fnc의 연산 부하가 큰 문제가 확인되었습니다. 
이에 따라 mvcc_snapshot을 보다 간단히 확인할 수 있도록 개선합니다.
현재 snapshot_fnc 내부에는 INS_ID와 DEL_ID를 확인하여 모든 트랜잭션에서 조회 가능한 row에 대해 MVCC 연산을 최소화하는 로직이 포함되어 있습니다. 
그러나 snapshot_fnc가 함수 포인터로 선언되어 있어, 컴파일러 최적화가 제한되며 성능 저하가 발생합니다.
이에 따라, 모든 트랜잭션에서 조회 가능한 row 여부를 heap_scan_get_visible_version()에서 사전에 판단하여, snapshot_fnc 호출 자체를 생략함으로써 연산 부하를 줄이는 방향으로 개선합니다.

### Implementation

1. heap scan 시 mvcc 헤더를 참조하여 모든 transaction에서 조회 가능한 row인 경우 즉시 값을 반환하게끔 변경합니다.
```

### 예제 4 — 대형 구조 변경: 이 톤이 기준이다 (#6911, +515/-229)

```markdown
<http://jira.cubrid.org/browse/CBRD-26615>

### Purpose

Parallel Heap Scan 수행 시 발생하는 I/O 병목 현상을 해소하기 위해 스캔 분배 방식을 최적화합니다.

기존 방식은 전역 Mutex를 사용하여 page_next 로직을 통해 페이지를 하나씩 순차적으로 할당받는 구조였습니다. 이로 인해 스레드가 페이지를 할당받고 I/O를 수행하는 과정에서 동기화 호출이 잦아지고, 사실상 I/O를 기다리는 시점이 직렬화되어 다중 스레드의 이점을 충분히 활용하지 못하는 병목이 존재했습니다.

이를 개선하기 위해, 스캔 시작 단계에서 Heap File의 헤더를 한 번만 읽어 전체 데이터가 포함된 Partial Sector (64페이지 단위) 정보를 미리 수집(Fetch)합니다. 수집된 섹션 정보들을 워커 스레드들에게 미리 나누어 할당함으로써, 각 스레드가 자신에게 할당된 섹터 범위를 독립적으로 순회(Iteration)하며 별도의 락 없이 병렬적으로 I/O를 발생시키고 데이터를 처리할 수 있도록 구조를 개선합니다.

### Implementation

Storage - File Manager (src/storage/)

`file_manager.c`: file_get_all_data_sectors 함수를 추가하여 힙 파일의 PART_FTAB(일부 사용 중인 섹터)과 FULL_FTAB(꽉 찬 섹터) 정보를 모두 순회하며 실제 데이터가 포함된 섹터 정보를 FILE_FTAB_COLLECTOR로 수집하도록 구현했습니다.
`file_manager.h`: 섹터 수집을 위한 구조체 및 비트맵 매크로(FILE_FULL_PAGE_BITMAP 등)와 외부 인터페이스를 선언했습니다.

`px_heap_scan_input_handler_ftabs.cpp:`
init_on_main: 메인 스레드에서 file_get_all_data_sectors를 호출하여 전체 섹션 위치를 파악하고 병렬도에 맞춰 분할합니다.
get_next_vpid_with_fix: 할당된 섹터 내의 비트맵을 로컬 스레드에서 직접 확인하며 페이지를 pgbuf_fix 하도록 수정하여, 전역 Mutex 없이도 독립적인 I/O 발생이 가능하도록 로직을 변경했습니다.
```
