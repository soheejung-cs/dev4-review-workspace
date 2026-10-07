---
name: sql-difftest
description: 적대적 SQL 차분 테스트 — 같은 SQL 세트를 develop·PR·(머지) 빌드에 돌려 출력 차이로 결함·스펙 변경을 찾는다. 바인드 타입 회전·재사용·쓰레기 값·다중 세션 공유 플랜·시스템 파라미터 모드 on/off·세션 파라미터·실행 사이 스키마 변경·수명/메모리. "이거 develop 이랑 결과 같아?", "모드 켜고 끄면서 테스트해", "적대적으로 깨 봐", 타입·도메인·플랜 캐시·실행기 PR 리뷰에.
---

# 적대적 SQL 차분 테스트 (sql-difftest)

리뷰 대상 PR 의 답이 **develop 과 같은가**를 코드가 아니라 출력으로 확인한다. 같지 않은 곳이 지적의 재료이고, 같은 곳은 "검증했다" 의 근거다. 2026-10-07 PR#8022(바인드 도메인 해석) 리뷰에서 세트 A~D14 로 자리를 잡았다(`claude-workspace/projects/CBRD-27510/도메인해석-난이도테스트-20261007.md`).

도구: **`tools/difftest/`** — `difftest.sh`(여러 설치본 SA 실행·정규화·diff), `modes.sh`(파라미터 모드 묶음), `concurrent.sh`(동시 vs 단독 vs 재기동), `lifetime.sh`(RSS·plandump), `fuzz_gen.py`(무작위 바인드 생성기).

## 0. 준비 — 빌드 세 개, 볼륨 하나 또는 셋
- **기준 빌드**: PR 의 merge-base 를 품은 develop 리비전(비교 축의 파일이 그 사이에 바뀌지 않았는지 `git log <base>..<mb> -- <files>` 로 확인). **PR 빌드**: PR 헤드. 내 변경이 얹힌 **머지 빌드**가 있으면 셋째 — "내 변경이 PR 의 답을 바꾼 곳 없음" 을 함께 증명한다.
- 설치본마다 `databases/` 가 따로다. 값 비교는 설치본별 볼륨으로 충분하지만, **비용·플랜 수치 비교는 `-v` 로 같은 볼륨**을 써야 한다(힙 페이지 수가 다르면 비용이 어긋난다).
- SA(`csql -S`)는 떠 있는 서버와 충돌한다 → 먼저 `cubrid service stop`. **빌드가 끝나면 goto 가 그 설치본의 demodb 서버를 자동으로 띄운다**(2026-10-07 실측) — 빌드 직후엔 반드시 확인.
- csql 은 cwd 에 `csql.err` 를 남긴다 — 세트·출력은 스크래치패드나 `projects/<이슈>/repro/` 에서.

## 1. 공격 축 — 한 세트에 하나씩, 실행 순서를 바꿔 가며
| 축 | 어떻게 | 2026-10-07 에 걸린 것 |
|---|---|---|
| **바인드 타입 회전** | 같은 PREPARE 를 INT → 문자열 → DOUBLE → INT → NULL → INT 로 재실행 | develop 의 재사용 타입 잔존(PRIOR 식 반올림·UNION 바인드 열·PERCENTILE_DISC·윈도우 정렬) |
| **같은 플랜, 다른 세션** | 같은 문장을 K 세션이 바인드 타입·키 범위만 달리 동시 실행 → 단독·재기동 단독과 diff | develop 은 "누가 먼저 컴파일했나" 로 답이 바뀜(세션 간 타입 누출) |
| **쓰레기·중의 값** | `'1.5.5'`, `' 12 '`, `'0x1F'`, `''`, `'NaN'`, `'일이삼'`, 300자, `B'1010'`, 날짜 리터럴, bigint 경계 — `fuzz_gen.py` | 윈도우가 변환 실패 행을 조용히 떨어뜨림(develop), BETWEEN 조용히 0행 → 오류(PR 스펙 변경) |
| **해석 근거 없는 자리** | `?` 전부 NULL, `? + ?`, `NVL(?, ?)`, 바인드만 있는 UNION 열, `SUM(?)` | 내부 센티널(-1383) 사용자 노출 여부 |
| **안 타는 가지 / 상수와 섞기** | `CASE WHEN a > ? THEN 1/? ELSE 0 END`, `NVL(a, ?)` 에 NULL 행 없음, `(PRIOR w) * ? + w * 2 - ? + 0.5` | 행 의존 가지 안 상수식 선계산(PR 회귀), 변환은 미리 안 함 |
| **부작용 있는 상수** | 가지 안 `시리얼.NEXT_VALUE + ?`, `RANDOM() + ?`, `SYS_GUID()`, `SYSDATE + ?` — 값 대신 **횟수·DISTINCT 수·범위** 를 본다 | 선계산되면 시리얼이 한 번만 소비됨(이번엔 정상) |
| **구조 조합** | 뷰(식 푸시다운·세션변수 읽는 뷰·GROUP BY/UNION 뷰), 계층 뷰 위 윈도우, 윈도우 뷰 위 CONNECT BY, 3단 서브쿼리, CTE+EXISTS, MERGE(DELETE 절·GROUP BY/윈도우 소스), INSERT…SELECT, 다중 VALUES, ON DUPLICATE KEY, REPLACE | **CONNECT BY + `?` 가진 파생 테이블 → 로드 -1383**(PR 결함) |
| **재컴파일 경로** | 같은 문장을 (a) 새 PREPARE→EXECUTE (b) `/*+ RECOMPILE */` 힌트 (c) PREPARE → 그 테이블 `ALTER ADD COLUMN`/`CREATE INDEX` → EXECUTE (d) `max_plan_cache_entries=0` 로 돌려 비교. (b)(c)는 **바인드 값을 아는 상태의 재컴파일**이라 첫 PREPARE 와 다른 스트림이 간다 | PR#8022 `UPDATE SET d = DATE'…' + ?` 가 (b)(c)에서만 -1383 (2026-10-07) |
| **리터럴 단독** | 바인드 없는 리터럴 문장(`SUM(NULL)`, `CAST(NULL AS …)` 인자)도 세트에 넣고 **raw diff 줄수**를 본다 — 짝 분류기는 리터럴 단독 diff 를 못 잡는다 | PR#8022 `SUM(NULL)` -1383 (2026-10-07) |
| **모드 on/off** | `modes.sh`: 기본 / oracle 호환 / concat·escape·normalization·string_max / `max_plan_cache_entries=0` / `max_plan_cache_clones=0` (+ `review-testing` §1-1 의 신호별 모드) | 캐시 0 에서 원인 오류 대신 -1383, query 인자 빈 문자열 |
| **세션 파라미터** | `SET NAMES`, `SET SYSTEM PARAMETERS`, 세션변수 `@v` 타입 바꿈(INT→'3'→2.5→NULL), 같은 PREPARE 유지 | A5(세션변수 타입 고정) 적용 범위 |
| **실행 사이 스키마 변경** | 같은 PREPARE 두고 `ALTER TABLE MODIFY` 타입 변경·`CREATE INDEX`·`ADD COLUMN`·`UPDATE STATISTICS` | 재실행이 새 열 타입을 따라가나 |
| **특수 타입** | ENUM·SET·JSON·BIT VARYING·DATE/TIME/TIMESTAMP/DATETIME 교차, 날짜 산술, 인덱스 키 변환(10 / 10.0 / '10' / ' 10' / '1e1'), 바인드 1000개 IN, `LIMIT ?, ?` | 0 diff |
| **수명·메모리** | `lifetime.sh`: 문장 1000개 PREPARE/EXECUTE×4/DEALLOCATE + 고유 문장 200개 × 6라운드, 오류 경로 600회 × 3 | RSS 1라운드 뒤 고정, plandump churn 정상 |
| **PL/Java** | `loadjava` + `CREATE FUNCTION` + PREPARE 로 인자 타입 확인 함수(`BigDecimal.scale()` 등) | 0 diff. `int` 원시 인자에 NULL 은 내 설계 오류(IllegalArgumentException) |

## 2. 절차
1. **스키마 하나를 고정**한다(예: `t1` 8행 — INT·BIGINT·SHORT·FLOAT·DOUBLE·NUMERIC·MONETARY·CHAR·VARCHAR·BIT·DATE·TIME·TIMESTAMP·DATETIME·ENUM·JSON·SET·SEQUENCE + NULL 행 + 그룹 열). 세트 파일은 축 하나에 하나, 문장마다 `EXECUTE` 4~6회(타입 회전 + 재사용 + NULL).
2. `difftest.sh -o <out> -s <세트> dev=<기준> pr=<PR> [mg=<머지>]` → 세트별 `*_dev-pr.diff`. 머지 빌드는 PR 과 0 diff 여야 한다(난수 값 제외).
3. **diff 를 분류**한다 — 이게 산출물이다.
   - **PR 결함**: develop 정상 / PR 오류·오답, 재현이 바인드 값과 무관하면 🔴. 소스에서 raise 지점을 찾아 조건까지 적는다(예: `domain_plan_validate()` 엄격 검사).
   - **스펙 변경**: 둘 다 설명 가능한데 답이 다름(조용히 0행 → 오류, 결과 타입 변경). 🟡 "스펙 목록에 적어 달라".
   - **develop 결함을 PR 이 고침**: 리터럴·새 PREPARE 로 정답을 확인해 PR 이 맞음을 증명. ✅ — **게시는 사용자가 정한다**(2026-10-07: "8022 잘못인 것만 게시"). 문서에는 전부 남긴다.
   - **테스트 설계 오류**: 문법 미지원(`ROWS BETWEEN ? PRECEDING`, `SYS_CONNECT_BY_PATH(nm, ?)`, `PRIOR LEVEL`, `ORDER BY ?`, `GROUP BY ?`), csql 이 `EXECUTE … USING` 에 SET 리터럴을 못 받음, `int` 원시 인자 NULL, 같은 INSERT 반복의 유일키 위반. 고쳐서 다시 돌린다.
   - **비결정**: 난수·OID·시간 — 정규화하거나 성질(횟수·범위)로 비교.
4. **좁힌다**: 걸린 문장 하나를 변형 8~12개로(한 요소씩 빼 보기) — "되는 모양" 목록이 원인 지목과 TC 대조군이 된다.
5. 동시성은 `concurrent.sh`, 모드는 `modes.sh`, 수명은 `lifetime.sh`. 세 가지 모두 **기준도 같은 방식으로** 돌려 develop 의 선재 결함과 가른다(동시 실행은 재기동 단독이 기준 — 캐시가 남은 단독은 이미 오염돼 있다).
6. 기록: 세트·출력·diff 는 `projects/<이슈>/repro/`, 결과는 `도메인해석-난이도테스트-<날짜>.md` 꼴의 표(축 / 재현 / develop / PR / 판정). 게시는 `review-response` 규약(초안 → 승인 → 인라인 또는 요약 코멘트, "왜 문제가 되는지" 포함).

## 3. 함정 (전부 실제로 데인 것, 2026-10-07)
- `csql -i -` 미지원 → 임시 파일. "Too many prepared statements" → 문장마다 `DEALLOCATE PREPARE`.
- 바인드 수 불일치는 조용히 다른 오류로 보인다 — 생성기가 `?` 를 세게 한다.
- 결과 블록 비교는 **라인 번호를 정규화**하면 매핑이 깨진다 — 문장별 비교는 "Result … Line N" 헤더 기준으로 블록을 잘라 쌍으로 비교한다.
- `grep -n 패턴 $f` 에서 `$f` 가 비면 stdin 을 기다리며 **영원히 멈춘다**(백그라운드 태스크가 그렇게 죽었다).
- `pgrep -f` 금지(자기 명령줄에 매칭). 서버 생존은 `pgrep -x cub_server` + `/proc/<pid>/exe` 로 어느 설치본인지 본다.
- 공유 표에 누적하는 DML 은 동시 vs 단독 비교에서 그 블록을 뺀다. 세션마다 키 범위를 다르게.
- 모드 적용 여부는 출력으로 확인한다(`paramdump` 에 안 나오는 파라미터가 있다). `compat_numeric_division_scale` 은 deprecated.
- develop 의 재사용 결함 때문에 **단독 실행도 앞 문장 순서에 오염**된다 — "첫 실행이 다르다" 가 보이면 새 PREPARE 로 리터럴 대조부터.
