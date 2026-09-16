# C/C++ 성능 규칙집 (자립형)

> **용도:** AI 에이전트 및 사람이 C/C++ 코드를 작성·리뷰할 때 직접 적용하는 규칙집.
> 외부 링크·참조 없이 이 파일 하나로 판단이 가능하도록 작성.
> 각 규칙은 ID로 인용 가능 (`MEM-03 위반` 형태).
> 대상: DB 엔진, 스토리지, 쿼리 처리, 시스템 레벨 코드.

---

## 사용 규약 (AI 에이전트용)

1. **최적화 제안 전 반드시 병목 근거를 요구한다.** 측정 데이터가 없으면 "측정 먼저"라고 답한다. 추측 기반 최적화 제안은 금지.
2. **핫패스와 콜드패스를 먼저 구분한다.** 콜드패스(에러 처리, 초기화, 종료)에는 이 규칙집을 적용하지 않는다. 가독성이 우선.
3. **규칙 충돌 시 우선순위:** 정확성 > 메모리 안전성 > 가독성 > 성능. 성능을 위해 UB를 도입하는 제안은 금지.
4. **정량 근거를 함께 제시한다.** "빠르다"가 아니라 "캐시 미스 N회 감소" 또는 "분기 M개 제거" 형태로.
5. **수치 임계값은 기준선이다.** 아키텍처·워크로드에 따라 다르므로 실측으로 조정한다고 명시한다.
6. **C 코드에는 C 규칙을 적용한다.** 모던 C++ 관용구(`shared_ptr`, RAII, 템플릿)를 C 코드베이스에
   제안하지 않는다. C에서는 ALIAS/CLOW 절이 CPP 절을 대체한다.
7. **이식성 없는 기법은 표시한다.** GCC/Clang 확장(computed goto, `__builtin_*`, `__attribute__`)을
   제안할 때는 폴백 경로를 함께 제시한다.
8. **약한 메모리 모델을 잊지 않는다.** x86에서 동작하는 락프리 코드가 ARM64에서 깨지는 것은
   흔한 일이다. 원자적 연산 관련 제안에는 COH-10을 함께 언급한다.

### 이 저장소(CUBRID)에서 반복 적발된 축

성능 경로 코드를 쓰거나 리뷰할 때 **18장 체크리스트를 먼저 훑고**, 아래 항목은 특히 우선 확인한다.

| 축 | 규칙 | 실제 사례 |
|---|---|---|
| 행 루프에 남은 **컴파일 타임 분기** | BR-04, A59 | 식 컴파일에서 `step->aux ? A : B`, `step->domain == NULL` 테스트가 행마다 반복 → 커널 특수화로 제거 |
| 행마다 **무조건 out-of-line 호출** | CC-05, A60 | `pr_clear_value()`를 고정폭 타입 슬롯에도 매 행 호출 → NULL 경로만 인라인 플래그 세팅으로 대체 |
| 술어 평가의 **다중 switch 디스패치** | BR-06, A61 | `switch(kind)` → `switch(fast_type)` → `switch(rel_op)` 3중 → (타입×연산자) 리프 함수 포인터로 컴파일 타임 확정 |
| **행마다 불변 포인터 재발행** | BR-04, A62 | 슬롯 소유 스텝이 매 행 같은 주소를 셀에 기록 → 프로그램 생성 시 1회로 |
| 직렬 경로만 최적화, **병렬 전용 루프 누락** | PAR-14, A63 | px BUILDVALUE_OPT 전용 누적 루프가 훅 밖이라 GROUP BY 없는 질의만 중립 |
| 핫 구조체 **hot/cold 미분리** | MEM-02/05 | 96B 스텝 구조체에 실행에 안 쓰는 필드(regu, 분기 범위) 동거 |
| 스레드 간 **소유권 교차 해제** | ALLOC-08, A64 | 프라이빗 힙 할당물을 다른 워커가 해제 → mspace abort |
| 고정소수점 **행당 자릿수 정리** | FP-07 | NUMERIC SUM 행당 반올림·패킹 → 지연 캐리로 마감 1회 |
| 병렬 워커 카운터 **false sharing** | MEM-03, GLOB-02 | px 워커별 통계·부분합 |
| 비용/선택도의 **부동소수점 `==`** | FP-01/02 | 플랜 비교자 비대칭 (CBRD-27139) |
| 1회 수치로 성능 판정 | MEAS-04 | 오염된 기준선 1패스로 "퇴행" 오판 → 3회 중앙값·교차검증 필수 |

---

## 0. 비용 기준선 (암기 대상)

절대값보다 **자릿수 비교**가 중요하다.

| 연산 | 대략 사이클 | 비고 |
|---|---|---|
| L1 캐시 히트 | 4~5 | 사실상 무료 |
| L2 캐시 히트 | 12~15 | |
| L3 캐시 히트 | 40~75 | 코어 공유 |
| **DRAM 접근** | **200~400** | L1의 약 100배 |
| 분기 예측 실패 | 15~20 | 파이프라인 플러시 |
| 정수 加/減/논리 | 1 | |
| 정수 곱 | 3~5 | |
| 정수 나눗셈 (64bit) | 20~100 | **매우 비쌈** |
| 부동소수 加/乘 | 4~5 | |
| 부동소수 나눗셈 | 13~20 | |
| `sqrt` | 15~20 | |
| 함수 호출 (인라인 안 됨) | 5~25 | |
| TLS 접근 (initial-exec) | 1~3 | 정적 링크 |
| TLS 접근 (global-dynamic) | 20~50 | `.so`의 `thread_local`, `__tls_get_addr` 호출 |
| 원자적 연산 (경쟁 없음) | 20~50 | |
| **원자적 연산 (경쟁 있음)** | **100~1000+** | 캐시 라인 이동 |
| 캐시 라인 무효화 (S→I 브로드캐스트) | 50~200 | 사본 보유 코어 수에 비례 |
| **HITM (다른 코어 캐시에서 전송)** | **100~300** | 코히런시 비용 중 최악. `perf c2c` |
| 컴파일러 배리어 | 0 | 명령 생성 없음. 재정렬만 방지 |
| `mfence` (seq_cst 펜스) | 30~100 | x86. acquire/release는 보통 0 |
| 정수 나눗셈 → 곱셈-시프트 치환 후 | 5~8 | CLOW-06 |
| 뮤텍스 락/언락 (경쟁 없음) | 20~50 | |
| 뮤텍스 (경쟁, 커널 진입) | 1,000~10,000+ | |
| `malloc`/`free` (소형) | 50~200 | |
| `shared_ptr` 복사 (경쟁 없음) | 20~40 | 원자적 증감 2회 |
| **`shared_ptr` 복사 (경쟁)** | **100~1,000+** | 카운터 라인이 코어 간 왕복 |
| `dynamic_cast` | 50~500 | 타입 계층 순회 |
| 예외 던지기 | 1,000~10,000+ | 정상 흐름에 쓰면 안 됨 |
| NUMA 원격 노드 접근 | 로컬의 1.5~2배 | 멀티 소켓 |
| 시스템 콜 | 500~2,000 | |
| 컨텍스트 스위치 | 1,000~10,000 | |
| SSD 랜덤 읽기 | ~50,000~500,000 | 마이크로초 단위 |
| 디스크(HDD) 시크 | ~10,000,000 | 밀리초 단위 |

**핵심 결론 3개:**
- **DRAM 한 번 접근 ≈ 정수 연산 200~400회.** 따라서 연산을 늘려 메모리 접근을 줄이는 트레이드는 거의 항상 이득이다.
- **나눗셈은 곱셈의 5~20배.** 반복 나눗셈은 역수 곱셈으로 치환한다.
- **경쟁하는 원자적 연산은 나눗셈보다 10~50배 비싸다.** 병렬화 이득을 통째로 잡아먹을 수 있다.

### 하드웨어 상수

| 항목 | 값 | 비고 |
|---|---|---|
| 캐시 라인 크기 | 64 B | x86-64, ARM64 대부분 |
| L1d 크기 | 32~48 KB / 코어 | |
| L2 크기 | 512 KB ~ 2 MB / 코어 | |
| L3 크기 | 8~256 MB / 소켓 | 공유 |
| 페이지 크기 | 4 KB (기본), 2 MB (huge) | |
| TLB 엔트리 | 수백~수천 | 초과 시 페이지 워크 |
| SIMD 폭 | 128/256/512 bit | SSE/AVX2/AVX-512 |

---

## 1. 측정 (MEAS)

### MEAS-01 — 측정 없는 최적화는 제안하지 않는다
병목의 80%는 예측이 틀린 곳에 있다. 코드를 읽고 "여기가 느릴 것 같다"는 판단은 근거로 인정하지 않는다.

### MEAS-02 — 3단 측정 절차를 따른다

```
1단계: 성격 파악 — 무엇이 병목인가 (메모리? 분기? 연산?)
  perf stat -r 10 -e cycles,instructions,cache-misses,cache-references,\
    branch-misses,branch-instructions,stalled-cycles-frontend,\
    stalled-cycles-backend ./target

2단계: 위치 파악 — 어느 함수인가
  perf record -g --call-graph dwarf ./target
  perf report --sort=overhead,symbol

3단계: 원인 파악 — 그 함수의 어느 명령인가
  perf annotate -s target_function
```

### MEAS-03 — 판정 기준선

| 지표 | 계산 | 정상 | 문제 시 원인 |
|---|---|---|---|
| IPC | instructions / cycles | > 1.5 | < 1.0 → 메모리 대기 또는 의존성 체인 |
| 캐시 미스율 | cache-misses / cache-references | < 3% | > 5% → 데이터 레이아웃 |
| 분기 실패율 | branch-misses / branch-instructions | < 1% | > 2% → 예측 불가 분기 |
| 프론트엔드 스톨 | stalled-cycles-frontend / cycles | < 10% | > 20% → I-cache 압박, 과도한 인라이닝 |
| 백엔드 스톨 | stalled-cycles-backend / cycles | < 20% | > 40% → 메모리 대기 |

### MEAS-04 — 벤치마크 위생
- 반복 실행 후 **중앙값** 비교. 평균은 이상치에 오염된다. `perf stat -r 10`.
- 최적화 대상 코드가 죽은 코드로 제거되지 않게 결과를 소비한다 (`DoNotOptimize` 패턴).
- CPU 주파수 스케일링·터보를 고정하거나, 충분히 길게 돌려 평준화한다.
- 워밍업 후 측정. 첫 실행은 캐시·페이지 폴트로 오염.
- **프로덕션과 유사한 데이터 크기로 측정한다.** 작은 데이터는 전부 L1에 들어가 캐시 문제를 숨긴다.

### MEAS-05 — 개선 후 정확성 검증
성능 변경은 반드시 기능 회귀 테스트를 통과해야 한다. 특히 부동소수점 연산 순서 변경, 병렬화, SIMD 치환은 결과가 달라질 수 있다.

### MEAS-06 — 단일 지표로 판정하지 않는다 (MEAS-03 표의 사용 규약)

MEAS-03의 임계값은 **의심 신호**이지 판정이 아니다. 지표 하나만 보고 결론을 내면 반대로 읽는다.
실증 2건 (`저지연패턴-HFT논문.md` 2.1·3.5):

| 사례 | 미스율 | 명령어 수 | 실행시간 |
|---|---|---|---|
| 캐시 cold → warm | 73.96% → 71.56% (거의 불변) | 4.93B → 12.01B (**증가**) | 267.7ms → **25.6ms** |
| 최적화 결합 | 16.0% → 33.9% (**2배 악화**) | 6.01B → **3.27B** | **최고속** |

- 첫 사례는 미스*율*이 아니라 **캐시 참조 횟수 자체**가 줄어 빨라졌다. 율만 보면 "개선 없음"으로 읽힌다.
- 둘째는 **미스율이 2배인데 가장 빠르다.** 총 명령어 수가 절반이라서다.
- 그러므로 **율(rate)과 절대량(count)을 함께 본다.** `perf stat`의 `cache-misses`와
  `cache-references`를 둘 다 기록하고, `instructions`도 같이 본다.
- 최종 판정 기준은 언제나 **wall-clock 중앙값**이다. 하드웨어 카운터는 *원인 설명*에만 쓴다.

> 이 규칙은 엔진 밖에서도 반복해 데인 실패 형태다 — 히스토그램 `buckets:300`만 보고
> "전수스캔 통계"라 판정했다가 실제로는 샘플링이었던 건(2026-08-28)이 같은 오류다.
> **보조 지표가 정상으로 보이는 것은 본질이 정상이라는 증거가 아니다.**

### MEAS-07 — 평균만이 아니라 분산(꼬리)도 지표다

DB 서버에서 사용자가 체감하는 것은 평균이 아니라 **느린 쪽 꼬리**다. 같은 평균이라도 분산이
크면 타임아웃·SLA 위반이 난다.

- 최적화 결과를 보고할 때 **중앙값과 함께 산포(MAD 또는 표준편차)를 적는다.**
  실증: 페어 트레이딩 표준편차 4,233ns → 400ns — 빨라진 것만이 아니라 **예측 가능해졌다**
  (`저지연패턴-HFT논문.md` 3.4).
- 사전 할당·고정 크기 버퍼는 평균보다 **분산을 줄이는 데 더 크게 기여**한다. 런타임 할당은
  느린 게 아니라 **가끔 아주 느린 것**이 문제다(ALLOC-01과 같은 이유).
- 분산이 큰 채로 평균만 좋아졌다면 개선이라 부르지 않는다.

---

## 2. 메모리 접근 (MEM) — 최우선 카테고리

시스템 레벨 코드에서 성능의 대부분을 결정한다. 다른 모든 카테고리보다 먼저 검토한다.

### MEM-01 — 순차 접근을 설계한다
하드웨어 프리페처는 **순차 또는 일정한 stride** 패턴만 인식한다. 포인터 추적(pointer chasing)은 프리페처를 무력화한다.

```c
// BAD: 노드 기반 순회 — 매 노드가 캐시 미스 후보
for (node_t *n = head; n; n = n->next) process(n->value);

// GOOD: 연속 배열 순회 — 프리페처가 작동
for (size_t i = 0; i < n; i++) process(values[i]);
```

**규칙:** 순회가 주 연산인 컨테이너는 링크드 구조를 쓰지 않는다. 삽입/삭제가 필요하면 인덱스 기반 free list를 배열 위에 구현한다.

### MEM-02 — 핫 구조체를 캐시 라인에 맞춘다
64B를 넘는 구조체를 매 반복 접근하면 라인 2개를 소비한다.

```c
// BAD: 72 B → 라인 2개
struct entry {
    uint64_t key;      // 8
    char     name[56]; // 56
    uint64_t aux;      // 8
};

// GOOD: 64 B 이내로 재배치, 큰 필드는 분리
struct entry {
    uint64_t key;
    uint64_t aux;
    uint32_t name_offset;  // 별도 문자열 풀 인덱스
    uint32_t name_len;
};  // 24 B
```

**필드 정렬 규칙:** 큰 타입 → 작은 타입 순으로 선언해 패딩을 최소화한다. `sizeof`를 확인해 예상과 다르면 패딩을 조사한다.

### MEM-03 — False Sharing을 제거한다 (병렬 코드 최다 실수)
서로 다른 코어가 **같은 캐시 라인의 다른 변수**를 쓰면 라인이 코어 간에 왕복한다. 논리적으로 독립이어도 성능은 락과 유사하게 붕괴한다.

```c
// BAD: 워커 카운터가 한 라인에 몰림
struct { uint64_t rows; uint64_t nulls; } stats[NUM_WORKERS];
// 워커 0과 1의 카운터가 같은 64B 라인 → 매 증가마다 라인 전송

// GOOD: 라인 단위 격리
struct alignas(64) worker_stats {
    uint64_t rows;
    uint64_t nulls;
    char _pad[64 - 2 * sizeof(uint64_t)];
};
static_assert(sizeof(worker_stats) == 64, "must be one cache line");
worker_stats stats[NUM_WORKERS];
```

**진단:** `perf c2c record` / `perf c2c report`로 라인 경쟁을 직접 확인한다. IPC가 스레드 수를 늘려도 개선되지 않으면 우선 이것을 의심한다.

**적용 대상:** 워커별 카운터, 부분합, 진행률, 결과 버퍼 포인터, 취소 플래그 인접 변수.

### MEM-04 — 데이터 레이아웃을 접근 패턴에 맞춘다 (AoS vs SoA)

```c
// AoS — 한 레코드의 여러 필드를 함께 쓸 때 유리
struct row { int64_t a; double b; int32_t c; };
row rows[N];

// SoA — 한 필드만 전체 순회할 때 유리 (+ 벡터화 가능)
struct columns { int64_t a[N]; double b[N]; int32_t c[N]; };
```

**판정:** 루프가 필드 일부만 읽는다면 SoA. 전 필드를 읽는다면 AoS.
컬럼 단위 집계·필터·통계 수집은 SoA가 원칙.

### MEM-05 — Hot/Cold 필드를 분리한다
자주 접근하는 필드와 드물게 접근하는 필드가 한 구조체에 있으면, 콜드 필드가 캐시 라인을 낭비한다.

```c
// GOOD
struct scan_state_hot {     // 매 행 접근
    uint64_t rows_seen;
    uint64_t reservoir_used;
    void    *sketch;
};
struct scan_state_cold {    // 시작/마감에만 접근
    char      table_name[128];
    timestamp started_at;
    int       error_code;
};
```

### MEM-06 — 명시적 프리페치는 순차 스캔에서만, 실측으로 거리를 정한다

```c
#define PREFETCH_DIST 16   // 실측으로 결정. 8~64 범위에서 탐색
for (size_t i = 0; i < n; i++) {
    __builtin_prefetch(&rows[i + PREFETCH_DIST], 0 /*read*/, 1 /*low locality*/);
    process(rows[i]);
}
```

**주의:** 하드웨어 프리페처가 이미 잡는 패턴에 수동 프리페치를 넣으면 캐시 오염으로 **느려진다.** 반드시 전후 측정.
해시 테이블 프로브처럼 주소를 미리 계산할 수 있는 랜덤 접근에서 효과가 크다.

### MEM-07 — 작업 집합을 캐시에 맞춰 분할한다 (블로킹/타일링)
데이터가 L2를 넘으면 청크로 나눠 처리한다.

```c
// BAD: 두 배열 전체를 반복 순회 → 매 패스가 DRAM에서 재적재
for (pass = 0; pass < P; pass++)
    for (i = 0; i < N; i++) work(a[i], b[i]);

// GOOD: 캐시에 들어가는 블록 단위로 모든 패스를 완료
const size_t BLOCK = L2_SIZE / (2 * sizeof(elem)) / 2;  // 여유 확보
for (size_t base = 0; base < N; base += BLOCK)
    for (pass = 0; pass < P; pass++)
        for (i = base; i < min(base + BLOCK, N); i++) work(a[i], b[i]);
```

### MEM-08 — 큰 순차 작업에는 huge page를 고려한다
수 GB를 순회하면 TLB 미스가 병목이 될 수 있다. 2 MB huge page는 TLB 엔트리 수요를 512배 줄인다.

```c
madvise(ptr, len, MADV_HUGEPAGE);   // Linux, THP 활성 시
```

### MEM-09 — 쓰기 전용 대량 버퍼는 read-for-ownership을 피한다
버퍼 전체를 덮어쓸 때 CPU는 기본적으로 먼저 읽어온다(RFO). 비시간적(non-temporal) 저장으로 우회 가능.

```c
_mm256_stream_si256((__m256i*)dst, val);   // 캐시를 우회해 직접 메모리로
_mm_sfence();                               // 이후 순서 보장 필요 시
```

**단, 그 데이터를 곧 다시 읽는다면 역효과다.**


### MEM-10 — 드물게 타지만 빨라야 하는 경로는 캐시를 데워 둔다 (cache warming)

가끔만 실행되지만 실행될 때 빨라야 하는 경로는 첫 실행이 항상 느리다 — 코드(I-cache)와
데이터(D-cache)가 전부 식어 있어서다. **대기 시간에 그 경로를 미리 태워 캐시를 유지**한다.
실측 ~90% (cold 267.7ms → warm 25.6ms, `저지연패턴-HFT논문.md` 2.1).

- HFT 원형: 매 틱마다 실행 엔진 코드를 끝까지 돌리되 실제 주문만 신호가 있을 때 내보낸다.
- DB 대응: 서버 재시작 직후의 첫 질의, 장시간 유휴 후의 복구 경로, 드물게 걸리는 슬로우패스
  전환(스필 등). **버퍼풀 예열·통계 프리로드가 같은 원리의 거시 버전**이다.
- ⚠ 예열 실행에 **부작용이 없어야 한다.** 상태를 바꾸는 경로면 dry-run 분리가 선행이다.
- ⚠ 측정과의 관계: 벤치마크 warmup(MEAS-04)은 이 효과를 **제거**하기 위한 것이고, 이 규칙은
  프로덕션에서 이 효과를 **이용**하는 것이다. 혼동하지 않는다.
---

## 3. 캐시 코히런시와 메모리 공유 규약 (COH)

MEM 규칙들의 **근거가 되는 하드웨어 계층**. 멀티스레드 성능 문제의 대부분이 이 절의 원리로 설명된다.
"공유하면 느려진다"는 부정확하다. **읽기 공유는 무료고, 쓰기 공유가 재앙이다.** 그 차이를 이해하는 것이 핵심.

### COH-01 — 캐시 라인 상태(MESI)를 기준으로 공유 패턴을 설계한다

각 코어의 캐시 라인은 상태를 가진다.

| 상태 | 의미 | 다른 코어 사본 | 쓰기 비용 |
|---|---|---|---|
| **M** (Modified) | 이 코어만 보유, 수정됨 | 없음 | 무료 (이미 소유) |
| **E** (Exclusive) | 이 코어만 보유, 깨끗함 | 없음 | 무료 (M으로 전이) |
| **S** (Shared) | 여러 코어가 읽기 사본 보유 | 있음 | **비쌈 (전체 무효화 필요)** |
| **I** (Invalid) | 무효 | — | 재적재 필요 |

**여기서 세 가지 규약이 나온다:**

1. **여러 코어가 읽기만 하면 전부 S 상태로 공존한다 → 코히런시 트래픽 0. 완전 무료.**
2. **한 코어만 읽고 쓰면 E/M 상태를 유지한다 → 트래픽 0.**
3. **한 코어가 쓰고 다른 코어가 그 라인을 갖고 있으면, 매 쓰기가 무효화 브로드캐스트를 유발한다.**

> **최상위 규약: 캐시 라인 하나당 "쓰는 주체"를 정확히 하나로 만든다.**
> 이것만 지키면 MEM-03, GLOB-02, CPP-01, PAR-01의 문제가 전부 사라진다.

### COH-02 — Read-For-Ownership(RFO)과 HITM 비용을 인식한다

코어가 라인에 쓰려면 먼저 **배타적 소유권**을 얻어야 한다.

```
다른 코어가 S로 보유  → 무효화 메시지 브로드캐스트 + 응답 대기
다른 코어가 M으로 보유 → 그 코어에서 데이터를 직접 전송 (HITM, cache-to-cache)
                          → 100~300 사이클. 코히런시 비용 중 최악
```

`perf c2c`가 측정하는 **HITM**이 정확히 이것이다. 리포트 상위 심볼이 곧 범인이다.

```bash
perf c2c record -F 60000 -a -- ./bench
perf c2c report --stdio | head -60
# "Shared Data Cache Line Table" 상단 = 경쟁이 가장 심한 라인
# 그 아래 오프셋별 읽기/쓰기 주체와 심볼명이 나온다
```

**중요:** 쓰기 자체가 비싼 게 아니라 **다른 코어가 그 라인을 갖고 있는 상태에서의 쓰기**가 비싸다. 같은 코드가 스레드 1개일 때 빠르고 여러 개일 때 느려지는 이유가 이것이다.

### COH-03 — 인접 라인 프리페처 때문에 128B 정렬이 필요할 수 있다

Intel의 공간 프리페처(spatial prefetcher)는 **128B 정렬 쌍의 형제 라인을 함께 가져온다.** 따라서 64B 패딩만으로는 false sharing이 남을 수 있다.

```c
/* 극단적 경쟁 구간에서는 128B */
#define COH_PAD 128
typedef struct { uint64_t v; char _pad[COH_PAD - sizeof(uint64_t)]; } hot_slot_t;
_Static_assert(sizeof(hot_slot_t) == COH_PAD, "pad mismatch");
```

**규칙:** 기본은 64B로 하고, `perf c2c`에서 여전히 HITM이 잡히면 128B로 올려 재측정한다. 무조건 128B를 쓰면 메모리·캐시 낭비가 2배가 되므로 실측 근거가 있을 때만.

### COH-04 — 라인당 단일 쓰기 주체 원칙을 불변식으로 유지한다

설계 시점에 적용하는 구체 규칙:

```c
/* 1. 워커별 상태는 라인 정렬 배열로 */
typedef struct { uint64_t rows, nulls, bytes; char _pad[CACHE_LINE - 24]; } wstate_t;
static wstate_t g_wstate[MAX_WORKERS] CACHE_ALIGNED;

/* 2. 공유 집계는 로컬 누적 후 1회 병합 (PAR-01) */

/* 3. 결과 버퍼는 라인 경계로 분할 — 청크 시작을 정렬 */
size_t chunk = ALIGN_UP(total / nworkers, CACHE_LINE / elem_size);

/* 4. 읽기 전용 데이터와 쓰기 데이터를 다른 라인으로 분리 */
struct scan_ctx {
    /* --- 읽기 전용 (모든 워커가 S로 공유. 무료) --- */
    const row_t *rows;
    size_t       nrows;
    int64_t      min_key;
    char _pad0[CACHE_LINE - 24];
    /* --- 쓰기 영역은 워커별 배열로 분리 (위 g_wstate) --- */
};
```

### COH-05 — 읽기 전용 공유는 자유롭게 하되 타입으로 명시한다

읽기 전용 데이터는 코어 수와 무관하게 완전 무료로 공유된다. `const`로 선언해 `.rodata`에 놓으면 컴파일러·하드웨어 양쪽에서 최적화된다 (GLOB-03).

**따라서 확장성 설계의 목표는 "공유를 줄이는 것"이 아니라 "쓰기 공유를 없애는 것"이다.** 읽기 전용 룩업 테이블은 몇 코어가 때리든 문제되지 않는다.

### COH-06 — 링 버퍼의 인덱스를 분리하고 사본을 캐싱한다 (SPSC 큐)

생산자는 `tail`을 쓰고 `head`를 읽고, 소비자는 `head`를 쓰고 `tail`을 읽는다. 두 인덱스가 같은 라인에 있으면 **모든 push/pop이 라인 왕복을 유발한다.**

```c
/* BAD: head와 tail이 같은 라인 → 매 연산 HITM */
struct ring { uint64_t head, tail; void *slots[N]; };

/* GOOD: 분리 + 상대 인덱스 사본 캐싱으로 접근 빈도까지 줄인다 */
struct ring {
    CACHE_ALIGNED _Atomic uint64_t head;        /* 소비자가 쓴다 */
    CACHE_ALIGNED _Atomic uint64_t tail;        /* 생산자가 쓴다 */
    CACHE_ALIGNED uint64_t p_cached_head;       /* 생산자 전용 사본 */
    CACHE_ALIGNED uint64_t c_cached_tail;       /* 소비자 전용 사본 */
    CACHE_ALIGNED void *slots[N];
};

/* 생산자: 사본이 여유를 보이면 공유 head를 아예 읽지 않는다 */
static bool push(struct ring *r, void *v) {
    uint64_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    if (t - r->p_cached_head >= N) {                       /* 사본으로 판단 */
        r->p_cached_head = atomic_load_explicit(&r->head, memory_order_acquire);
        if (t - r->p_cached_head >= N) return false;       /* 정말 꽉 찬 경우만 */
    }
    r->slots[t & (N - 1)] = v;
    atomic_store_explicit(&r->tail, t + 1, memory_order_release);
    return true;
}
```

이 패턴은 공유 라인 접근을 큐가 거의 빌 때/찰 때로 한정한다. 배치 처리(한 번에 여러 개 push/pop)를 추가하면 트래픽이 더 줄어든다.

### COH-07 — 비트필드를 스레드 간 공유하지 않는다 (C의 조용한 데이터 레이스)

비트필드 쓰기는 **워드 단위 read-modify-write**다. 같은 워드의 서로 다른 비트필드를 두 스레드가 쓰면 데이터 레이스이며, **한쪽의 갱신이 소실된다.** C11 표준에서 인접 비트필드는 같은 "메모리 위치"로 취급된다.

```c
/* BAD: 논리적으로 독립인데 레이스 + 값 손실 */
struct page_flags { unsigned dirty:1; unsigned pinned:1; unsigned io:1; };
/* 스레드 A가 dirty=1, 스레드 B가 pinned=1 → 하나가 사라질 수 있다 */

/* GOOD 1: 0폭 비트필드로 메모리 위치를 분리 */
struct page_flags {
    unsigned dirty:1;
    unsigned :0;              /* 다음 필드를 새 저장 단위로 강제 */
    unsigned pinned:1;
};

/* GOOD 2: 원자적 바이트로 분리 (권장) */
struct page_flags {
    _Atomic unsigned char dirty;
    _Atomic unsigned char pinned;
    _Atomic unsigned char io;
};

/* GOOD 3: 한 워드를 원자적으로 다루고 비트 연산 */
_Atomic uint32_t flags;
atomic_fetch_or_explicit(&flags, FLAG_DIRTY, memory_order_relaxed);
```

**규칙:** 스레드 간 공유되는 플래그 집합에는 비트필드를 쓰지 않는다. 단일 스레드 전용이거나 락으로 보호될 때만 쓴다.

### COH-08 — 원자적 변수의 자연 정렬을 보장한다

정렬되지 않은 원자적 접근은 여러 캐시 라인에 걸쳐 버스 락이 되거나 원자성이 깨진다. 특히 **라인 경계를 걸치는 경우(split lock)** 는 시스템 전체 성능에 영향을 준다.

```c
_Alignas(8)  _Atomic uint64_t counter;      /* 8바이트 자연 정렬 */
_Alignas(16) _Atomic struct { void *p; uint64_t tag; } tagged_ptr;  /* DWCAS */
_Static_assert(_Alignof(_Atomic uint64_t) >= 8, "unaligned atomic");
```

패킹된 구조체(`__attribute__((packed))`) 안에 원자적 필드를 두지 않는다.

### COH-09 — 스토어 버퍼와 4K 앨리어싱을 인식한다

스토어는 스토어 버퍼를 경유한다. 직전에 쓴 주소를 읽으면 store-to-load forwarding으로 빠르지만, **주소 하위 12비트가 같고 실제 주소는 다른 경우(4K aliasing)** CPU가 의존성을 오판해 스톨이 발생한다.

```c
/* BAD: 두 큰 배열의 시작 오프셋이 4KB 배수 → 스캔 중 거짓 의존성 */
double *a = aligned_alloc(4096, N * 8);
double *b = aligned_alloc(4096, N * 8);
for (i = 0; i < N; i++) b[i] = a[i] * 2.0;   /* a[i]와 b[i]가 4K 앨리어싱 */

/* GOOD: 오프셋을 캐시 라인 단위로 어긋나게 */
char *pool = aligned_alloc(4096, 2 * N * 8 + 4096);
double *a = (double*)pool;
double *b = (double*)(pool + N * 8 + CACHE_LINE);   /* 의도적 오프셋 */
```

진단: `perf stat -e ld_blocks_partial.address_alias` (Intel).

### COH-10 — 컴파일러 배리어와 CPU 배리어를 구분한다

```c
/* 컴파일러 배리어: 재정렬만 방지. 명령 생성 0. 런타임 비용 없음 */
#define COMPILER_BARRIER() __asm__ __volatile__("" ::: "memory")

/* CPU 배리어: 실제 명령 생성 */
atomic_thread_fence(memory_order_acquire);   /* x86: 보통 명령 없음 */
atomic_thread_fence(memory_order_release);   /* x86: 보통 명령 없음 */
atomic_thread_fence(memory_order_seq_cst);   /* x86: mfence (~30~100 사이클) */
```

**x86-64는 강한 메모리 모델(TSO)** 이라 store→load 재정렬만 발생한다. 따라서 acquire/release는 대부분 컴파일러 배리어만으로 충족되고 명령이 생성되지 않는다. **ARM64는 약한 모델**이라 `dmb ish` 등이 실제로 생성된다.

**결론:** 코드는 표준 원자적 연산과 메모리 순서로 표현하고, 플랫폼별 비용 차이는 컴파일러에 맡긴다. 직접 `mfence`를 넣지 않는다. 단, x86에서 잘 돌던 락프리 코드가 ARM에서 깨지는 일이 흔하므로 **약한 모델에서 반드시 검증**한다.

### COH-11 — 프로세스 간 공유 메모리도 같은 규칙을 따른다

`shm_open`/`mmap`으로 공유한 영역도 캐시 코히런시 대상이므로 COH-01~09가 전부 적용된다. 추가로:

- 서로 다른 프로세스는 컴파일 단위를 공유하지 않으므로 **컴파일러 배리어에 의존할 수 없다.** 원자적 타입과 명시적 메모리 순서가 필수.
- 구조체 레이아웃·정렬이 양쪽에서 동일해야 한다. 컴파일러·플래그가 다르면 패딩이 달라질 수 있으므로 `_Static_assert`로 크기·오프셋을 고정한다.

```c
_Static_assert(sizeof(shm_header_t) == 64, "shm layout changed");
_Static_assert(offsetof(shm_header_t, seq) == 8, "shm offset changed");
```

### COH-12 — 공유(sharing)보다 이전(transfer)을 선호한다

여러 스레드가 같은 데이터를 동시 접근하게 하는 대신, **소유권을 한 번에 넘기는** 설계가 코히런시 트래픽을 근본적으로 제거한다.

| 모델 | 코히런시 특성 |
|---|---|
| 공유 + 락 | 접근마다 라인 왕복 + 직렬화 |
| 공유 + 원자적 | 접근마다 RFO |
| **분할 소유 (파티셔닝)** | **트래픽 0. 각 라인에 쓰기 주체 1개** |
| **이전 (큐/메시지)** | 넘기는 순간에만 트래픽 |

파티셔닝이 가능하면 항상 그것이 최선이다. 통계 수집·스캔처럼 데이터를 구역으로 나눌 수 있는 작업은 락 없이 선형 확장이 가능하다.

---

## 4. 앨리어싱과 타입 규칙 (ALIAS) — C에서 가장 큰 최적화 레버

컴파일러가 값을 레지스터에 유지할 수 있는지는 **"이 두 포인터가 겹칠 수 있는가"** 에 달려 있다.
겹칠 수 있다고 판단하면 매 쓰기 후 모든 로드를 다시 해야 하므로 루프 최적화·벡터화가 전부 막힌다.
**C에서는 이 정보를 프로그래머가 타입과 `restrict`로 제공해야 한다.**

### ALIAS-01 — strict aliasing 규칙을 지킨다 (정확성과 성능이 동시에 걸린 항목)

C 표준은 객체를 그 유효 타입과 호환되지 않는 타입의 lvalue로 접근하는 것을 UB로 규정한다. 컴파일러는 이를 근거로 **"타입이 다른 포인터는 겹치지 않는다"(TBAA)** 고 가정하고 로드를 레지스터에 유지한다.

```c
/* BAD: UB. 최적화 수준에 따라 결과가 달라진다 */
float f = 1.0f;
uint32_t bits = *(uint32_t *)&f;

/* GOOD 1: memcpy — 컴파일러가 단일 로드로 치환하므로 런타임 비용 0 */
uint32_t bits;
memcpy(&bits, &f, sizeof bits);

/* GOOD 2: union — C에서는 합법적인 타입 재해석 (C++에서는 아님) */
union f32_bits { float f; uint32_t u; };
union f32_bits cvt = { .f = f };
uint32_t bits = cvt.u;
```

> **`-fno-strict-aliasing`으로 덮지 않는다.** 이 플래그는 컴파일러 전역을 보수적으로 만들어
> **모든 루프의 최적화 품질을 떨어뜨린다.** 리눅스 커널이 쓰는 건 역사적 이유이며,
> 새 코드는 `memcpy`/`union`으로 표현한다.
> 이미 이 플래그가 걸린 코드베이스라면, 제거 시도가 무료로 얻는 성능 개선일 수 있다.

### ALIAS-02 — `char*` 계열은 모든 것과 앨리어싱한다

`char*`, `unsigned char*`, `void*` 경유 접근은 **어떤 타입과도 겹칠 수 있다**는 것이 표준 규정이다. 따라서 바이트 단위 루프는 컴파일러가 앨리어싱을 배제하지 못해 벡터화·레지스터 유지가 어렵다.

```c
/* BAD: 바이트 루프 — dst와 src가 겹칠 수 있다고 가정 */
for (size_t i = 0; i < n; i++) dst[i] = src[i];

/* GOOD: 의도를 표준 함수로 전달 → 컴파일러 내장으로 치환, SIMD 사용 */
memcpy(dst, src, n);     /* 겹치지 않음을 약속 */
memmove(dst, src, n);    /* 겹칠 수 있음 */
```

**규칙:** 바이트 복사·비교·채우기는 직접 루프로 쓰지 않고 `memcpy`/`memmove`/`memcmp`/`memset`을 쓴다. 이들은 컴파일러 내장으로 치환되어 손으로 쓴 어떤 루프보다 빠르다.

### ALIAS-03 — `restrict`로 겹치지 않음을 명시한다

```c
/* 이 약속이 없으면 out[i] 쓰기가 a/b 재로드를 유발해 벡터화 불가 */
void merge_add(double * restrict out,
               const double * restrict a,
               const double * restrict b, size_t n)
{
    for (size_t i = 0; i < n; i++) out[i] = a[i] + b[i];
}
```

블록 스코프에도 쓸 수 있다.

```c
{
    row_t * restrict p = base + off;   /* 이 블록 내에서 유일한 접근 경로임을 약속 */
    for (...) p[i].flags |= F;
}
```

**주의:** 실제로 겹치면 **UB이며 조용히 잘못된 결과가 나온다.** 겹칠 가능성이 있는 경로가 하나라도 있으면 붙이지 않는다. 안전한 대안은 겹침 여부를 런타임에 검사해 두 경로로 분기하는 것이다.

```c
if (dst + n <= src || src + n <= dst) fast_path_restrict(dst, src, n);
else                                  safe_path(dst, src, n);
```

### ALIAS-04 — 같은 타입 포인터 간 앨리어싱을 지역 변수로 끊는다

같은 타입의 두 포인터는 겹칠 수 있으므로, 한쪽에 쓰면 다른 쪽 로드가 무효화된다. GLOB-01의 구조체 필드 버전이다.

```c
/* BAD: dst->sum 쓰기마다 src->vals 재로드 가능 (dst와 src가 같을 수 있으므로) */
void accumulate(stats_t *dst, const stats_t *src, size_t n) {
    for (size_t i = 0; i < n; i++) dst->sum += src->vals[i];
}

/* GOOD: 지역 변수로 누적 → 레지스터 유지 + 벡터화 가능 */
void accumulate(stats_t *dst, const stats_t *src, size_t n) {
    uint64_t sum = dst->sum;
    for (size_t i = 0; i < n; i++) sum += src->vals[i];
    dst->sum = sum;
}
```

**일반 규칙:** 루프에서 갱신되는 값은 **반드시 지역 변수에 담아 누적하고 루프 종료 후 1회 반영**한다. 대상이 전역이든 구조체 필드든 포인터 역참조든 동일하다.

### ALIAS-05 — 포인터를 정수로 왕복시키지 않는다 (프로버넌스)

포인터를 정수로 변환했다 되돌리면 컴파일러가 출처(provenance) 추적을 잃는다. 최적화가 보수적으로 바뀌거나, 반대로 잘못된 가정을 하게 된다.

```c
/* BAD: 태그 포인터를 정수 연산으로 조작 */
uintptr_t tagged = (uintptr_t)p | TAG;
node_t *q = (node_t *)(tagged & ~TAG_MASK);   /* 프로버넌스 상실 */

/* GOOD: 원본 포인터를 별도 보관하거나, 인덱스 기반으로 설계 */
struct tagged_ref { uint32_t index; uint32_t tag; };   /* 배열 인덱스 + 태그 */
node_t *q = &pool[ref.index];
```

인덱스 기반 설계는 프로버넌스 문제가 없고, 포인터보다 작아 캐시 효율도 좋다.

### ALIAS-06 — 정렬을 컴파일러에 알린다

```c
/* 실제 정렬을 먼저 보장한다 */
double *buf = aligned_alloc(64, n * sizeof(double));
/* 컴파일러에 알려 정렬 SIMD 명령을 쓰게 한다 */
double *p = __builtin_assume_aligned(buf, 64);
for (size_t i = 0; i < n; i++) p[i] *= 2.0;
```

거짓이면 UB. 정렬 할당(CLOW-11)과 반드시 짝지어 쓴다.

### ALIAS-07 — 벡터화 실패 원인을 앨리어싱부터 확인한다

```bash
gcc   -O3 -fopt-info-vec-missed foo.c 2>&1 | grep -i alias
clang -O3 -Rpass-analysis=loop-vectorize foo.c 2>&1 | grep -i alias
```

"cannot prove pointers are not aliased" 계열 메시지가 나오면 `restrict` 또는 지역 변수 누적으로 해결한다. 이것이 벡터화 실패의 가장 흔한 원인이다.

### ALIAS-08 — `const`는 앨리어싱 정보가 아니다

`const T *`는 "이 포인터로 쓰지 않겠다"는 뜻일 뿐, **다른 포인터가 그 객체를 수정하지 않는다는 보장이 아니다.** 최적화에 도움이 되지 않는다. 겹치지 않음을 약속하려면 `restrict`가 필요하다.

```c
/* const만으로는 부족 — 컴파일러는 out이 in을 가리킬 수 있다고 본다 */
void f(int *out, const int *in, size_t n);
/* 이렇게 해야 최적화된다 */
void f(int * restrict out, const int * restrict in, size_t n);
```

---

## 5. 분기 (BR)

### BR-01 — 예측 가능한 분기는 그대로 둔다
분기 예측기는 규칙적 패턴에서 정확도 99% 이상이며 사실상 무료다. **branchless 변환은 예측 실패율이 실측으로 높을 때만** 적용한다.

### BR-02 — 콜드 경로를 힌트로 표시한다

```c
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

if (UNLIKELY(ptr == NULL)) goto error;      // 에러는 거의 발생 안 함
if (LIKELY(cache_hit)) return cached_value;
```

C++20 이상: `if (x) [[likely]] { ... }`

효과는 분기 예측이 아니라 **코드 배치**다. 핫 경로가 연속 배치되어 I-cache 효율이 오른다.

### BR-03 — 데이터 의존 분기는 산술로 치환한다

```c
// BAD: 랜덤 데이터에서 예측 실패율 ~50%
for (i = 0; i < n; i++) if (v[i] > t) count++;

// GOOD: 분기 제거
for (i = 0; i < n; i++) count += (v[i] > t);

// 조건부 선택도 산술로
// BAD
int m = (a > b) ? a : b;
// GOOD (컴파일러가 보통 cmov로 처리하지만 명시 가능)
int m = a ^ ((a ^ b) & -(a < b));   // 가독성 저하 — 실측 이득 있을 때만
```

**우선순위:** 컴파일러가 `cmov`를 생성하는지 먼저 확인한다(어셈블리 확인). 생성한다면 수동 비트 트릭은 불필요.

### BR-04 — 루프 안의 불변 분기를 밖으로 뺀다

```c
// BAD: 매 반복 조건 검사
for (i = 0; i < n; i++) {
    if (mode == MODE_A) do_a(v[i]);
    else                do_b(v[i]);
}

// GOOD: 루프 분기 (loop unswitching)
if (mode == MODE_A) for (i = 0; i < n; i++) do_a(v[i]);
else                for (i = 0; i < n; i++) do_b(v[i]);
```

코드 중복이 생기므로 템플릿이나 매크로로 관리한다. 컴파일러가 자동으로 하기도 하나 보장되지 않는다.

> **확장 적용 (실행 엔진·인터프리터):** "컴파일 타임에 이미 정해진 값"을 실행 루프에서 다시
> 테스트하는 것이 같은 위반이다. 연산자 종류, 피연산자 타입, 결과 도메인, 플래그, 필드 인덱스는
> 프로그램을 만들 때 확정되므로 **특수화된 함수(커널)를 선택해 두고 루프에서는 호출만** 한다.
> 마찬가지로 **매 반복 같은 값을 쓰는 스토어**(불변 포인터 재발행 등)는 준비 단계로 옮긴다.

### BR-05 — 스위치는 밀집 정수 케이스로 만든다
케이스 값이 연속·밀집하면 컴파일러가 점프 테이블을 생성한다. 희소하면 비교 연쇄가 된다. enum 값을 0부터 연속으로 부여한다.

### BR-06 — 가상 함수 호출을 핫루프에서 제거한다
간접 호출은 예측 실패 + 인라이닝 불가 + 최적화 차단.

```cpp
// BAD: 매 행마다 가상 디스패치
for (auto& row : rows) processor->handle(row);

// GOOD 1: 타입별로 루프를 분리 (디스패치를 루프 밖으로)
switch (processor->kind()) {
  case KIND_A: for (auto& r : rows) static_cast<A*>(processor)->handle(r); break;
  case KIND_B: for (auto& r : rows) static_cast<B*>(processor)->handle(r); break;
}

// GOOD 2: 템플릿으로 컴파일 시점 확정
template<typename Proc> void run(Proc& p, span<row> rows) {
    for (auto& r : rows) p.handle(r);   // 인라인 가능
}
```

> **트레이드오프 주의:** 함수 포인터 디스패치 자체가 항상 나쁜 것은 아니다. 수백 케이스 스위치를
> 트리 재귀로 매 행 통과하는 것보다, 한 번 확정한 함수 포인터를 호출하는 편이 훨씬 싸다.
> 나쁜 것은 **디스패치가 루프 안에서 매번 재결정되는 것**이다. 특히 `switch(종류)` →
> `switch(타입)` → `switch(연산자)`처럼 중첩되면 (종류×타입×연산자) 리프를 미리 확정한다.


### BR-07 — 조건 순서가 곧 비용이다 (단축 평가 활용)

`&&`/`||`는 단축 평가된다. **싸고 자주 걸러지는 조건을 앞에** 두면 비싼 조건의 실행 자체가
사라진다. 실측 전 구간에서 약 50% (`저지연패턴-HFT논문.md` 2.6).

```c
/* 나쁨 — 비싼 검사가 먼저 */
if (expensive_type_check (v) && flag_is_set (v)) ...
/* 좋음 — 싼 플래그로 대부분 탈락시킨 뒤 비싼 검사 */
if (flag_is_set (v) && expensive_type_check (v)) ...
```

- 술어 평가·스캔 필터처럼 **행마다 도는 조건**에서 효과가 크다. 선택도가 낮은(=많이 거르는)
  조건을 앞으로 보낸다.
- **부작용이 있는 조건은 순서를 바꾸지 않는다.** 단축 평가로 호출이 사라지면 의미가 달라진다.
- 순서를 바꿨으면 **선택도 가정을 주석으로 남긴다.** 데이터가 바뀌면 역전될 수 있다.

### BR-08 — 에러 검사는 플래그 하나로 합치고, 처리는 콜드 함수로 뺀다

핫 패스에 에러 검사가 `if/else if` 체인으로 늘어서면 (1) 분기 예측 슬롯을 소모하고
(2) 처리 코드가 인라인되어 **I-cache를 오염**시킨다. 두 문제를 한 번에 없앤다.

```c
/* 나쁨 — 검사마다 분기 + 처리 코드가 핫 패스에 인라인 */
if (check_a (p)) { handle_a (p); return; }
else if (check_b (p)) { handle_b (p); return; }
else do_work (p);

/* 좋음 — 분기 1개, 처리는 out-of-line */
if (likely (!p->error_flags)) do_work (p);
else handle_error (p);          /* __attribute__((noinline)), 콜드 */
```

실측: if-else 체인 7.35ns → 플래그 4.68ns(분기 축소, ~36%), 슬로우패스 분리 ~12%
(`저지연패턴-HFT논문.md` 2.7·2.8). 둘은 세트로 쓴다 — 각각 분기 예측 관점과 I-cache 관점이다.

- 에러 처리 함수에는 `__attribute__((noinline))`을 명시한다. 안 그러면 컴파일러가
  인라인해 목적이 사라진다.
- **콜드패스에는 적용하지 않는다.** 초기화·종료·에러 경로 자체를 이렇게 고치는 건 무의미하다
  (사용 규약: 핫/콜드 구분).
- MEAS-03의 `stalled-cycles-frontend > 20%`가 이 문제의 신호다.

---

## 6. 할당 (ALLOC)

### ALLOC-01 — 핫루프에서 절대 할당하지 않는다
`malloc`/`new`는 50~200 사이클 + 락 경쟁 + 캐시 오염. 루프 밖으로 옮기거나 재사용한다.

### ALLOC-02 — 컨테이너는 항상 사전 예약한다

```cpp
std::vector<sample> v;
v.reserve(expected_n);     // 재할당·복사·이동 전부 제거
```

`reserve` 없는 `push_back` 루프는 자동으로 지적 대상이다.

### ALLOC-03 — 일괄 해제 가능한 작업에는 arena를 쓴다

```c
typedef struct { char *base; size_t used, cap; } arena_t;

static inline void *arena_alloc(arena_t *a, size_t sz, size_t align) {
    size_t off = (a->used + (align - 1)) & ~(align - 1);
    if (UNLIKELY(off + sz > a->cap)) return NULL;   /* 또는 청크 추가 */
    a->used = off + sz;
    return a->base + off;
}
static inline void arena_reset(arena_t *a) { a->used = 0; }   /* O(1) 전체 해제 */
```

**적합 상황:** 요청 단위 임시 객체, 파싱 중간 결과, 스캔 중 수집 버퍼, 플랜 노드.
**부적합:** 수명이 개별적으로 다른 객체.

### ALLOC-04 — 고정 크기 객체는 풀 할당자를 쓴다
free list를 배열 위에 인덱스로 구현하면 포인터 추적 없이 O(1) 할당/해제가 된다.

```c
typedef struct { uint32_t next_free; /* payload */ } slot_t;
/* free_head 인덱스만 관리. 노드가 연속 메모리에 있어 캐시 친화적 */
```

### ALLOC-05 — 소형 다수 케이스는 SBO로 처리한다
대부분이 작고 드물게 크다면, 작은 경우를 스택/인라인 버퍼로 처리한다.

```cpp
template<typename T, size_t N>
class small_vector {
    alignas(T) unsigned char buf_[N * sizeof(T)];
    T*     data_ = reinterpret_cast<T*>(buf_);
    size_t size_ = 0, cap_ = N;
    bool   heap_ = false;
public:
    void push_back(const T& v) {
        if (UNLIKELY(size_ == cap_)) grow();   // 여기서만 힙 전환
        new (data_ + size_++) T(v);
    }
    ~small_vector() { /* 소멸자 호출 후 heap_이면 free */ }
};
```

### ALLOC-06 — 할당 실패를 조용히 삼키지 않는다
성능과 별개로 정확성 문제. `nothrow new`나 `realloc` 실패를 성공으로 처리하면 데이터가 조용히 누락된다.

```c
void *p = db_private_realloc(old, new_sz);
if (UNLIKELY(p == NULL)) {
    er_set(ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1, new_sz);
    return ER_FAILED;      /* 절대 무시하지 말 것 */
}
```

### ALLOC-07 — 컨테이너 소멸 비용을 인식한다
소멸자가 있는 객체 수백만 개를 담은 컨테이너는 소멸 자체가 비싸다. arena + trivially destructible 타입으로 설계하면 소멸이 무료가 된다.

### ALLOC-08 — 스레드 간 소유권이 바뀌는 메모리는 전역 힙을 쓴다
스레드 로컬(private) 힙에서 할당한 것을 다른 스레드가 해제하면 할당자가 중단(abort)될 수 있다.
워커 풀·병렬 스캔처럼 생성 스레드와 해제 스레드가 다를 수 있으면 처음부터 전역 힙으로 할당한다.
자료구조 주석에 **"어느 스레드가 해제하는가"** 를 명시해 둔다.

---

## 7. 부동소수점 (FP)

### FP-01 — 부동소수점 등가 비교에 `==`를 쓰지 않는다
비자명한 산술 후에는 정확히 같아지는 일이 사실상 없다. 비교 로직이 "동등" 분기에 도달하지 못하고, 서브-epsilon 노이즈가 결과를 결정한다.

```c
/* BAD */
if (cost_a == cost_b) return EQ;

/* GOOD: 상대 오차 기준 */
#define COST_EPS 1e-6
static inline bool fp_eq(double a, double b) {
    double diff = fabs(a - b);
    double mag  = fmax(fabs(a), fabs(b));
    return diff <= COST_EPS * fmax(mag, 1.0);   /* 0 근처는 절대 오차로 */
}
```

### FP-02 — 비교자는 대칭성과 추이성을 모두 만족해야 한다
**성분별 비교 후 합으로 순서를 정하면 비대칭 비교자가 된다.**

```c
/* BAD: 합이 같고 성분 구성이 다르면 양방향 모두 LT를 반환 */
if (a_fixed == b_fixed && a_var == b_var) return EQ;
return (a_fixed + a_var <= b_fixed + b_var) ? LT : GT;
    /* forward: LT, reverse: LT — 방문 순서에 따라 결과가 달라짐 */

/* GOOD: 단일 스칼라(총합)를 epsilon으로 비교 */
int cost_cmp(double a, double b) {
    if (fp_eq(a, b)) return EQ;
    return (a < b) ? LT : GT;
}
```

**필수 검증 프로브:**

```c
/* 대칭성: 무작위 쌍에 대해 cmp(a,b)와 cmp(b,a)가 반대여야 함 */
for (int i = 0; i < 400; i++) {
    double a = rnd_cost(), b = rnd_cost();
    int f = cost_cmp(a, b), r = cost_cmp(b, a);
    assert((f == LT && r == GT) || (f == GT && r == LT) || (f == EQ && r == EQ));
}
/* 추이성: a≈b, b≈c 이면 a≈c 여야 함 — epsilon 비교는 이걸 깨기 쉽다 */
for (...) {
    if (fp_eq(a,b) && fp_eq(b,c)) assert(fp_eq(a,c));   /* 실패 가능! */
}
```

> **경고:** epsilon 기반 등가는 추이성을 위반할 수 있다 (a≈b, b≈c 이지만 a≉c).
> `std::sort`에 strict weak ordering을 위반하는 비교자를 넘기면 **정의되지 않은 동작**이며,
> 크래시나 메모리 손상으로 이어질 수 있다.
> 정렬 키로 쓸 때는 (1) 비교 전 값을 양자화하거나, (2) epsilon 등가를 쓰지 않고
> 엄격 비교 + 결정적 타이브레이커(ID 등)를 쓴다.

```c
/* 정렬용 안전 패턴: 양자화 후 엄격 비교, 동률은 결정적 타이브레이크 */
static inline int64_t quantize(double c) { return (int64_t)(c / COST_EPS); }
int plan_cmp(const plan_t *x, const plan_t *y) {
    int64_t qx = quantize(x->total_cost), qy = quantize(y->total_cost);
    if (qx != qy) return (qx < qy) ? -1 : 1;
    return (x->id < y->id) ? -1 : (x->id > y->id) ? 1 : 0;   /* 안정적 */
}
```

### FP-03 — `-ffast-math`를 정확성 민감 코드에 쓰지 않는다
결합법칙 재배열, NaN/Inf 부재 가정, 나눗셈→역수곱 치환이 일어난다. 비용 모델·선택도 추정·통계 집계는 재현성이 필요하므로 금지. 필요하면 함수 단위로만 적용한다.

### FP-04 — 반복 나눗셈은 역수 곱으로 치환한다

```c
/* BAD: 루프마다 나눗셈 (13~20 사이클) */
for (i = 0; i < n; i++) out[i] = v[i] / total;

/* GOOD: 나눗셈 1회 + 곱셈 n회 */
const double inv = 1.0 / total;
for (i = 0; i < n; i++) out[i] = v[i] * inv;
```

**주의:** 결과가 비트 단위로 달라질 수 있다. 재현성이 필요하면 적용하지 않는다.

### FP-05 — 대량 합산은 오차 누적을 고려한다
단순 순차 합산은 O(n) 오차가 쌓인다. 정확도가 필요하면 Kahan 보상 합산 또는 쌍대(pairwise) 합산을 쓴다. 쌍대 합산은 정확도와 벡터화를 동시에 얻는다.

```c
/* 부분합 여러 개로 나누면 오차 감소 + ILP 향상 + 벡터화 가능 */
double s0=0, s1=0, s2=0, s3=0;
for (i = 0; i + 4 <= n; i += 4) { s0+=v[i]; s1+=v[i+1]; s2+=v[i+2]; s3+=v[i+3]; }
double sum = (s0 + s1) + (s2 + s3);
for (; i < n; i++) sum += v[i];
```

### FP-06 — 정수로 표현 가능한 값은 정수로 다룬다
카운트, 오프셋, 크기, 개수는 부동소수점으로 저장하지 않는다. 비교 문제와 정밀도 손실을 원천 차단한다.

### FP-07 — 고정소수점(DECIMAL/NUMERIC) 누산은 캐리를 지연한다
십진 고정소수점 덧셈은 정밀도·스케일 조회, 자릿수 스캔, 반올림, 패킹이 매번 붙는다.
대량 합산에서는 **헤드룸이 넉넉한 정수 워드 버킷에 누적**하고, 자릿수 정리·반올림·패킹은
마감 시 1회만 수행한다. 부호별 버킷을 따로 두면 행당 오버플로 검사도 없앨 수 있다.
누산 상태를 도입하면 **소비 지점(중간 결과 읽기, 부분합 병합, 스필 저장·로드, 해제, 초기화)마다
구체화(flush)를 선행**해야 하므로, 그 지점 목록을 코드 주석으로 고정하고 리뷰에서 전수 확인한다.


### FP-08 — float와 double을 한 수식에 섞지 않는다

혼용하면 암시적 승격(float→double)·강등(double→float) 변환 명령이 붙는다. 행마다 도는
루프에서 무시 못 할 비용이다. 실측 21.6ns → 14.2ns (~52%, `저지연패턴-HFT논문.md` 2.11).

```c
float a, b;
a = b * 1.23;    /* 1.23은 double 리터럴 → 승격+강등 2회 */
a = b * 1.23f;   /* float 리터럴 → 변환 없음 */
```

- 필요한 정밀도를 먼저 정하고 타입을 통일한다. double→float 강등은 정밀도 손실이 따른다.
- DB 대응: 통계·선택도 계산은 double로 통일돼 있는지, 리터럴에 `f`가 섞여 있지 않은지.

---

## 8. 전역·정적·TLS 상태 (GLOB)

전역 상태는 **단일 스레드에서는 최적화 차단**, **멀티 스레드에서는 캐시 코히런시 폭풍**을 일으킨다.
성능 문제의 원인으로 자주 간과되므로 별도 카테고리로 다룬다.

### GLOB-01 — 루프에서 전역 변수를 지역 변수로 복사한다

컴파일러는 **함수 호출을 넘어 전역을 레지스터에 유지할 수 없다.** 호출된 함수가 그 전역을 수정할 가능성을 배제할 수 없기 때문이다. 결과적으로 매 접근이 메모리 로드가 된다.

```c
extern int g_threshold;

/* BAD: 매 반복 g_threshold를 메모리에서 재로드.
        process() 호출이 g_threshold를 바꿀 수 있다고 가정하므로 캐싱 불가 */
for (i = 0; i < n; i++) {
    if (v[i] > g_threshold) process(v[i]);
}

/* GOOD: 레지스터에 유지. 루프 불변식 이동·벡터화도 가능해짐 */
const int threshold = g_threshold;
for (i = 0; i < n; i++) {
    if (v[i] > threshold) process(v[i]);
}
```

**적용 대상:** 설정값, 임계값, 플래그, 크기 상수 등 루프 중 변하지 않는 전역 전부.

### GLOB-02 — 인접 선언된 전역의 false sharing을 제거한다 (매우 흔함)

**같은 번역 단위에 연달아 선언된 전역은 링커가 메모리상 인접하게 배치한다.** 따라서 논리적으로 무관한 두 전역이 같은 64B 캐시 라인에 들어가고, 서로 다른 스레드가 각각을 갱신하면 라인이 코어 간에 계속 왕복한다.

```c
/* BAD: 두 카운터가 같은 라인에 놓일 수 있음 */
uint64_t g_scan_count;      /* 스캔 스레드가 갱신 */
uint64_t g_flush_count;     /* 플러시 스레드가 갱신 */
/* → 논리적으로 완전 독립인데 성능은 락을 공유한 것처럼 붕괴 */

/* GOOD: 각각 라인 단위로 격리 */
CACHE_ALIGNED uint64_t g_scan_count;
CACHE_ALIGNED uint64_t g_flush_count;
```

**진단:**

```bash
nm -S --size-sort binary | grep -E 'g_scan_count|g_flush_count'
# 두 심볼 주소 차이가 64 미만이면 같은 캐시 라인
objdump -t binary | sort -k1 | less    # .bss/.data 배치 확인
perf c2c record ./binary && perf c2c report   # 실제 라인 경쟁 확인
```

MEM-03(구조체 배열의 false sharing)과 원인은 같지만, **전역은 선언 위치만으로 발생하므로 발견이 훨씬 어렵다.**

### GLOB-03 — read-mostly 전역은 `const`로 선언한다

읽기 전용이면 `.rodata`에 배치되어 모든 코어가 깨끗하게 공유하며 코히런시 트래픽이 없다. 반면 **쓰기가 한 번이라도 섞이면 그 쓰기가 모든 코어의 캐시 라인을 무효화**한다.

```c
/* BAD: 초기화 후 안 바뀌지만 컴파일러는 모름 */
int g_page_size = 4096;

/* GOOD */
const int g_page_size = 4096;          /* .rodata */
static constexpr int PAGE_SIZE = 4096; /* C++: 아예 컴파일 시점 상수 */
```

### GLOB-04 — `volatile`을 스레드 동기화 목적으로 쓰지 않는다

`volatile`은 **재정렬을 막지 않고, 원자성도 보장하지 않으며, 캐시 일관성도 제공하지 않는다.** 매 접근마다 메모리 재로드만 강제해서 성능만 떨어진다.

```c
/* BAD: 동기화가 안 되며 느리기만 하다 */
volatile bool g_stop;

/* GOOD */
std::atomic<bool> g_stop;
g_stop.load(std::memory_order_relaxed);
```

`volatile`의 정당한 용도는 **MMIO 레지스터, 시그널 핸들러가 쓰는 변수(`sig_atomic_t`), setjmp 경계를 넘는 변수** 뿐이다.

### GLOB-05 — `thread_local` 접근 비용을 인식하고 포인터를 캐싱한다

TLS 접근은 지역 변수보다 비싸다. 비용은 TLS 모델에 따라 크게 다르다.

| 모델 | 상황 | 비용 |
|---|---|---|
| initial-exec | 실행 파일에 정적 링크 | 세그먼트 기준 오프셋 1회 → 매우 저렴 |
| local-exec | 같은 모듈 내 | 저렴 |
| **global-dynamic** | `dlopen` 가능한 `.so` | **`__tls_get_addr()` 함수 호출** |

```c
/* BAD: 루프마다 TLS 조회 */
for (size_t i = 0; i < n; i++) tls_stats.rows++;

/* GOOD: 포인터를 한 번만 얻는다 */
stats_t *st = &tls_stats;
for (size_t i = 0; i < n; i++) st->rows++;
```

**빌드 옵션:** `-ftls-model=initial-exec` — 해당 공유 라이브러리를 `dlopen`하지 않는 경우에만 안전하다. 플러그인·확장 모듈이면 쓰지 말 것.

### GLOB-06 — 핫루프에서 `errno`를 읽지 않는다

`errno`는 TLS 매크로(`*__errno_location()`)라 매 접근이 TLS 조회다.

```c
/* BAD: 실패가 드문데도 매 반복 TLS 접근 */
for (...) { r = op(); if (errno) handle(); }

/* GOOD: 반환값으로 판단하고 errno는 실패 경로에서만 */
for (...) { r = op(); if (UNLIKELY(r < 0)) handle(errno); }
```

### GLOB-07 — 함수 지역 `static`의 스레드 안전 가드를 인식한다 (C++)

C++11 이후 함수 지역 `static`은 스레드 안전 초기화가 **보장**되며, 이를 위해 컴파일러가 **매 진입 시 가드 변수를 검사**한다. 초기화가 끝난 뒤에도 가드 바이트의 원자적 로드가 남는다.

```cpp
/* BAD: 매 호출마다 가드 검사 (핫패스에서 누적) */
const lookup_table& get_table() {
    static lookup_table t = build_table();
    return t;
}

/* GOOD 1: 시작 시 명시적으로 1회 초기화, 이후 무검사 접근 */
static lookup_table *g_table = nullptr;   /* 서버 부팅 시 초기화 */
inline const lookup_table& get_table() { return *g_table; }

/* GOOD 2: 컴파일 시점 상수라면 constexpr */
static constexpr auto TABLE = make_table();   /* C++20 */
```

`-fno-threadsafe-statics`로 가드를 제거할 수 있으나, **초기화 경합이 없음을 보장할 때만** 사용한다.

### GLOB-08 — 공유 라이브러리의 전역 접근은 GOT 간접 참조를 거친다

PIC 코드에서 외부에 보이는 전역 접근은 GOT(Global Offset Table)를 경유하므로 로드가 한 단계 추가된다.

```bash
-fvisibility=hidden           # 기본을 hidden으로. 내부 심볼은 직접 접근
-Wl,-Bsymbolic-functions      # 모듈 내 참조를 내부 바인딩
```

```c
__attribute__((visibility("default"))) void public_api(void);   /* 공개 API만 노출 */
```

심볼 수가 줄어 링크·로드 시간도 짧아진다.

### GLOB-09 — 전역 카운터·통계는 샤딩한다

서버 전역 통계 카운터는 모든 스레드가 때리는 단일 지점이다. 코어 수만큼 샤드로 분산하고 읽을 때 합산한다.

```c
#define STAT_SHARDS 64
/* 각 샤드를 독립 캐시 라인에 */
typedef struct { uint64_t v; char _pad[CACHE_LINE - sizeof(uint64_t)]; } shard_t;
static shard_t g_stat[STAT_SHARDS] CACHE_ALIGNED;

static inline void stat_inc(int shard_hint) {
    g_stat[shard_hint & (STAT_SHARDS - 1)].v++;    /* 원자적 연산 불필요 */
}
static uint64_t stat_read(void) {
    uint64_t s = 0;
    for (int i = 0; i < STAT_SHARDS; i++) s += g_stat[i].v;
    return s;   /* 근사 스냅샷. 정확한 시점 일관성이 필요하면 별도 처리 */
}
```

`shard_hint`는 스레드 ID나 `sched_getcpu()`를 쓴다. **쓰기가 O(1) 무경쟁, 읽기가 O(샤드수)** 로 뒤바뀌므로 읽기가 드문 통계에 적합하다.

### GLOB-10 — 전역 상태는 인라이닝·벡터화·재정렬을 차단한다

컴파일러는 임의의 함수 호출이 전역을 수정할 수 있다고 가정한다. 따라서 루프 불변식 이동, 벡터화, 로드/스토어 재정렬이 모두 막힌다.

```c
/* BAD: g_config 접근 때문에 루프 최적화가 대부분 차단 */
void scan(row_t *rows, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (rows[i].key > g_config.min_key) g_stats.hits++;
}

/* GOOD: 전역을 진입부에서 걷어내고 순수 루프로 만든다 */
void scan(row_t *rows, size_t n) {
    const int64_t min_key = g_config.min_key;   /* 지역화 */
    uint64_t hits = 0;                          /* 지역 누적 */
    for (size_t i = 0; i < n; i++)
        hits += (rows[i].key > min_key);         /* 벡터화 가능 */
    g_stats.hits += hits;                        /* 1회 반영 */
}
```

**일반 원칙:** 핫루프는 **전역을 인자로 받고 결과를 반환하는 순수 함수**로 분리한다. 전역 읽기는 진입부에, 전역 쓰기는 종료부에 모은다.

---

## 9. 병렬 (PAR)

### PAR-01 — 원자적 연산을 스레드 로컬 누적으로 대체한다

```cpp
/* BAD: 매 행 원자적 증가 — 경쟁 시 100~1000 사이클 */
std::atomic<uint64_t> total{0};
for (auto& r : rows) { process(r); total.fetch_add(1); }

/* GOOD: 로컬 누적 후 1회 병합 */
uint64_t local = 0;
for (auto& r : rows) { process(r); local++; }
total.fetch_add(local, std::memory_order_relaxed);
```

**규칙:** 루프 본문에 원자적 연산이 있으면 무조건 검토 대상.

### PAR-02 — 메모리 순서를 필요 최소로 지정한다

| 용도 | 순서 | 비용 |
|---|---|---|
| 독립 카운터·통계 누적 | `relaxed` | 최소 |
| 취소 플래그 폴링 | `relaxed` | 최소 |
| 데이터 발행 (생산자) | `release` | 중 |
| 데이터 획득 (소비자) | `acquire` | 중 |
| 순차 일관성 필요 | `seq_cst` | 최대 (기본값) |

```cpp
counter.fetch_add(1, std::memory_order_relaxed);
ready.store(true, std::memory_order_release);        /* 앞선 쓰기를 발행 */
if (ready.load(std::memory_order_acquire)) use();    /* 이후 읽기를 보장 */
```

**주의:** 기본값 `seq_cst`는 가장 안전하고 가장 비싸다. 완화는 정확성 근거가 명확할 때만.

### PAR-03 — 취소 검사는 배치 단위로 한다

```cpp
std::atomic<bool> abort{false};
/* BAD: 매 행 검사 */
for (auto& r : rows) { if (abort.load()) return; process(r); }

/* GOOD: 페이지/블록 단위 검사 */
for (size_t p = 0; p < npages; p++) {
    if (UNLIKELY(abort.load(std::memory_order_relaxed))) break;
    scan_page(p);     /* 내부에서는 검사하지 않음 */
}
```

### PAR-04 — 워크 분할은 캐시 라인 경계에 맞춘다
스레드별 범위가 같은 캐시 라인을 공유하면 false sharing이 발생한다. 청크 시작을 64B(또는 페이지) 경계로 정렬한다.

### PAR-05 — 락 범위를 최소화하고, 락 안에서 할당·I/O를 하지 않는다

```cpp
/* BAD */
{ std::lock_guard g(mtx); auto v = build_large_result(); results.push(v); }

/* GOOD */
auto v = build_large_result();                       /* 락 밖에서 준비 */
{ std::lock_guard g(mtx); results.push(std::move(v)); }   /* 최소 구간만 */
```

### PAR-06 — 병렬화가 이득인지 먼저 확인한다
스레드 생성·동기화·병합 비용이 있으므로, 작업량이 작으면 직렬이 빠르다. 워커 수·페이지 수 하한을 두고 미달 시 직렬 폴백한다.

```c
if (npages < MIN_PAGES_FOR_PARALLEL || avail_workers < 2)
    return scan_serial(...);
```

### PAR-07 — 워커 실패를 전파하고 형제를 조기 종료시킨다
한 워커가 실패하면 나머지가 낭비 작업을 계속한다. 공유 abort 플래그 + 에러 코드 수집으로 조정자에게 전파한다.

### PAR-08 — 워커 스레드의 스레드 로컬 상태를 반드시 정리한다
연결 엔트리, 트랜잭션 인덱스, 에러 컨텍스트가 잔류하면 다음 작업에서 오작동한다. RAII로 보장한다.

### PAR-09 — NUMA 배치를 고려한다 (멀티 소켓)

Linux는 **first-touch 정책**을 쓴다. 페이지는 `malloc` 시점이 아니라 **처음 접근한 스레드가 속한 노드**에 할당된다. 원격 노드 접근은 로컬의 1.5~2배 지연.

```c
/* BAD: 마스터 스레드가 전체를 초기화 → 모든 페이지가 노드 0에 */
buf = malloc(size);
memset(buf, 0, size);
/* 이후 노드 1의 워커는 전부 원격 접근 */

/* GOOD: 각 워커가 자기 담당 구역을 직접 first-touch */
buf = malloc(size);
parallel_for(0, nworkers, [&](int w) {
    auto [lo, hi] = worker_range(w, size);
    memset(buf + lo, 0, hi - lo);       /* 자기 노드에 할당됨 */
});
```

**보조 조치:**

```bash
numactl --hardware                                  # 노드 구성 확인
numactl --cpunodebind=0 --membind=0 ./server        # 단일 노드로 실험
```

```c
numa_alloc_onnode(size, node);                      /* 명시적 노드 배치 */
pthread_setaffinity_np(...);                        /* 스레드 핀닝으로 마이그레이션 방지 */
```

스레드가 노드 간을 옮겨다니면 first-touch로 잘 배치한 것도 무의미해지므로, **핀닝과 함께 써야 효과가 있다.**

### PAR-10 — read-mostly 데이터에 맞는 동기화를 선택한다

`mutex`는 읽기 위주 데이터에 과하다. 반면 `rwlock`도 읽기 측이 공유 카운터를 갱신하므로 코어 수가 늘면 그 자체가 병목이 된다.

| 방식 | 적합 상황 | 읽기 비용 | 주의 |
|---|---|---|---|
| `mutex` | 읽기:쓰기 균형 | 높음 | 기본 선택 |
| `rwlock` | 읽기 10:1 이상 | 중 (공유 카운터 갱신) | 코어 많으면 카운터가 병목 |
| **seqlock** | 읽기 압도적, 임계구역 짧음 | **낮음 (쓰기 없음)** | 읽기 재시도 필요, 포인터 읽기 불가 |
| **RCU / epoch** | 읽기 거의 무료여야 함 | **최소** | 해제 지연·grace period 관리 |
| per-CPU + 합산 | 카운터·통계 | 쓰기 최소 | 읽기가 O(코어수) |
| **COW 포인터 스왑** | 드물게 갱신되는 설정·메타데이터 | **최소 (원자적 로드 1회)** | 구 버전 해제 시점 관리 |

COW 패턴은 구현이 단순하고 효과가 커서 설정·카탈로그 캐시에 특히 유용하다.

```cpp
std::atomic<const config_t*> g_config;

/* 읽기: 원자적 로드 1회. 락 없음 */
const config_t *c = g_config.load(std::memory_order_acquire);
use(c->min_key);

/* 쓰기: 새 객체를 만들고 포인터만 교체 */
config_t *nc = new config_t(*old);  nc->min_key = v;
g_config.store(nc, std::memory_order_release);
/* 구 객체는 모든 독자가 떠난 뒤 해제 (epoch/RCU 또는 지연 해제 큐) */
```

### PAR-11 — 할당자 경쟁을 확인한다

`malloc`은 내부 락 또는 아레나를 쓴다. 멀티스레드 할당 폭주는 할당자에서 직렬화되어, 코어를 늘려도 성능이 안 오르는 전형적 원인이 된다.

**진단:** `perf record` 후 `malloc`/`free`/`_int_malloc`이 상위에 뜨는지, 또는 락 관련 심볼이 보이는지 확인.

**대응 순서:**
1. 할당 자체를 제거 (ALLOC-01, ALLOC-03) — 근본 해결
2. 스레드별 arena 사용
3. 스레드 캐싱 할당자로 교체: `tcmalloc`, `jemalloc`, `mimalloc` (링크만 바꿔도 효과가 큰 경우가 많다)

### PAR-12 — 스핀과 커널 대기를 상황에 맞게 쓴다

임계구역이 수백 사이클 이내면 스핀이 유리하고, 그보다 길면 커널 대기가 유리하다. 스핀 시에는 `pause` 명령으로 하이퍼스레드 형제에게 자원을 양보하고 전력·경쟁을 줄인다.

```c
static inline void cpu_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#endif
}

for (int i = 0; i < SPIN_LIMIT; i++) {          /* 적응적 스핀 */
    if (try_lock()) return;
    cpu_relax();
}
lock_slow_path();                                /* 그 다음 커널 대기 */
```

**주의:** `pause` 없는 순수 스핀은 하이퍼스레드 형제의 성능을 크게 떨어뜨린다.

### PAR-13 — 스레드 수를 워크로드와 코어 수에 맞춘다

코어 수보다 많은 실행 스레드는 컨텍스트 스위치와 캐시 오염만 늘린다. 통계·유지보수 작업용 스레드 상한은 논리 코어 수와 대상 페이지 수로 클램프한다.

```c
int workers = min3(requested, num_online_cpus(), (int)(npages / MIN_PAGES_PER_WORKER));
if (workers < 2) return scan_serial(...);        /* PAR-06 */
```

### PAR-14 — 직렬 경로에 넣은 최적화를 병렬 경로에도 넣었는지 확인한다

병렬 실행이 **별도의 전용 루프**를 쓰는 구조라면, 직렬 경로만 개선하고 병렬 경로를 빠뜨리기 쉽다.
"같은 질의가 병렬로 돌면 개선이 사라진다", "특정 형태의 질의만 중립이다"라는 증상으로 나타난다.
개선 지점마다 **직렬/병렬 양쪽 소비처를 목록화**해 확인하고, 목록을 커밋 메시지나 주석에 남긴다.


### PAR-15 — 단일 생산자 큐에는 락도 CAS도 필요 없다 (사전 할당 링 버퍼)

생산자-소비자 구조(WAL append, 로그 버퍼, 통계 수집 파이프라인)에서 **생산자가 하나면**
사전 할당 링 버퍼 + 시퀀스 번호 + **메모리 배리어만으로** 가시성이 보장된다(LMAX Disruptor
원리). 락 기반 큐 대비 실측 38~55%, 이벤트 수가 늘수록 격차 확대(`저지연패턴-HFT논문.md` 4장).

- 락이 비싼 진짜 이유는 lock 명령(~25ns)이 아니라 **경합 중재가 컨텍스트 스위치로 이뤄지며
  캐시가 무효화·재적재되는 것**이다.
- **배칭이 공짜로 따라온다**: 소비자가 커서를 봤을 때 여러 칸 전진해 있으면 그 지점까지
  동기화 개입 없이 몰아서 처리 → 버스트에서 자연스럽게 따라잡는다.
- 다중 생산자면 슬롯 클레임에만 CAS를 쓴다. head/tail/size 를 공유 변수로 두는 bounded
  queue는 쓰기 경합이 그 변수들에 집중된다(MEM-03 false sharing과 결합하면 최악).
- 지연 분산도 작아진다(MEAS-07): 실측 σ 453,766ns → 53,600ns.
- ⚠ [[저수준-검증된코드-재사용]] 규칙 적용 대상이다 — **이걸 손으로 새로 짜지 말고**
  기존 검증된 구현(엔진 내 lockfree 큐 등)을 먼저 찾는다. 이 규칙은 리뷰 판단 기준이다.

### PAR-16 — 대기 전략은 지연↔CPU 트레이드오프다. 명시적으로 고른다

이벤트가 없을 때 소비자가 어떻게 기다리는가는 설계 결정이다:

| 전략 | 지연 | CPU | 적합 |
|---|---|---|---|
| busy-spin | 최저 | 코어 하나를 태움 | 전용 코어가 있는 극핫 경로 |
| spin N회 → yield | 낮음 | 중 | 일반 핫 경로 (하이브리드) |
| condvar/sleep | 높음 (wakeup 지연) | 최소 | 콜드·백그라운드 |

- "빠르게"만 요구하고 CPU 비용을 안 적은 설계는 미완성이다. busy-spin은 **CPU를 태워
  지연을 사는 것** — 다른 워커의 코어를 빼앗으면 시스템 전체로는 손해일 수 있다.
- DB 대응: 로그 flush 대기, 워커 풀 잡 대기, 래치 spin 한도. CUBRID의 spin-then-block
  파라미터들이 이 축 위에 있다.

---

## 10. C++ 언어 기능 비용 (CPP)

C++ 추상화는 대부분 무료지만, **참조 카운팅·타입 조회·예외**는 예외다. 특히 멀티스레드에서 비용이 급증한다.

### CPP-01 — `shared_ptr`을 핫패스에서 복사하지 않는다 (멀티스레드 최대 함정)

복사·소멸마다 **원자적 참조 카운트 증감**이 일어난다. 여러 스레드가 **같은 객체**의 `shared_ptr`을 복사하면 카운터가 든 캐시 라인이 코어 간에 계속 왕복하므로, **사실상 락과 동일한 비용**이 된다. 논리적으로는 읽기 전용 공유인데 성능은 쓰기 경쟁이 되는 것이 문제다.

```cpp
/* BAD: 호출마다 원자적 증감 2회 + 라인 경쟁 */
void process(std::shared_ptr<Node> n);
for (auto& n : nodes) process(n);

/* BAD: 범위 for에서 복사 */
for (std::shared_ptr<Node> n : nodes) { ... }

/* GOOD: 소유권 이전이 아니면 참조 또는 raw 포인터 */
void process(const Node& n);
void observe(const Node* n);
for (const auto& n : nodes) process(*n);
```

**규칙:**
- 함수가 소유권을 **공유**하면 `shared_ptr` (값)
- 소유권을 **이전**하면 `unique_ptr` (값) 또는 `shared_ptr&&`
- **관찰만** 하면 `const T&` 또는 `const T*` ← 대부분 이 경우
- 컨테이너 순회는 항상 `const auto&`

`weak_ptr::lock()`도 원자적 연산을 수반하므로 루프 안에서 반복 호출하지 않는다.

### CPP-02 — 불필요한 복사를 제거한다

```cpp
/* 인자: 큰 객체는 const& */
void f(const std::vector<row>& v);          /* not std::vector<row> v */

/* 반환: 값 반환 + RVO에 맡긴다. std::move로 감싸면 RVO를 방해한다 */
std::vector<row> build() {
    std::vector<row> v;  ...;
    return v;                                /* not return std::move(v); */
}

/* 루프: auto는 복사한다 */
for (const auto& r : rows) { ... }           /* not for (auto r : rows) */

/* 삽입: emplace로 임시 객체 생성 회피 */
v.emplace_back(a, b, c);                     /* not v.push_back(T(a,b,c)); */

/* 맵 조회: 중복 탐색 제거 */
if (auto it = m.find(k); it != m.end()) use(it->second);   /* not count()+[] */
```

### CPP-03 — 예외는 예외적 상황에만 쓴다

던지지 않으면 비용이 거의 없으나(zero-cost 모델), **던지는 순간 마이크로초 단위**다. 정상 제어 흐름(예: "찾지 못함")에 쓰지 않는다.

```cpp
void scan_page(page_t*) noexcept;   /* noexcept면 정리 코드 생성이 생략된다 */
```

`noexcept`는 이동 연산자에 특히 중요하다. `std::vector` 재할당 시 이동이 `noexcept`가 아니면 복사로 폴백한다.

### CPP-04 — `dynamic_cast`와 RTTI를 핫패스에서 피한다

타입 계층 순회가 필요해 50~500 사이클이 든다. 태그 필드 + `static_cast`로 대체한다.

```cpp
/* BAD */
if (auto *p = dynamic_cast<HeapScan*>(node)) { ... }

/* GOOD: 자체 종류 태그 */
enum node_kind { NK_HEAP_SCAN, NK_INDEX_SCAN, ... };
if (node->kind == NK_HEAP_SCAN) { auto *p = static_cast<HeapScan*>(node); ... }
```

### CPP-05 — 대량 I/O에 iostream을 쓰지 않는다

로케일·포맷 처리 오버헤드가 크다. 대량 처리는 `read`/`write` 또는 `fread`/`fwrite`.

```cpp
std::ios::sync_with_stdio(false);   /* C stdio 동기화 해제 */
out << data << '\n';                /* std::endl은 매번 flush → 절대 금지 */
```

### CPP-06 — 가상 상속·다중 상속을 성능 경로에서 피한다
포인터 조정(thunk)과 추가 간접 참조가 생긴다. 상속 계층은 얕게 유지한다.

### CPP-07 — 컴파일 시점에 확정 가능한 것은 확정한다

```cpp
constexpr size_t BUCKETS = 1024;                 /* 런타임 로드 제거 */

template<bool Parallel>                          /* 런타임 분기 제거 */
void collect(...) {
    if constexpr (Parallel) { ... } else { ... }
}
```

`if constexpr`는 선택되지 않은 분기를 컴파일에서 아예 제외하므로 코드 크기와 분기를 동시에 줄인다.

### CPP-08 — 컨테이너 소멸 비용을 인식한다

소멸자가 있는 객체 수백만 개는 소멸 자체가 O(n) 비용이다. trivially destructible 타입 + arena로 설계하면 소멸이 사실상 무료가 된다.

```cpp
static_assert(std::is_trivially_destructible_v<sample_t>);   /* 보장 명시 */
```


### CPP-09 — 반복 호출되는 가상 함수는 컴파일 타임 디스패치로 바꿀 수 있다

vtable 조회 + 간접 호출은 호출 자체 비용보다 **인라인·상수 전파가 막히는 것**이 더 크다.
호출 대상이 컴파일 타임에 정해진다면 템플릿/CRTP/오버로딩으로 옮긴다.
실측 2.60ns → 1.92ns (~26%, `저지연패턴-HFT논문.md` 2.2).

- **적용 전제**: 런타임 다형성이 실제로 필요 없을 때만. 타입이 런타임에 정해지면 해당 없다.
- ⚠ **`.c` 파일에는 제안하지 않는다** (사용 규약). CUBRID 핫패스의 상당수가 C다 —
  거기서는 함수 포인터 테이블을 루프 밖으로 빼는 BR-06이 대응 수단이다.
- 템플릿화는 **코드 팽창 → I-cache 압박**을 부른다. 인스턴스가 소수일 때만 이득이다
  (CPP-03·CC-02와 같은 트레이드오프).

---

## 11. C 저수준 기법 (CLOW)

모던 C++ 추상화 없이, C 그 자체에서 쓰는 기법들. 이식성이 낮은 항목은 표시했다.

### CLOW-01 — 파일 내부 심볼에 `static`을 붙인다

컴파일러가 호출부를 전부 알게 되므로 인라이닝, 상수 전파, 미사용 제거, 레지스터 전달 최적화가 가능해진다. GOT/PLT 간접 참조도 우회한다 (GLOB-08).

```c
static int  compute_ndv(const col_t *c);   /* 외부 노출 불필요하면 항상 static */
static uint64_t g_local_counter;
```

**규칙:** 헤더에 선언할 필요가 없는 모든 함수·변수는 `static`. 이건 노력 대비 이득이 가장 큰 기계적 개선이다.

### CLOW-02 — 유연 배열 멤버로 이중 참조를 없앤다

```c
/* BAD: 헤더와 데이터가 분리 → 포인터 추적 1회 + 할당 2회 */
struct blob { size_t len; char *data; };

/* GOOD: 단일 할당, 연속 배치, 캐시 친화적 */
struct blob { size_t len; char data[]; };            /* C99 */
struct blob *b = malloc(sizeof *b + len);
b->len = len;
```

히스토그램 버킷 배열, 가변 길이 레코드, 직렬화 버퍼에 적합하다.

### CLOW-03 — 인터프리터 디스패치 루프에는 computed goto를 쓴다 (GCC/Clang 확장)

`switch` 디스패치는 **단일 간접 분기**라 분기 예측기가 하나의 이력만 학습한다. computed goto는 분기 지점이 명령별로 분산되어 예측 정확도가 크게 오른다. 표현식 평가기·VM 루프에서 10~30% 개선이 보고되는 기법이다.

```c
#if defined(__GNUC__)
  #define DISPATCH_TABLE  static void *tbl[] = { &&OP_ADD, &&OP_SUB, &&OP_LOAD, &&OP_HALT }
  #define NEXT()          goto *tbl[(++pc)->op]
  #define OP(name)        OP_##name:
#else
  /* 이식 폴백: switch 루프 */
#endif

int eval(insn_t *pc) {
    DISPATCH_TABLE;
    NEXT();
    OP(ADD)  { st[sp-1] += st[sp]; sp--; NEXT(); }
    OP(SUB)  { st[sp-1] -= st[sp]; sp--; NEXT(); }
    OP(LOAD) { st[++sp] = mem[pc->arg];  NEXT(); }
    OP(HALT) return st[sp];
}
```

**주의:** 이식성이 없으므로 `switch` 폴백을 반드시 함께 유지한다. 코드 크기가 커져 I-cache 압박이 생길 수 있으므로 실측 필수.

### CLOW-04 — 에러 처리는 `goto`로 단일 출구를 만든다

중첩 `if`를 없애고 정리 코드를 한 곳에 모으면 **핫 경로가 분기 없는 직선**이 되어 I-cache 효율과 분기 예측이 좋아진다.

```c
int collect(ctx_t *ctx) {
    int r = ER_FAILED;
    void *res = NULL, *sk = NULL;

    if (!(res = reservoir_create(cap)))     goto exit;
    if (!(sk  = hll_create(P)))             goto exit;
    if (scan_heap(ctx, res, sk) != NO_ERROR) goto exit;
    /* 핫 경로: 여기까지 분기 없이 직선 진행 */
    r = NO_ERROR;
exit:
    hll_destroy(sk);
    reservoir_destroy(res);
    return r;
}
```

**규칙:** 정리 순서는 생성의 역순. `free(NULL)`이 안전하므로 초기화를 `NULL`로 하면 분기 없이 정리된다.

### CLOW-05 — 작은 구조체는 값으로 전달한다

x86-64 SysV ABI에서 **16바이트 이하 구조체는 레지스터로 전달**된다. 포인터로 넘기면 오히려 메모리 왕복과 앨리어싱 가정이 생긴다.

```c
typedef struct { int64_t lo, hi; } range_t;   /* 16 B */
bool in_range(range_t r, int64_t v);          /* GOOD: 레지스터 2개로 전달 */
bool in_range(const range_t *r, int64_t v);   /* BAD: 메모리 경유 + 앨리어싱 */
```

17바이트 이상은 메모리로 전달되므로 그때부터 포인터가 낫다.

### CLOW-06 — 런타임 상수 나눗셈은 곱셈-시프트로 치환한다

컴파일 시점 상수 나눗셈은 컴파일러가 자동 치환한다. **런타임에 정해지고 이후 반복 사용되는 제수**는 직접 역수를 준비한다.

```c
/* 2의 거듭제곱이면 즉시 치환 */
if (IS_POW2(d)) { q = x >> __builtin_ctzll(d); r = x & (d - 1); }

/* 일반 제수: 마법수를 1회 계산해 재사용 (libdivide 기법) */
typedef struct { uint64_t magic; uint8_t more; } divisor_t;
divisor_t dv = divisor_init(d);        /* 루프 밖에서 1회 */
for (i = 0; i < n; i++) out[i] = divide_by(in[i], &dv);   /* mulhi + shift */
```

나눗셈 20~100 사이클 → 곱셈+시프트 5~8 사이클. 히스토그램 버킷 계산처럼 같은 제수를 수백만 번 쓰는 경로에서 효과가 크다.

### CLOW-07 — 인덱스 타입을 하나로 통일한다

`int`와 `size_t`를 섞으면 매 반복 부호 확장(`movsxd`) 명령이 생기고, 부호 있는 오버플로 UB 가정 차이로 벡터화 여부가 달라진다.

```c
/* BAD: i는 int, 배열 인덱싱은 size_t → 매 반복 부호 확장 */
for (int i = 0; i < (int)n; i++) sum += buf[i];

/* GOOD: 하나로 고정 */
for (size_t i = 0; i < n; i++) sum += buf[i];
```

### CLOW-08 — VLA와 `alloca`를 쓰지 않는다

크기가 컴파일 시점에 결정되지 않으면 프레임 포인터 조정 코드가 생기고, 스택 오버플로 위험이 있으며(공격 표면), 최적화도 나빠진다. 상한이 작으면 고정 배열, 크면 arena를 쓴다.

```c
/* BAD */
void f(size_t n) { char buf[n]; ... }

/* GOOD */
void f(size_t n) {
    char stack_buf[256];
    char *buf = (n <= sizeof stack_buf) ? stack_buf : arena_alloc(a, n, 1);
    ...
}
```

### CLOW-09 — `setjmp`/`longjmp`는 성능 경로에 두지 않는다

`setjmp`가 있는 함수는 지역 변수를 메모리에 두어야 하므로 **함수 전체의 레지스터 할당이 나빠진다.** 에러 처리는 `goto`(CLOW-04)와 반환 코드로 한다.

### CLOW-10 — 핫/콜드 코드를 섹션으로 분리한다

```c
__attribute__((hot))  void scan_page(page_t *p);
__attribute__((cold)) __attribute__((noinline)) void report_corruption(...);
__attribute__((section(".text.unlikely"))) static void slow_path(void);
```

핫 함수들이 연속 배치되면 I-cache·I-TLB 미스가 줄어든다. PGO(CC-02)가 자동으로 수행하지만, PGO를 못 쓰는 환경에서는 명시가 효과적이다.

### CLOW-11 — 정렬 할당 API를 쓴다

```c
void *p = aligned_alloc(64, ALIGN_UP(size, 64));   /* C11: size는 align 배수여야 함 */
if (posix_memalign(&p, 64, size) != 0) goto oom;   /* POSIX */
```

캐시 라인 격리(COH-03), SIMD 정렬 로드(ALIAS-06), DMA에 필요하다. `malloc`은 보통 16바이트 정렬만 보장한다.

### CLOW-12 — 고정 크기 복사는 크기를 상수로 만든다

```c
memcpy(dst, src, 16);   /* 상수 → 2회 8바이트 이동으로 인라인. 함수 호출 없음 */
memcpy(dst, src, n);    /* 변수 → libc 호출 */
```

레코드 크기가 몇 가지로 제한되면 크기별 분기로 상수화하면 이득이 있다.

```c
switch (rec_size) {
    case 8:  memcpy(d, s, 8);  break;
    case 16: memcpy(d, s, 16); break;
    default: memcpy(d, s, rec_size);
}
```

### CLOW-13 — 비트 연산 내장 함수를 활용한다

```c
__builtin_popcountll(x)    /* 1비트 개수. POPCNT 명령 */
__builtin_clzll(x)         /* 선행 0 개수. LZCNT — HLL rank 계산에 직접 사용 */
__builtin_ctzll(x)         /* 후행 0 개수. TZCNT */
__builtin_bswap64(x)       /* 엔디안 변환 */
```

**주의:** `x == 0`일 때 `clz`/`ctz`는 정의되지 않는다. 반드시 사전 검사하거나 `x | 1` 같은 보정을 넣는다.

```c
/* HyperLogLog rank: 선행 0 개수 + 1 */
static inline int hll_rank(uint64_t w, int p) {
    uint64_t rest = w << p;                  /* 상위 p비트 제거 */
    return rest ? __builtin_clzll(rest) + 1 : (64 - p + 1);
}
```

### CLOW-14 — 함수 포인터 테이블보다 스위치를 우선한다

함수 포인터 호출은 간접 분기 + 인라이닝 불가 + 앨리어싱 가정 악화를 동시에 유발한다. 종류가 소수로 고정되면 `switch`로 정적 디스패치하고, 종류가 많고 확장이 필요할 때만 테이블을 쓴다 (BR-06).

> **단, 디스패치가 실행 루프 안에서 매번 재결정되는 구조라면 반대다.** 수백 케이스 스위치를
> 트리 재귀로 매 행 통과하는 인터프리터는, 준비 단계에서 함수 포인터를 확정해 두는 편이
> 훨씬 싸다 (BR-04 확장 적용). 판단 기준은 "디스패치 결정이 루프 안에 있는가"다.

### CLOW-15 — 구조체 필드 순서로 패딩을 제거한다

```c
/* BAD: 24 B (패딩 7 + 3) */
struct s { char  a; uint64_t b; int c; };

/* GOOD: 16 B */
struct s { uint64_t b; int c; char a; };

/* 검증을 코드에 남긴다 */
_Static_assert(sizeof(struct s) == 16, "unexpected padding");
```

정렬 크기 내림차순으로 선언한다. `pahole` 도구로 기존 구조체의 패딩 홀을 찾을 수 있다.

```bash
pahole -C entry ./binary      # 구조체 레이아웃과 홀 표시
```

### CLOW-16 — `__attribute__((packed))`를 신중히 쓴다

패딩을 제거해 메모리는 줄지만, **미정렬 접근을 유발해 느려지고 일부 아키텍처에서는 트랩이 발생한다.** 원자적 필드를 넣으면 COH-08 위반이다. 디스크·네트워크 포맷에는 packed 구조체 대신 **명시적 직렬화**(SER-03)를 쓴다.

---

## 12. 자료구조 (DS)

### DS-01 — 기본 선택은 연속 배열

| 요구 | 선택 | 회피 |
|---|---|---|
| 순차 순회 | `std::vector` | `std::list`, `std::map` |
| 키 조회 (순서 불필요) | 오픈 어드레싱 해시 | `std::unordered_map` (체이닝) |
| 범위 질의 | 정렬 vector + 이진검색 | `std::map` |
| 우선순위 | `std::priority_queue` (vector 기반) | 정렬 리스트 |
| 소수 고정 요소 | `std::array` / 스택 배열 | 동적 할당 |
| 양단 삽입 | `std::deque` | `std::list` |
| 중간 삽입 빈번 | 인덱스 free list on array | `std::list` |

### DS-02 — 표준 해시 컨테이너의 한계를 인식한다

`std::unordered_map`은 규격상 버킷별 노드 체이닝이라 조회마다 포인터 추적이 발생한다. 성능 경로에서는 오픈 어드레싱 구현(flat hash)을 쓴다. 직접 구현 시 요점:

```
- 오픈 어드레싱 + 선형 탐사 (캐시 라인 내에서 해결되는 비율 높음)
- 로드 팩터 0.75 이하 유지
- 2의 거듭제곱 크기 → 모듈로를 마스크로 (& (cap-1))
- 키·값을 SoA로 분리하면 프로브 단계에서 키만 읽어 캐시 효율 상승
- 삭제는 툼스톤 또는 백워드 시프트
```

### DS-03 — 해시 함수는 분포와 속도를 함께 본다

정수는 곱셈-시프트 계열(splitmix64/mix64), 바이트열은 xxHash/wyhash 계열이 적합. 암호학적 해시(SHA 등)는 성능 경로에서 금지.

**중요:** 같은 데이터를 여러 경로(직렬/병렬)에서 처리한다면 **해시 함수를 통일**해야 병합이 무손실이다. 타입별로 함수를 명시적으로 고정한다.

```c
/* 타입별 해시 통일 — 경로가 섞여도 병합 안전 */
static inline uint64_t h_int(int64_t v)     { return mix64((uint64_t)v); }
static inline uint64_t h_double(double v)   { uint64_t b; memcpy(&b,&v,8); return mix64(b); }
static inline uint64_t h_bytes(const void *p, size_t n) { return wyhash(p, n, SEED); }
```

### DS-04 — 정렬은 표준 구현을 신뢰하되 비교자를 검증한다

`std::sort`는 대개 introsort로 잘 최적화되어 있다. 직접 구현보다 **비교자를 가볍고 올바르게** 만드는 데 집중한다 (FP-02 참조). 비교자가 인라인되지 않으면 성능이 크게 떨어지므로 람다/함수 객체를 쓰고 함수 포인터를 피한다.

### DS-05 — 근사 자료구조를 적극 고려한다

정확한 답이 불필요한 곳에서 메모리와 시간을 크게 절약한다.

| 목적 | 구조 | 특성 |
|---|---|---|
| 고유값 개수 | HyperLogLog | 고정 메모리(예: 16 KB), 상대오차 ≈ 1.04/√m |
| 원소 존재 여부 | Bloom / Cuckoo filter | 오탐만 발생, 미탐 없음 |
| 빈도 상위 | Count-Min Sketch | 과대추정 편향 |
| 분위수 | t-digest / KLL | 스트리밍 |
| 균등 표본 | Reservoir sampling (Algorithm L) | 1패스, 고정 메모리 |

**병합 가능성 규칙:** 병렬·파티션 처리 결과를 합칠 계획이면, 병합이 수학적으로 단일 처리와 동일한 구조를 선택한다.
- HLL: 레지스터별 max → 단일 스케치와 동일 (무손실)
- Reservoir: 모집단 크기 비례 층화 병합 필요 (단순 결합은 편향)
- Count-Min: 카운터별 합 → 동일

---

## 13. 문자열 (STR)

### STR-01 — 핫패스에서 무할당 뷰를 쓴다

```cpp
std::string_view s = ...;      /* 복사·할당 없음 */
if (s == other) { ... }
```

`std::string`을 값으로 주고받는 API는 핫패스에서 지적 대상. SSO 범위(구현별 15~22자)를 넘으면 힙 할당.

### STR-02 — 비교 전 정규화를 양측에 일관 적용한다

고정폭 문자 타입(CHAR)의 후행 공백, 대소문자, 콜레이션 처리는 **양쪽에 동일하게** 적용해야 자기일관성이 유지된다.

```c
static inline size_t rtrim_len(const char *s, size_t n) {
    while (n > 0 && s[n-1] == ' ') n--;
    return n;
}
/* 양측 모두 rtrim 후 비교 */
```

**주의:** 원시 바이트 비교는 빠르지만 콜레이션을 무시한다. 양측 일관되면 자기일관성은 유지되나, 사용자 기대와 다를 수 있으므로 문서화한다.

### STR-03 — 문자열 풀 + 오프셋으로 저장한다

구조체에 문자열을 인라인 배열로 넣으면 구조체가 커져 캐시 효율이 떨어진다.

```c
struct rec { uint64_t key; uint32_t name_off; uint32_t name_len; };  /* 16 B */
char *string_pool;   /* 별도 연속 버퍼 */
```

### STR-04 — 짧은 문자열 비교는 길이를 먼저 본다
길이가 다르면 즉시 불일치. `memcmp` 호출 전에 길이 비교로 걸러낸다.

---

## 14. 직렬화·레이아웃 (SER)

### SER-01 — 오프셋 기반 포맷으로 설계한다

포인터를 직렬화하지 않는다. 시작점 기준 오프셋을 저장하면 재배치·mmap이 가능하다.

```
[헤더: magic | version | count | ... ]
[고정 크기 엔트리 배열: ..., offset_to_var, len, ... ]
[가변 길이 영역: 문자열/블롭 ]
```

### SER-02 — 매직·버전·프레이밍을 검증한다

손상되거나 이전 버전 데이터를 읽으면 조용히 오작동하는 대신 명확히 실패해야 한다.

```c
if (memcmp(hdr->magic, MAGIC, 4) != 0)        goto invalid;
if (hdr->version > CURRENT_VERSION)           goto invalid;
if (hdr->count > MAX_COUNT)                   goto invalid;
/* 가변 영역 슬롯이 버퍼 범위를 벗어나는지 반드시 확인 */
if (slot.offset + slot.len > total_size)      goto invalid;
```

검증 실패 시에는 크래시가 아니라 **기본 추정치로 폴백**한다.

### SER-03 — 엔디안 안전한 접근자를 쓴다

구조체를 그대로 캐스팅해 읽지 않는다. 정렬 위반(UB)과 엔디안 문제를 동시에 유발한다.

```c
static inline uint64_t get_u64(const char *p) {
    uint64_t v; memcpy(&v, p, 8); return le64toh(v);
}
```

`memcpy`는 컴파일러가 단일 로드로 최적화하므로 비용이 없다.

### SER-04 — 헤더 크기를 고정하고 예약 필드를 둔다
향후 확장을 위해 예약 바이트를 남기면 포맷 버전을 올리지 않고 필드를 추가할 수 있다.

---

## 15. 컴파일러 (CC)

### CC-01 — 기본 플래그

```bash
-O2                 # 안전한 기본. 대부분 -O3와 차이 작음
-O3                 # 벡터화·언롤 적극. 코드 크기 증가 → I-cache 압박 가능
-march=native       # 배포 대상이 동일 아키텍처일 때만
-mtune=generic      # 배포 대상이 다양할 때
-flto               # 링크 시점 최적화. 크로스 TU 인라이닝
-fno-omit-frame-pointer   # 프로파일링 정확도 확보 (성능 손실 ~1%)
-g                  # 심볼. 릴리즈에도 포함해 두면 프로파일링 가능
```

**주의:** `-O3`가 항상 빠르지 않다. 코드 팽창으로 느려지는 경우가 있으므로 `-O2`와 실측 비교한다.

### CC-02 — PGO를 적용한다 (효과 대비 노력이 가장 좋음)

```bash
# 1차: 계측 빌드
g++ -O2 -fprofile-generate -o app.inst ...
./app.inst < representative_workload      # 대표 워크로드 필수

# 2차: 프로파일 반영 빌드
g++ -O2 -fprofile-use -fprofile-correction -o app ...
```

분기 예측 힌트, 인라이닝 결정, 핫/콜드 코드 배치가 실측 기반으로 최적화된다.
**대표성 없는 워크로드로 PGO를 하면 역효과다.**

### CC-03 — aliasing을 해제해 벡터화를 허용한다

```c
void axpy(double * restrict y, const double * restrict x, double a, size_t n);
```

C++는 `restrict`가 표준이 아니므로 `__restrict__`(GCC/Clang) 사용. 잘못 쓰면 UB이므로 실제로 겹치지 않음을 보장해야 한다. 상세는 ALIAS 절.

### CC-04 — 벡터화 여부를 확인한다

```bash
g++     -O3 -fopt-info-vec -fopt-info-vec-missed src.cpp
clang++ -O3 -Rpass=loop-vectorize -Rpass-missed=loop-vectorize src.cpp
```

**벡터화를 막는 흔한 원인:** 포인터 aliasing, 루프 내 함수 호출, 조건 분기, 비순차 접근(gather), 루프 캐리 의존성, 부호 있는 정수 오버플로 가능성, 반복 횟수 미확정.

### CC-05 — 인라이닝을 의도적으로 제어한다

```c
static inline           /* 헤더의 작은 핫 함수 */
__attribute__((always_inline))   /* 강제. 남용 시 I-cache 압박 */
__attribute__((noinline))        /* 콜드 경로를 밖으로 밀어냄 */
__attribute__((hot)) / ((cold))  /* 코드 배치 힌트 */
```

**규칙:** 콜드 경로(에러 처리)를 `noinline`으로 분리하면 핫 경로의 I-cache 효율이 오른다. 이것이 `always_inline` 남용보다 효과가 크다.

> **행당 무조건 호출 점검:** 핫루프가 매 반복 호출하는 out-of-line 함수는 그 자체가 비용이다
> (5~25 사이클 + 최적화 장벽). 함수 내부가 "대부분의 경우 아무 일도 하지 않는다"면, 조건을
> 호출 지점으로 끌어올리거나 아예 필요한 경우에만 호출하도록 특수화한다. 특히 자원 정리·
> 초기화 계열 유틸리티가 고정폭 타입에도 무조건 불리는 패턴을 확인한다.

### CC-06 — 부호 있는 정수 오버플로에 의존하지 않는다

UB이며, 컴파일러가 오버플로 검사 자체를 제거할 수 있다. 래핑이 필요하면 unsigned를 쓰거나 `__builtin_add_overflow`를 쓴다.

```c
if (__builtin_mul_overflow(a, b, &result)) goto overflow;
```

루프 인덱스에는 부호 있는 정수 또는 `size_t`를 일관되게 쓴다. 부호 혼용은 매 반복 부호 확장 명령을 유발할 수 있다 (CLOW-07).

> `volatile` + 사후 검사로 오버플로를 잡는 오래된 관용구는 메모리 왕복을 강제해 느리다.
> `__builtin_*_overflow`는 같은 검사를 플래그 레지스터로 처리하며 값을 레지스터에 유지한다.

### CC-07 — 어셈블리를 확인한다

중요한 핫 함수는 생성된 어셈블리를 직접 본다. 예상한 명령(cmov, 벡터 명령, 곱셈 치환)이 나왔는지 확인한다.

```bash
g++ -O3 -S -masm=intel -o - src.cpp | less
objdump -d --no-show-raw-insn -M intel binary | less
perf annotate -s hot_function
```

---

## 16. I/O 및 시스템 (SYS)

### SYS-01 — 시스템 콜을 배치한다
시스템 콜 1회는 500~2,000 사이클. 작은 읽기/쓰기를 반복하지 말고 버퍼링한다.

### SYS-02 — 접근 패턴을 커널에 알린다

```c
posix_fadvise(fd, off, len, POSIX_FADV_SEQUENTIAL);  /* 순차 스캔 */
posix_fadvise(fd, off, len, POSIX_FADV_WILLNEED);    /* 곧 읽을 것 */
posix_fadvise(fd, off, len, POSIX_FADV_DONTNEED);    /* 캐시 오염 방지 */
madvise(ptr, len, MADV_SEQUENTIAL | MADV_WILLNEED);
```

대량 순차 스캔에서 `DONTNEED`로 페이지 캐시를 정리하면 다른 작업의 캐시를 보호한다.

### SYS-03 — 페이지 폴트 오류를 전파한다
페이지 fix/pin 실패를 무시하면 잘못된 데이터를 읽는다. 반환값을 항상 검사한다.

### SYS-04 — fork 오버헤드가 있는 환경에서는 빌트인을 쓴다

프로세스 자원이 고갈된 상태(예: 크래시 루프)에서는 외부 명령 실행이 실패한다. 셸 빌트인만으로 진단·복구 경로를 구성한다.

```bash
# fork 없이 동작 (빌트인)
echo, printf, read, cd, kill, exec, [[ ]], while, for
# fork 필요 (고갈 시 실패)
ls, cat, ps, grep, awk, sed
```


### SYS-05 — 작은 요청-응답 소켓에는 Nagle을 끈다 (TCP_NODELAY)

Nagle 알고리즘은 작은 패킷을 모아 보내려고 **ACK를 기다린다** — 요청-응답(RPC) 패턴에서는
이 대기가 곧 왕복 지연에 더해진다. 소켓 옵션 한 줄로 제거된다:

```c
int yes = 1;
setsockopt (fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof (yes));
```

- DB 대응: **클라이언트↔브로커↔CAS↔서버가 전부 요청-응답 소켓**이다. 작은 질의가 많은
  워크로드에서 연결 경로에 NODELAY 누락이 있으면 질의당 수십 ms가 붙을 수 있다.
- 대량 스트리밍(결과셋 벌크 전송)은 반대로 Nagle이 이득일 수 있다 — 패턴별로 판단한다.
- 리뷰 포인트: 새 소켓을 여는 코드에서 이 옵션의 유무가 **의도인지 누락인지** 확인한다.

---

## 17. 안티패턴 카탈로그

즉시 지적 대상. 발견 시 규칙 ID와 함께 대안을 제시한다.

| # | 안티패턴 | 위반 | 대안 |
|---|---|---|---|
| A01 | 핫루프 안 `new`/`malloc` | ALLOC-01 | 루프 밖 예약, arena |
| A02 | `reserve()` 없는 `push_back` 루프 | ALLOC-02 | `reserve(n)` |
| A03 | 루프 본문 원자적 연산 | PAR-01 | 로컬 누적 후 병합 |
| A04 | 워커별 카운터가 인접 배열 | MEM-03 | `alignas(64)` + 패딩 |
| A05 | 부동소수점 `==` 비교 | FP-01 | 상대 epsilon |
| A06 | 성분별 비교 후 합으로 순서 결정 | FP-02 | 단일 스칼라 비교 |
| A07 | 핫루프 내 가상 함수 호출 | BR-06 | 템플릿, 루프 분리 |
| A08 | `std::list`/`std::map` 순차 순회 | DS-01 | vector, 정렬 vector |
| A09 | 루프마다 나눗셈 | FP-04 | 역수 곱 (재현성 확인) |
| A10 | 루프 안 불변 조건 검사 | BR-04 | loop unswitching |
| A11 | 구조체 캐스팅으로 직렬 데이터 읽기 | SER-03 | `memcpy` + 엔디안 변환 |
| A12 | 할당 실패를 성공으로 처리 | ALLOC-06 | 에러 전파 |
| A13 | `std::string` 값 전달 (핫패스) | STR-01 | `string_view` |
| A14 | 매 행 취소 플래그 검사 | PAR-03 | 배치 단위 검사 |
| A15 | 락 안에서 할당/I/O | PAR-05 | 락 밖에서 준비 |
| A16 | 검증 없는 외부 포맷 파싱 | SER-02 | 매직/버전/범위 검사 |
| A17 | 정확성 민감 코드에 `-ffast-math` | FP-03 | 제거 또는 함수 단위 |
| A18 | 크기 미확인 스택 배열 (VLA) | CLOW-08 | 상한 확인 또는 힙/arena |
| A19 | 대표성 없는 마이크로벤치로 결론 | MEAS-04 | 실 워크로드 크기로 재측정 |
| A20 | 병렬화 이득 확인 없이 스레드 생성 | PAR-06 | 하한 미달 시 직렬 폴백 |
| A21 | 루프 안에서 전역 변수 반복 접근 | GLOB-01 | 진입부에서 지역 변수로 복사 |
| A22 | 핫 전역을 패딩 없이 인접 선언 | GLOB-02 | `CACHE_ALIGNED` 각각 적용 |
| A23 | 동기화 목적 `volatile` | GLOB-04 | `std::atomic` |
| A24 | 루프 안 `thread_local` 반복 조회 | GLOB-05 | 포인터를 루프 밖에서 1회 획득 |
| A25 | 핫루프 내 `errno` 검사 | GLOB-06 | 반환값 판단, 실패 경로에서만 `errno` |
| A26 | 핫패스 함수 지역 `static` 접근 | GLOB-07 | 부팅 시 초기화 + raw 포인터 |
| A27 | 전역 카운터 무샤딩 | GLOB-09 | 코어별 샤드 + 읽을 때 합산 |
| A28 | 마스터 스레드가 전체 버퍼 초기화 (NUMA) | PAR-09 | 워커별 first-touch |
| A29 | 읽기 위주 데이터에 `mutex` | PAR-10 | seqlock, COW 포인터 스왑, RCU |
| A30 | 멀티스레드 할당 폭주 | PAR-11 | arena 또는 스레드 캐싱 할당자 |
| A31 | `pause` 없는 순수 스핀 | PAR-12 | `cpu_relax()` 삽입 + 적응적 스핀 |
| A32 | 핫패스 `shared_ptr` 값 전달·복사 | CPP-01 | `const T&` 또는 `const T*` |
| A33 | `for (auto x : container)` 복사 | CPP-02 | `for (const auto& x : ...)` |
| A34 | `return std::move(local)` | CPP-02 | 그냥 `return local` (RVO) |
| A35 | 정상 제어 흐름에 예외 사용 | CPP-03 | 반환값·`optional`·에러 코드 |
| A36 | 핫패스 `dynamic_cast` | CPP-04 | 종류 태그 + `static_cast` |
| A37 | 대량 출력에 `std::endl` | CPP-05 | `'\n'` + 명시적 flush |
| A38 | 이동 연산자에 `noexcept` 누락 | CPP-03 | `noexcept` 표기 (벡터 재할당 시 복사 폴백 방지) |
| A39 | 타입 캐스팅으로 비트 재해석 | ALIAS-01 | `memcpy` 또는 `union` |
| A40 | `-fno-strict-aliasing`으로 문제 은폐 | ALIAS-01 | 원인 수정 후 플래그 제거 |
| A41 | 바이트 단위 복사·비교 루프 | ALIAS-02 | `memcpy`/`memcmp`/`memset` |
| A42 | 겹치지 않는 인자에 `restrict` 누락 | ALIAS-03 | `restrict` 추가 (겹침 보장 확인 후) |
| A43 | 루프에서 구조체 필드에 직접 누적 | ALIAS-04 | 지역 변수 누적 후 1회 반영 |
| A44 | 포인터↔정수 왕복 (태그 포인터) | ALIAS-05 | 인덱스 기반 참조 |
| A45 | 스레드 간 공유되는 비트필드 | COH-07 | 원자적 바이트 또는 0폭 분리 |
| A46 | 미정렬 원자적 변수 / packed 내 원자 | COH-08 | `_Alignas` + 자연 정렬 |
| A47 | 링 버퍼 head/tail 같은 라인 | COH-06 | 라인 분리 + 사본 캐싱 |
| A48 | 배열들의 시작이 4KB 배수로 정렬 | COH-09 | 라인 단위 오프셋 부여 |
| A49 | 직접 `mfence` 삽입 | COH-10 | 표준 원자적 연산 + 메모리 순서 |
| A50 | 파일 내부 심볼에 `static` 누락 | CLOW-01 | `static` 추가 |
| A51 | 헤더+데이터 분리 구조체 | CLOW-02 | 유연 배열 멤버 |
| A52 | 중첩 `if`로 에러 처리 | CLOW-04 | `goto` 단일 출구 |
| A53 | 16B 이하 구조체를 포인터로 전달 | CLOW-05 | 값 전달 (레지스터) |
| A54 | 루프 내 런타임 상수 나눗셈 | CLOW-06 | 역수 사전 계산 |
| A55 | 인덱스에 `int`/`size_t` 혼용 | CLOW-07 | 하나로 통일 |
| A56 | VLA / `alloca` 사용 | CLOW-08 | 고정 배열 + arena 폴백 |
| A57 | 구조체 필드 순서로 인한 패딩 낭비 | CLOW-15 | 정렬 내림차순 + `_Static_assert` |
| A58 | 성능 목적 `__attribute__((packed))` | CLOW-16 | 명시적 직렬화 |
| A59 | **컴파일 타임 상수를 행 루프에서 재테스트** | BR-04 | 특수화된 커널/함수 포인터를 준비 단계에서 확정 |
| A60 | **행마다 무조건 out-of-line 호출 (대개 no-op)** | CC-05 | 조건을 호출 지점으로, 필요할 때만 호출 |
| A61 | **다중 switch 디스패치가 행 루프 안에 중첩** | BR-06 | (종류×타입×연산자) 리프를 컴파일 시 결정 |
| A62 | **매 반복 같은 값을 쓰는 스토어 (불변 포인터 재발행)** | BR-04 | 준비 단계에서 1회 |
| A63 | **직렬 경로만 최적화하고 병렬 전용 루프 누락** | PAR-14 | 소비처 목록화 후 양쪽 적용 |
| A64 | **스레드 로컬 힙 할당물을 다른 스레드가 해제** | ALLOC-08 | 전역 힙 사용 |
| A65 | 고정소수점 대량 합산에서 행당 반올림·패킹 | FP-07 | 지연 캐리 + 마감 1회 구체화 |
| A66 | `volatile` + 사후 검사로 오버플로 판정 | CC-06 | `__builtin_*_overflow` |

---

## 18. 리뷰 체크리스트

### 필수 (모든 성능 관련 변경)
- [ ] 병목 측정 데이터가 첨부되었나 (MEAS-01)
- [ ] 개선 수치가 반복 실행 중앙값인가 (MEAS-04)
- [ ] **율(rate)만이 아니라 절대량(count)도 함께 봤나** (MEAS-06)
- [ ] **중앙값과 함께 산포(MAD/표준편차)를 적었나** (MEAS-07)
- [ ] 기능 회귀 테스트를 통과했나 (MEAS-05)
- [ ] 결과가 달라질 수 있는 변경(FP 순서, 병렬, SIMD)을 명시했나

### 메모리
- [ ] 핫 구조체가 64B를 불필요하게 초과하지 않나 (MEM-02)
- [ ] 병렬 카운터에 false sharing이 없나 (MEM-03)
- [ ] 접근 패턴과 레이아웃(AoS/SoA)이 일치하나 (MEM-04)
- [ ] hot/cold 필드가 분리됐나 (MEM-05)
- [ ] 루프 안 할당이 없나 (ALLOC-01)
- [ ] 컨테이너를 사전 예약했나 (ALLOC-02)
- [ ] 크로스 스레드 해제 가능성이 있는 메모리가 전역 힙인가 (ALLOC-08)
- [ ] float/double 혼용 수식이 핫루프에 없나 (FP-08)

### 연산·분기
- [ ] 예측 불가 분기가 핫루프에 있나 (BR-03)
- [ ] 루프 불변 조건이 밖으로 나갔나 (BR-04)
- [ ] **컴파일 타임에 정해진 값을 행마다 다시 테스트하지 않나** (BR-04 / A59)
- [ ] **행마다 무조건 호출되는 out-of-line 함수가 실제로 일을 하나** (CC-05 / A60)
- [ ] **디스패치(switch/가상호출)가 루프 안에서 재결정되지 않나** (BR-06 / A61)
- [ ] **행마다 도는 조건에서 싸고 잘 거르는 것이 앞에 있나** (BR-07)
- [ ] **에러 검사가 핫 패스에 체인으로 늘어서 있지 않나** (BR-08)
- [ ] **에러 처리 함수가 `noinline`인가** (BR-08)
- [ ] **매 반복 같은 값을 쓰는 스토어가 없나** (BR-04 / A62)
- [ ] 벡터화가 실제로 됐는지 확인했나 (CC-04)
- [ ] 반복 나눗셈이 없나 (FP-04, CLOW-06)
- [ ] 오버플로 검사를 빌트인으로 하나 (CC-06)

### 병렬·시스템
- [ ] 생산자-소비자 큐에서 생산자 수에 맞는 동기화 수준인가 — 단일 생산자에 락/CAS는 과잉 (PAR-15)
- [ ] 대기 전략(spin/yield/sleep)의 CPU 비용을 명시했나 (PAR-16)
- [ ] 새로 여는 요청-응답 소켓에 TCP_NODELAY 유무가 의도인가 (SYS-05)

### 물리 설계 (리팩터링·모듈 분리 리뷰에서만)
- [ ] 헤더에 넣은 `#include`가 5가지 경우(Is-A/Has-A/Inline/Enum/Typedef)에 해당하나 (PHYS-01)
- [ ] 전방 선언으로 충분한데 헤더를 include하지 않았나 (PHYS-01)
- [ ] 전이 include에 기대고 있지 않나 (PHYS-01)
- [ ] 새 순환 의존을 만들지 않았나 (PHYS-02)
- [ ] 순환을 봉합이 아니라 escalation/demotion으로 제거했나 (PHYS-03)
- [ ] 핫패스에 추상 인터페이스를 넣어 가상 호출을 만들지 않았나 (PHYS-05 ⚠ CPP-09와 충돌 지점)
- [ ] 구현 파일이 자기 헤더를 첫 include로 넣나 (PHYS-07)

### 부동소수점
- [ ] `==` 비교가 없나 (FP-01)
- [ ] 비교자의 대칭성을 프로브로 검증했나 (FP-02)
- [ ] 비교자가 정렬에 쓰인다면 추이성을 확인했나 (FP-02 경고)
- [ ] `-ffast-math`가 정확성 민감 코드에 없나 (FP-03)
- [ ] 고정소수점 대량 합산에서 캐리를 지연했나, flush 지점을 전수 확인했나 (FP-07)

### 앨리어싱 (C — 벡터화 실패 시 최우선 확인)
- [ ] 타입 캐스팅 비트 재해석이 없나 (ALIAS-01)
- [ ] `-fno-strict-aliasing`에 의존하지 않나 (ALIAS-01)
- [ ] 바이트 루프를 `mem*` 함수로 대체했나 (ALIAS-02)
- [ ] 겹치지 않는 포인터 인자에 `restrict`를 붙였나 (ALIAS-03)
- [ ] 루프 누적을 지역 변수로 했나 (ALIAS-04)
- [ ] 벡터화 실패 메시지에 alias가 없나 (ALIAS-07)

### 캐시 코히런시 (멀티스레드)
- [ ] 캐시 라인마다 쓰는 주체가 하나인가 (COH-01, COH-04) — **최상위 규약**
- [ ] `perf c2c`에서 HITM 상위 심볼을 확인했나 (COH-02)
- [ ] 읽기 전용 데이터가 쓰기 데이터와 다른 라인인가 (COH-04)
- [ ] 스레드 간 공유 비트필드가 없나 (COH-07)
- [ ] 원자적 변수가 자연 정렬돼 있나 (COH-08)
- [ ] 링 버퍼 head/tail이 분리돼 있나 (COH-06)
- [ ] ARM 등 약한 메모리 모델에서 검증했나 (COH-10)
- [ ] 공유하는 대신 분할·이전할 수 있나 (COH-12)

### C 저수준
- [ ] 내부 심볼에 `static`을 붙였나 (CLOW-01)
- [ ] 에러 처리가 `goto` 단일 출구인가 (CLOW-04)
- [ ] 루프 내 런타임 나눗셈이 없나 (CLOW-06)
- [ ] 인덱스 타입이 통일됐나 (CLOW-07)
- [ ] VLA/`alloca`가 없나 (CLOW-08)
- [ ] 구조체 패딩을 `_Static_assert`로 고정했나 (CLOW-15)

### 전역·정적 상태
- [ ] 루프 안 전역 접근을 지역 변수로 걷어냈나 (GLOB-01)
- [ ] 핫 전역들이 같은 캐시 라인에 있지 않나 (GLOB-02) — `nm`으로 주소 확인
- [ ] read-only 전역이 `const`인가 (GLOB-03)
- [ ] 동기화 목적 `volatile`이 없나 (GLOB-04)
- [ ] `thread_local` 조회가 루프 밖으로 나갔나 (GLOB-05)
- [ ] 전역 카운터가 샤딩됐나 (GLOB-09)
- [ ] 핫루프가 전역 없는 순수 함수로 분리 가능한가 (GLOB-10)

### C++ 기능
- [ ] `shared_ptr`을 핫패스에서 복사하지 않나 (CPP-01) — **멀티스레드 최우선**
- [ ] 범위 for가 `const auto&`인가 (CPP-02)
- [ ] `return std::move(local)`이 없나 (CPP-02)
- [ ] 이동 연산자에 `noexcept`가 있나 (CPP-03)
- [ ] 정상 흐름에 예외를 쓰지 않나 (CPP-03)
- [ ] 핫패스에 `dynamic_cast`가 없나 (CPP-04)

### 병렬
- [ ] 원자적 연산이 스레드 로컬로 대체됐나 (PAR-01)
- [ ] 메모리 순서가 과도하지 않나 (PAR-02)
- [ ] 취소 검사가 배치 단위인가 (PAR-03)
- [ ] 직렬 폴백 조건이 있나 (PAR-06)
- [ ] 워커 에러가 조정자로 전파되나 (PAR-07)
- [ ] 스레드 로컬 상태가 정리되나 (PAR-08)
- [ ] 멀티 소켓이면 first-touch 배치를 고려했나 (PAR-09)
- [ ] read-mostly 데이터에 과한 동기화를 쓰지 않나 (PAR-10)
- [ ] 할당자 경쟁이 프로파일 상위에 없나 (PAR-11)
- [ ] 스핀 루프에 `cpu_relax()`가 있나 (PAR-12)
- [ ] **직렬 경로 개선을 병렬 전용 경로에도 적용했나** (PAR-14 / A63)

### 데이터·직렬화
- [ ] 근사 구조의 병합이 단일 처리와 동일한가 (DS-05)
- [ ] 직렬/병렬 경로의 해시 함수가 통일됐나 (DS-03)
- [ ] 외부 포맷 파싱에 검증이 있나 (SER-02)
- [ ] 정렬 위반 없이 읽나 (SER-03)

---

## 19. 우선순위 결정 절차

성능 문제를 받았을 때 이 순서로 진행한다. 위 단계에서 해결되면 아래로 내려가지 않는다.

```
1. 알고리즘 복잡도를 낮출 수 있나?
   → O(n²)를 O(n log n)으로 만드는 것이 다른 모든 최적화의 총합보다 크다.
   → 불필요한 작업을 아예 제거할 수 있나? (조기 종료, 캐싱, 중복 제거)

2. 접근하는 데이터 양을 줄일 수 있나?
   → 컬럼 프루닝, 압축, 근사 구조(DS-05), 필요한 필드만 읽기

3. 메모리 접근 패턴을 개선할 수 있나?
   → MEM-01 ~ MEM-07. 여기서 대부분의 실질 이득이 나온다.

4. 할당을 제거할 수 있나?
   → ALLOC-01 ~ ALLOC-05

4.5 컴파일러가 최적화를 못 하게 막는 것을 걷어낼 수 있나?
   → ALIAS-03/04: 앨리어싱 가정 해제. 벡터화 실패의 최다 원인.
   → GLOB-01, GLOB-10: 전역 지역화. 이것만으로 루프 최적화가 풀리는 경우가 많다.
   → BR-04 확장, CC-05: 실행 루프에 남은 컴파일 타임 분기·무조건 호출 제거.
   → 이 단계는 알고리즘을 바꾸지 않으므로 위험이 낮고 이득이 크다. 반드시 먼저 시도한다.
   → 멀티스레드면 GLOB-02(전역 false sharing), CPP-01(shared_ptr refcount)을 먼저 확인.
     이 둘은 코어를 늘려도 성능이 안 오르는 대표 원인이다.

5. 분기와 의존성 체인을 줄일 수 있나?
   → BR-*, ILP 향상(부분합 분할 등)

6. 병렬화할 수 있나?
   → PAR-*. 단, 3~5를 먼저 하지 않으면 나쁜 코드를 여러 코어에서 돌리는 것.
   → 스레드를 늘려도 성능이 안 오르면 순서대로 확인:
     (1) false sharing — MEM-03, GLOB-02
     (2) 원자적 참조 카운팅 — CPP-01
     (3) 할당자 경쟁 — PAR-11
     (4) 원자적 연산 경쟁 — PAR-01
     (5) NUMA 원격 접근 — PAR-09
   → 근본 대책은 COH-12: 공유를 없애고 분할 소유로 바꾼다.
     락·원자적 연산을 최적화하는 것보다 공유 자체를 제거하는 설계가 항상 낫다.
   → 병렬 전용 루프가 따로 있으면 PAR-14로 개선 누락을 확인한다.

7. 벡터화할 수 있나?
   → CC-03, CC-04. 데이터 레이아웃(MEM-04)이 먼저 정리돼야 가능.

8. 컴파일러 옵션·PGO
   → CC-01, CC-02. 코드 변경 없이 얻는 이득.

9. 수동 어셈블리/intrinsic
   → 마지막 수단. 유지보수 비용이 크므로 측정된 병목의 극소 부분에만.
```

**단계 3이 가장 중요하다.** 현대 CPU에서 DRAM 접근 1회가 정수 연산 200~400회와 맞먹으므로, 메모리 접근 패턴 개선이 연산 최적화보다 거의 항상 효과가 크다.


## 20. 물리 설계와 빌드 비용 (PHYS)

> 런타임이 아니라 **빌드·의존성 비용**의 축이다. 원전은 Lakos, *Large-Scale C++ Vol I*
> — 정리본 `notes/대규모Cpp-물리설계-Lakos.md`. 여기엔 CUBRID에서 실제로 데인 것만 뽑았다.
> 리팩터링·모듈 분리 리뷰에서 이 장을 연다. 성능 리뷰(0~19장)와 목적이 다르다.

### PHYS-01 — 헤더의 `#include`는 빌드 비용을 전파한다 (절연)

**캡슐화**(클라이언트가 코드를 안 고쳐도 됨)와 **절연**(재컴파일조차 안 해도 됨)은 다르다.
헤더에 `#include`를 넣으면 그 의존이 **전이적으로** 모든 하위 클라이언트에 퍼진다.

- 헤더에는 **그 헤더 자체를 컴파일하는 데 필요한 것만** include한다. 나머지는 `.cpp`로 민다.
- 헤더가 다른 헤더를 include해야 하는 경우는 5가지뿐: **Is-A**(public 상속) /
  **Has-A**(값 데이터 멤버) / **Inline**(인라인 본문에서 사용) / **Enum** / **Typedef**.
  그 외에는 **전방 선언**으로 충분하다.
- **전이 include에 기대지 않는다.** 다른 헤더가 대신 include해주는 것에 의존하면, 그쪽이
  리팩터링될 때 조용히 깨진다. 직접 쓰면 직접 include한다(예외: 직접 include한 타입의 public 기반 클래스).
- 효과는 "빌드가 빨라진다"에 그치지 않는다 — **핫픽스를 얼마나 빨리 낼 수 있는지가
  구현 세부의 절연 정도에 비례한다.** 인터페이스가 안 변하면 그 `.o`만 갈아 끼울 수 있다.

### PHYS-02 — 순환 의존은 어느 레벨에서도 만들지 않는다

파일·모듈·라이브러리 어느 층에서도 순환은 금지다. 순환이 있으면 **레벨 번호를 못 매기고**,
그 순간 "어디부터 테스트할 수 있는가"가 사라진다.

- 순환을 발견하면 봉합(`#ifdef`, 전방 선언 남발)하지 말고 아래 PHYS-03의 기법으로 **제거**한다.
- 링크 순서로 우회되는 순환은 특히 위험하다 — 플랫폼이 바뀌면 터진다.

### PHYS-03 — 순환을 깨는 표준 기법 (빈도순)

| 기법 | 요지 | CUBRID 대응 예 |
|---|---|---|
| **Escalation** (승격) | 상호 의존하는 기능을 **상위 새 모듈로** 올려 상호→하향 의존으로 바꾼다 | storage↔query가 서로 참조하면 그 접점을 상위 util로 |
| **Demotion** (강등) | 양쪽이 공통으로 쓰는 저수준 기능을 **하위로** 내린다 | 공통 정보 구조를 하위 헤더로 분리 |
| **Opaque pointer** | 정의 없이 **이름만으로** 포인터를 든다(전방 선언) | 자식이 부모를 역참조해 생기는 순환 |
| **Dumb data** | 포인터 대신 **정수 인덱스**. 해석은 상위 관리자가 | 직렬화 가능해진다 — 페이지/슬롯 구조에 적합 |
| **Manager class** | 소유·수명 관리를 **별도 관리자 타입**에 몰아준다 | 노드가 다른 노드를 `delete`하지 않게 |
| **Redundancy** | 아주 작은 코드를 의도적으로 복제해 무거운 의존을 피한다 | **최후 수단.** lock-step 유지가 필요하면 쓰지 않는다 |

- **escalation → demotion 조합이 압도적으로 자주 쓰인다.** 먼저 이 둘을 검토한다.
- 재귀 소멸자(`~Link() { delete next; }`)는 **긴 리스트에서 스택 오버플로**를 낸다.
  계층적 소유는 런타임 규약이 아니라 **타입 시스템**이 통제해야 한다.

### PHYS-04 — 세분화는 기예다. 기계적으로 쪼개지 않는다

- 지나친 분할은 **발견성을 해친다.** "`DateUtil`을 찾아볼 사람이 `DayOfWeekUtil`이 있는지
  어떻게 아는가"가 원전이 스스로에게 건 제동이다.
- **본질적으로 primitive한 연산은 그 타입에 남긴다** — 내부 표현에 접근해야 효율적으로
  구현되는 연산이 그렇다. 밖으로 빼면 성능을 잃는다.
- **함께 변해야 하는 코드는 함께 둔다.** 템플릿과 그 특수화, 인코더와 디코더, 열거형과
  그것을 아는 유틸리티는 떨어뜨리면 매번 동시 릴리스가 강제된다.

### PHYS-05 — 무거운 leaf 의존은 위로 올린다 (layered → lateral)

하위 모듈이 무거운 설비(DB 접근, 플랫폼 API, 외부 라이브러리)에 직접 의존하면 **그 위 전부가
전이적으로** 그것에 묶인다. 결과: 모듈 하나만 떼서 단위 테스트할 수 없게 된다.

- 무거운 의존을 **추상 인터페이스로 바꾸고 구상 구현은 상위에서 주입**한다.
- 비이식(플랫폼 종속) 코드는 **최소화하고 한 곳에 격리**한다. 도메인 코드에 섞지 않는다.
- 전역 싱글톤은 재사용과 테스트를 인위적으로 막는다. 상위에서 넘겨주는 구조를 우선한다.
- ⚠ **성능 경로에서는 추상 인터페이스가 가상 호출을 낳는다**(CPP-09와 충돌). 핫패스에는
  템플릿 결합을, 콜드/초기화 경로에는 인터페이스를 쓴다. **이 장과 0~19장이 부딪히면
  핫패스에서는 성능 장이 우선한다.**

### PHYS-06 — 빌드 의존성은 측정 가능하다

체감("빌드가 느리다") 대신 숫자로 만든다. `#include` 지시문만 파싱하면 되므로 스크립트
수십 줄로 충분하다 — C++ 파싱이 필요 없다.

- 모듈 의존 그래프를 뽑아 **순환을 찾고**, 컴포넌트별 누적 의존 수(CCD)를 센다.
- 리팩터링 **전후 비교 지표**로 쓴다. MEAS-01("측정 없는 최적화 금지")이 물리 설계에도 적용된다.
- 실측 참고: 빌드 디렉터리를 옮기면 `CMakeCache`의 경로가 컴파일 커맨드라인에 박혀 있어
  **전체 재빌드 1회가 확정 비용**이다(2026-08-27 `.50` 1358 타깃, 08-28 `.52` 재현).

### PHYS-07 — `.c`/`.cpp`는 자기 헤더를 첫 실질 include로 넣는다

자기 컴포넌트의 헤더를 구현 파일 **첫 줄**(라이선스·주석 제외)에 include하면, 그 헤더가
**단독으로 컴파일되는지를 컴파일러가 매 빌드마다 검증**한다. include 순서 의존 결함 —
"A.h를 B.h보다 먼저 include해야 컴파일되는" 헤더 — 이 원천적으로 생길 수 없게 된다.

- 비용이 0이다. 순서만 바꾸면 된다. 신규 파일은 무조건, 기존 파일은 손댈 때 정리한다.
- 이게 안 지켜진 코드베이스에서는 헤더 하나를 리팩터링할 때 **어떤 순서 의존이 숨어
  있는지 알 수 없어** 수정 비용이 예측 불가가 된다.
- 테스트 코드의 의존성은 **테스트 대상의 의존성을 초과하지 않게** 한다 — 초과하는 순간
  그 테스트는 대상보다 무거운 것들이 다 빌드돼야 돌고, 결국 안 돌게 된다.


---

## 부록 A: 자주 쓰는 매크로·유틸

```c
/* 분기 힌트 */
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

/* 캐시 라인 */
#define CACHE_LINE 64
#define CACHE_ALIGNED __attribute__((aligned(CACHE_LINE)))

/* 배열 길이 (포인터에 쓰면 컴파일 에러가 나도록) */
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* 2의 거듭제곱 정렬 */
#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((a) - 1))
#define IS_POW2(x)       ((x) && !((x) & ((x) - 1)))

/* 안전한 미정렬 로드 */
static inline uint32_t load_u32(const void *p) { uint32_t v; memcpy(&v,p,4); return v; }
static inline uint64_t load_u64(const void *p) { uint64_t v; memcpy(&v,p,8); return v; }

/* 정수 해시 (splitmix64 계열) */
static inline uint64_t mix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

/* 상대 오차 기반 부동소수점 등가 */
static inline bool fp_eq_rel(double a, double b, double eps) {
    double d = fabs(a - b);
    double m = fmax(fabs(a), fabs(b));
    return d <= eps * fmax(m, 1.0);
}

/* 벤치마크에서 최적화 제거 방지 */
#define DO_NOT_OPTIMIZE(x) __asm__ volatile("" : : "r,m"(x) : "memory")

/* 스핀 대기 시 코어 양보 (PAR-12) */
static inline void cpu_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

/* 캐시 라인 격리 전역 선언 (GLOB-02) */
#define HOT_GLOBAL(type, name) CACHE_ALIGNED type name

/* 샤딩된 카운터 (GLOB-09) */
#define STAT_SHARDS 64
typedef struct { uint64_t v; char _pad[CACHE_LINE - sizeof(uint64_t)]; } stat_shard_t;
_Static_assert(sizeof(stat_shard_t) == CACHE_LINE, "shard must be one cache line");

/* 컴파일 시점 구조체 크기 검증 (MEM-02, CLOW-15) */
#define ASSERT_SIZE(T, n)   _Static_assert(sizeof(T) == (n), #T " size changed")
#define ASSERT_FITS_LINE(T) _Static_assert(sizeof(T) <= CACHE_LINE, #T " exceeds cache line")
#define ASSERT_OFFSET(T, f, n) _Static_assert(offsetof(T, f) == (n), #T "." #f " moved")

/* 컴파일러 배리어 — 명령 생성 없음, 재정렬만 방지 (COH-10) */
#define COMPILER_BARRIER() __asm__ __volatile__("" ::: "memory")

/* 안전한 타입 재해석 (ALIAS-01) — memcpy는 단일 로드로 최적화된다 */
#define BIT_CAST(dst_type, src) \
    __builtin_choose_expr(sizeof(dst_type) == sizeof(src), \
        ({ dst_type _d; memcpy(&_d, &(src), sizeof _d); _d; }), (void)0)

/* 겹침 검사 — restrict 경로 분기용 (ALIAS-03) */
#define NO_OVERLAP(a, b, n) ((const char*)(a) + (n) <= (const char*)(b) || \
                             (const char*)(b) + (n) <= (const char*)(a))

/* 0 안전 비트 연산 (CLOW-13) */
static inline int clz64_safe(uint64_t x) { return x ? __builtin_clzll(x) : 64; }
static inline int ctz64_safe(uint64_t x) { return x ? __builtin_ctzll(x) : 64; }

/* HyperLogLog rank — 상위 p비트를 제거한 뒤 선행 0 + 1 */
static inline int hll_rank(uint64_t w, int p) {
    uint64_t rest = w << p;
    return rest ? __builtin_clzll(rest) + 1 : (64 - p + 1);
}
```

---

## 부록 B: 멀티스레드 확장성 문제 진단 절차

"코어를 늘렸는데 빨라지지 않는다"는 증상의 원인을 순서대로 좁힌다.

```bash
# 0) 스레드 수별 스루풋 곡선을 먼저 그린다
for t in 1 2 4 8 16 32; do THREADS=$t ./bench; done
#   - 선형 증가 후 평탄화  → 자원 포화 (메모리 대역폭, I/O)
#   - 특정 지점 이후 하락  → 경쟁 (false sharing, 락, 원자적 연산)
#   - 처음부터 개선 없음   → 직렬 구간 지배 (암달의 법칙) 또는 락 하나에 전부 대기

# 1) 캐시 라인 경쟁 확인 (false sharing / 원자적 카운터)
perf c2c record -- ./bench
perf c2c report --stdio
#   HITM(다른 코어 캐시에서 가져옴) 상위 심볼이 범인.
#   전역 변수 이름이 보이면 GLOB-02, 구조체 배열이면 MEM-03, refcount면 CPP-01

# 2) 락 대기 확인
perf record -e sched:sched_switch -g -- ./bench     # 컨텍스트 스위치 유발 지점
perf lock record ./bench && perf lock report        # 커널 락 (지원 시)
#   futex 관련 심볼이 상위면 뮤텍스 경쟁

# 3) 할당자 확인
perf report | grep -iE 'malloc|free|arena|tcache'
#   상위권이면 PAR-11

# 4) NUMA 확인
numastat -p $(pidof bench)          # 노드별 메모리 배치
perf stat -e node-load-misses,node-store-misses ./bench
#   원격 접근 비율이 높으면 PAR-09

# 5) 메모리 대역폭 포화 확인
perf stat -e uncore_imc/data_reads/,uncore_imc/data_writes/ ./bench
#   대역폭 한계에 닿았다면 경쟁 문제가 아니라 알고리즘·데이터양 문제 (우선순위 절차 1~2단계로 회귀)
```

### 증상 → 원인 매핑

| 증상 | 우선 확인 | 관련 규칙 |
|---|---|---|
| 스레드 늘리면 오히려 느려짐 | false sharing | MEM-03, GLOB-02 |
| 읽기만 하는데도 확장 안 됨 | 원자적 참조 카운팅 | CPP-01 |
| 읽기만 하는데도 확장 안 됨 (2) | rwlock 읽기 카운터 | PAR-10 |
| IPC가 낮고 백엔드 스톨 높음 | 메모리 대기 | MEM-01, MEM-04, PAR-09 |
| 프로파일에 `malloc` 상위 | 할당자 경쟁 | ALLOC-01, PAR-11 |
| CPU 사용률 100%인데 진척 없음 | 스핀 루프 | PAR-12 |
| 소켓 수 늘릴 때만 나빠짐 | NUMA 원격 접근 | PAR-09 |
| 스레드 1개일 때만 빠름 | 직렬 구간 지배 | 알고리즘 재검토 |
| 병렬일 때만 개선이 사라짐 | 병렬 전용 루프 누락 | PAR-14 |

---

## 부록 C: 외부 원전·도구 (선택 참고)

본문만으로 판단이 가능하도록 작성했으므로, 아래는 더 깊이 파고들 때만 본다.

| 자료 | 링크 | 비고 |
|---|---|---|
| **Optimizing software in C++** (Agner Fog) | `agner.org/optimize/optimizing_cpp.pdf` | 무료. 사실상 이 분야 표준 문서 |
| **Instruction tables** (Agner Fog) | `agner.org/optimize/instruction_tables.pdf` | 명령어 레이턴시·스루풋 |
| **Microarchitecture** (Agner Fog) | `agner.org/optimize/microarchitecture.pdf` | 파이프라인·분기예측기 내부 |
| **What Every Programmer Should Know About Memory** (Drepper) | `lwn.net/Articles/250967/` | 캐시 계층의 고전 |
| **Intel Optimization Reference Manual** | Intel 개발자 사이트 | 공식 문서 |
| **Modern Microprocessors: A 90-Minute Guide** | `lighterra.com/papers/modernmicroprocessors/` | CPU 동작 원리 압축 |
| *Performance Analysis and Tuning on Modern CPUs* (Bakhvalov) | 무료 PDF | perf/VTune 실무 |
| *Data-Oriented Design* (Richard Fabian) | 온라인 무료 | MEM-04의 배경 |
| 블로그 | `easyperf.net`, `godbolt.org`, `quick-bench.com` | 어셈블리·마이크로벤치 |

**도구:** `perf`(stat/record/annotate/c2c/lock), Intel VTune(Top-Down), Valgrind(cachegrind/callgrind),
`pahole`(구조체 레이아웃), `numastat`/`numactl`(NUMA), Compiler Explorer(어셈블리).

> 이 컨테이너 설치 현황: `~/tools`에 perf 4.18 + FlameGraph, heaptrack 1.2, bpftrace,
> VTune **2025.0** (2026 버전은 Cascade Lake 미지원). 사용법은 `dev/profiling-guide.md`.
