/*
 * tla_trace.h — TLA+ trace validation 용 계측 헤더 (C·C++ 공용, x86_64 Linux).
 *
 * skills/harness-review/tla.md §3-T 의 도구다. 실제 코드가 어떤 순서로 무엇을 했는지 스레드별 NDJSON 으로 남기고,
 * tools/tla/trace_to_tla.py 가 그것을 TraceData.tla(우리 TraceCheck)와 Specula 병합 JSON(--merged) 으로 바꾼다.
 * 형식은 Specula(arXiv 2607.25333, github.com/specula-org/Specula) skills/harness-generation 의 Category B(시간 상자)와 같다
 * (2026-11-17 맞춤 — references/concurrent-timebox-guide.md):
 *   - 한 줄 = 한 행동: {"tag":"trace","event":"<행동>","tid":<0부터>,"start":<tsc>,"end":<tsc>,"state":{...}}
 *     선택: "args":{...} — 행동의 인자(Specula 의 args 필드).
 *   - 파일: $TLA_TRACE_DIR(없으면 cwd)/$TLA_TRACE_PREFIX(없으면 trace)-thread-<tid>.ndjson — tid 는 프로세스 안에서
 *     처음 기록한 순서로 0,1,2… (Specula 전처리기가 파일 이름에서 tid 를 읽는다).
 *   - 스레드마다 자기 파일에 쓴다 — 기록에 뮤텍스를 두면 관찰하려는 엇갈림 자체가 바뀐다(probe effect).
 *   - 행동 하나를 [start, end] 구간(rdtscp)으로 감싼다. 구간이 겹친 행동끼리는 TLC 가 모든 순서를 시도한다.
 *   - 상태는 end 를 찍은 뒤에 쓴다(구간을 좁게).
 *
 * 켜기: 계측을 넣은 번역 단위를 -DTLA_TRACE 로 컴파일한다. 없으면 매크로가 전부 빈 문장이다 — 엔진 브랜치에 커밋하지 않는다
 *   (계측은 git diff 로 만든 패치로만 둔다: tla/<모듈>/<이름>/trace/instrument.patch).
 *   event 는 TraceCheck.tla 의 MatchEvent 갈래 이름과 1:1 로 같아야 한다.
 * 쓰기:
 *   TLA_TRACE_BEGIN (t);                                   // 행동 시작 직전
 *   ... 실제 코드(잠금 획득·원자 연산·갱신) ...
 *   TLA_TRACE_END (t, "Inc");                              // 상태 없음
 *   TLA_TRACE_END_ST (t, "Inc", "\"c\":%d", c);            // state 객체의 안쪽 JSON 을 printf 형식으로
 *   TLA_TRACE_END_ARGS (t, "Put", "\"k\":%d", k, "\"n\":%d", n);  // args 와 state 를 함께 — 형식 문자열 둘, 각 값 하나씩
 * 주의:
 *   - 버퍼는 프로세스가 정상 종료할 때 비워진다. abort·kill 로 끝나는 실행이면 그 전에 TLA_TRACE_FLUSH () 를 부른다.
 *   - rdtscp 가 코어 사이에서 같은 시계여야 한다: grep -wc constant_tsc /proc/cpuinfo · grep -wc nonstop_tsc /proc/cpuinfo 가 0 이 아니어야 한다.
 *   - 손으로 쓰거나 지어낸 trace 는 쓰지 않는다. 시험은 실제 코드(단위 suite·재현 SQL)를 돌려 만든다.
 *   - 한 디렉터리에 한 프로세스의 trace 만 둔다 — 파일 이름에 pid 가 없다(Specula 이름 규약).
 */
#ifndef TLA_TRACE_H
#define TLA_TRACE_H

#ifdef TLA_TRACE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <x86intrin.h>

static __thread FILE *tla_trace_fp_;
static __thread int tla_trace_tid_ = -1;
static int tla_trace_next_tid_;  /* 프로세스 안 스레드 번호 — 처음 기록한 순서 */

static inline FILE *
tla_trace_file_ (void)
{
  if (tla_trace_fp_ == NULL)
    {
      const char *dir = getenv ("TLA_TRACE_DIR");
      const char *prefix = getenv ("TLA_TRACE_PREFIX");
      char path[1024];
      tla_trace_tid_ = __atomic_fetch_add (&tla_trace_next_tid_, 1, __ATOMIC_RELAXED);
      snprintf (path, sizeof (path), "%s/%s-thread-%d.ndjson", dir ? dir : ".", prefix ? prefix : "trace",
		tla_trace_tid_);
      tla_trace_fp_ = fopen (path, "w");
      if (tla_trace_fp_ == NULL)
	{
	  perror ("tla_trace: fopen");
	  abort ();
	}
      setvbuf (tla_trace_fp_, NULL, _IOFBF, 1 << 16);
    }
  return tla_trace_fp_;
}

static inline uint64_t
tla_trace_now_ (void)
{
  unsigned int aux;
  return __rdtscp (&aux);
}

#define TLA_TRACE_BEGIN(var) uint64_t var = tla_trace_now_ ()
#define TLA_TRACE_HEAD_(var, ev) \
  uint64_t tla_t1_ = tla_trace_now_ (); \
  FILE *tla_f_ = tla_trace_file_ (); \
  fprintf (tla_f_, "{\"tag\":\"trace\",\"event\":\"%s\",\"tid\":%d,\"start\":%llu,\"end\":%llu", (ev), \
	   tla_trace_tid_, (unsigned long long) (var), (unsigned long long) tla_t1_)
#define TLA_TRACE_END(var, ev) \
  do { TLA_TRACE_HEAD_ (var, ev); fputs (",\"state\":{}}\n", tla_f_); } while (0)
#define TLA_TRACE_END_ST(var, ev, ...) \
  do { TLA_TRACE_HEAD_ (var, ev); fputs (",\"state\":{", tla_f_); fprintf (tla_f_, __VA_ARGS__); \
       fputs ("}}\n", tla_f_); } while (0)
#define TLA_TRACE_END_ARGS(var, ev, args_fmt, args_val, st_fmt, st_val) \
  do { TLA_TRACE_HEAD_ (var, ev); fputs (",\"args\":{", tla_f_); fprintf (tla_f_, args_fmt, args_val); \
       fputs ("},\"state\":{", tla_f_); fprintf (tla_f_, st_fmt, st_val); fputs ("}}\n", tla_f_); } while (0)
#define TLA_TRACE_FLUSH() do { if (tla_trace_fp_) fflush (tla_trace_fp_); } while (0)

#else /* !TLA_TRACE */
#define TLA_TRACE_BEGIN(var) do { } while (0)
#define TLA_TRACE_END(var, ev) do { } while (0)
#define TLA_TRACE_END_ST(var, ev, ...) do { } while (0)
#define TLA_TRACE_END_ARGS(var, ev, args_fmt, args_val, st_fmt, st_val) do { } while (0)
#define TLA_TRACE_FLUSH() do { } while (0)
#endif /* TLA_TRACE */

#endif /* TLA_TRACE_H */
