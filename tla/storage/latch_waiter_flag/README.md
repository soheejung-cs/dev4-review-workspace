# latch_waiter_flag — 래치 대기를 포기한 스레드가 남기는 `waiter_exists`

- 이슈·PR: CBRD-28163 · CUBRID/cubrid#8587 (2026-11-17 자기 리뷰, `tla_candidates.py` 가 `pgbuf_timed_sleep_error_handling` 을 [동시성] 후보로 냄)
- 대상 함수: `pgbuf_timed_sleep_error_handling ()`(src/storage/page_buffer.c — PR 이 바꾼 함수), 그와 같은 BCB 를 두고 엇갈리는 `pgbuf_latch_bcb_upon_fix ()`·`pgbuf_block_bcb ()`·`pgbuf_unlatch_bcb_upon_unfix ()`·`pgbuf_wakeup_reader_writer ()`·`pgbuf_timed_sleep ()`·무잠금 READ fix/unfix
- 실행: `python3 tools/tla/tlc_run.py tla/storage/latch_waiter_flag/LatchWaiterFlag.tla --config tla/storage/latch_waiter_flag/before.cfg --expect violation`
  · `… --config …/after.cfg --expect ok` (그 밖의 cfg 는 아래 결과 표)

## 무엇을 모델로 했나
페이지 하나의 BCB — 래치 워드(`latch_mode`·`fcnt`·`waiter_exists`)와 대기열 — 를 스레드 H 가 READ 로 쥔 채 시작한다(세션 A 의 스캔).
요청자 `Req` 는 각자 READ 또는 WRITE 를 요청하고(초기 상태가 모든 조합을 고른다), 바로 받거나 줄을 서며, `CanTimeout` 에 든 요청자는 기다리다 포기할 수 있다.
행동 하나가 코드의 임계 구역 하나(BCB mutex 구간, 또는 래치 워드의 CAS 하나)라서 TLC 가 fix·unfix·깨우기·포기의 모든 엇갈림을 끝까지 따라간다.
`FIX = "develop"` 은 포기 경로가 표시를 그대로 두고, `"pr"` 은 막힌 reader·writer 가 남지 않으면 지우고, `"always"` 는 확인 없이 지우는 변이다.

| 모델 | 코드 (PR 헤드 `cae32d077`, base develop `bb71348d6` — 줄이 다르면 base 를 괄호에) |
|---|---|
| `Fix` 의 첫 갈래(유휴 BCB 승인) | `pgbuf_latch_bcb_upon_fix ()` :7028-7036 `is_page_idle` |
| `Fix` 의 둘째 갈래(READ 위 READ, 표시 없음) | 같은 함수 :7044-7050 · 무잠금 READ fix :8421 (base :8411) 도 같은 조건 |
| `Fix` 의 셋째 갈래(표시를 세우고 줄 섬) | 같은 함수 :7054-7059(표시가 이미 있음)·:7108-7112(보유자가 아님) → `pgbuf_block_bcb ()` :7714 의 맨 뒤 붙이기 |
| `Unfix` | 무잠금 READ unfix `pgbuf_lockfree_unfix_ro ()` :8495 (base :8485)(READ·표시 없음·fcnt > 1) / `pgbuf_unlatch_bcb_upon_unfix ()` :7324 의 CAS → fcnt 0 이면 :7521 `pgbuf_wakeup_reader_writer ()` |
| `Wake` | `pgbuf_wakeup_reader_writer ()` :8138 (base :8128) — NO_LATCH 대기자 떼기, 맨 앞 승인, READ 이면 뒤의 READ 도(WRITE 는 건너뜀), WRITE 승인 뒤 멈춤, 끝에서 막힌 요청이 없으면 표시를 지움(:8265-8268) |
| `Woken` | `pgbuf_timed_sleep ()` :7997-8002 (깨움) · :8019 (타임아웃 갈래의 확인) — `THREAD_PGBUF_RESUMED` |
| `GiveUpStart` | `pgbuf_timed_sleep ()` 인터럽트 :8005 · 타임아웃 :8035 (`request_latch_mode = PGBUF_NO_LATCH`) |
| `GiveUp` + `Succ` | `pgbuf_timed_sleep_error_handling ()` :7859 — case 1 :7868, case 2 :7875, case 3 :7892-7936(승계 루프), 끝 :7938-7946 (base: case 1·2 가 :7872·:7889 에서 바로 return, 끝 :7938) |
| `Blocked` | `pgbuf_is_exist_blocked_reader_writer ()` :11881 (base :11871) |

## 성질
- `NoNeedlessWait` — 사용자에게 보이는 것(CBRD-28163): READ 로만 잡힌 페이지에 대한 READ 요청은 앞에 줄 선 WRITE 요청이 있을 때만 기다린다. 포기가 진행 중인 상태(`Quiet` 아님)는 뺀다.
- `FlagSound` — PR 이 세우는 관계(규칙 #028 꼴)의 한쪽: 표시가 서 있으면 막힌 reader·writer 가 줄에 있다.
- `FlagComplete` — 다른 쪽: 막힌 reader·writer 가 있으면 표시가 서 있다(READ 가 줄 선 writer 를 앞지르지 않고, 마지막 unfix 가 깨우기를 건너뛰지 않는다).
- `IdleClean` — 유휴 BCB 에 표시가 없다(`pgbuf_latch_bcb_upon_fix ()` :6993 의 안전망이 발화하지 않는다).
- 교착 없음 — TLC 의 deadlock 검사. `after_wait.cfg` 는 r2·r3 가 포기하지 않으므로 깨우기를 잃으면 deadlock 으로 잡힌다.

## 결과
| 날짜 | 대상 | cfg | 판정 | 상태(생성/고유) | 깊이 | 시간 | 도구 |
|---|---|---|---|---|---|---|---|
| 2026-11-17 (.51) | develop(FIX="develop"), 요청자 2·모두 포기 가능, 모든 READ/WRITE 조합 | before.cfg | **NoNeedlessWait 위반**(기대대로) — 반례 5걸음 | 150 / 112 | 5 | 1.4s | TLC2 2.19 (tla2tools v1.7.4, sha256 `936a2620…`) |
| 2026-11-17 (.51) | develop, 같은 범위 | before_flag.cfg | **FlagSound 위반**(기대대로) — 반례 4걸음 | 74 / 59 | 4 | 1.3s | 같음 |
| 2026-11-17 (.51) | PR 헤드 `cae32d077`(FIX="pr"), before.cfg 와 같은 범위 | after.cfg | **전부 성립** — TypeOK·FlagSound·FlagComplete·NoNeedlessWait·IdleClean, 교착 없음 | 394 / 214 | 8 | 1.3s | 같음 |
| 2026-11-17 (.51) | PR, 요청자 3·모두 포기 가능([W,R,W]·[W,W,R] 같은 줄) | after_3.cfg | 전부 성립 | 8,674 / 3,459 | 11 | 1.6s | 같음 |
| 2026-11-17 (.51) | PR, 요청자 3·r1 만 포기(r2·r3 는 끝까지 기다림) | after_wait.cfg | 전부 성립, 교착 없음 | 3,234 / 1,689 | 11 | 1.7s | 같음 |
| 2026-11-17 (.51) | 변이: 확인 없이 지움(FIX="always"), 요청자 3 | mut_always.cfg | **FlagComplete 위반**(기대대로) | 620 / 445 | 5 | 1.3s | 같음 |

before 반례(TC `cbrd_28163` 과 같은 꼴): H 가 READ 로 쥔 페이지에 r1 이 WRITE 를 요청해 표시를 세우고 줄 선다(`Fix` 셋째 갈래) → r1 이 타임아웃으로 포기(`GiveUpStart` → `GiveUp` case 3: 줄이 빈다, 표시는 그대로) → r2 의 READ 요청이 표시를 보고 줄 선다 — 앞에 아무도 없는데 H 가 놓을 때까지 기다린다.
코드로는 page_buffer.c(base) :8035 → :7892-7938(표시를 건드리지 않고 return) → 다음 READ 요청의 :7054-7059(표시가 있고 보유자가 아님 → 줄 섬).
변이 반례: [r1 W, r2 W] 에서 r1 이 포기하면 r2 가 막힌 채 표시가 지워진다 — 뒤이은 READ 가 r2 를 앞지른다.

## 모델이 보지 않는 것
- **포기 쪽의 엔트리 락 없는 확인(CBRD-27739 원인 A)**: `pgbuf_timed_sleep ()` 의 타임아웃 갈래는 `thread_suspend_timeout_wakeup_and_unlock_entry ()` 가 엔트리 락을 풀고 돌아온 뒤 락 없이 `resume_status` 를 보고 `request_latch_mode` 를 쓴다. 모델은 확인과 쓰기를 한 걸음(`GiveUpStart`)으로 두었다 — 깨우는 쪽과의 그 엇갈림은 CBRD-27739 의 몫이다.
- **승계 루프가 엔트리 락 전에 fcnt 를 더하는 순서(CBRD-27739 원인 B)**: `Succ` 는 READ 대기자의 확인과 fcnt 더하기를 한 걸음으로 두었다. 단위 suite `unit/storage/page_buffer.timed_sleep_error_handling` 의 R1 이 그 순서를 실제 함수로 재현한다(develop 동일).
- **조건부 래치 요청**(`PGBUF_CONDITIONAL_LATCH`): 거절될 때도 CAS 로 표시를 세우고 줄 서지 않고 돌아간다(:7092-7095·:7108-7112, 거절 :7227-7259) — 같은 '대기자 없는 표시' 의 다른 원천이고 PR 범위 밖(PR 본문 Remarks 의 확인 부탁).
- 보유자의 재고정·READ→WRITE 승격(`wait_for_latch_promote`, 맨 앞 끼우기), flush 대기자(`PGBUF_LATCH_FLUSH` — 표시를 세우지 않고 `pgbuf_wake_flush_waiters ()` 가 깨움; 단위 suite 의 e2·f·f2 가 포기 경로와의 조합을 확인), 페이지 폐기 경로가 flush 대기 전에 표시를 세우는 :3648, 한 스레드의 fix 여러 개(`request_fix_count` > 1 — 단위 suite c·h).
- 래치 대기 무한(`LK_INFINITE_WAIT`) 트랜잭션의 debug `assert (0)` 과 철회 중 재시도(`goto try_again`) — 포기 여부만 `CanTimeout` 으로 고른다.
