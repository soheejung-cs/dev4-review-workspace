---- MODULE TraceCheck ----
\* 코드 대조 틀(tla.md §3-T). 이 파일을 명세 디렉터리의 trace/ 로 복사해 Apply 와 VARIABLES 만 바꾼다.
\* TraceData(Trace·Threads)는 tools/tla/trace_to_tla.py 가 만든다.
EXTENDS Naturals, Sequences, FiniteSets, TraceData
VARIABLES c, done
M == INSTANCE Counter
N == Len(Trace)

\* 아직 소비하지 않은 행동 중, 이 행동보다 확실히 먼저 끝난 것(t1 < 내 t0)이 남아 있지 않으면 다음 차례가 될 수 있다.
\* 구간이 겹친 행동끼리는 어느 순서든 시도한다(시간 상자 — Specula Category B).
Ready(i) == /\ i \notin done
            /\ \A j \in (1..N) \ done : j = i \/ ~(Trace[j].t1 < Trace[i].t0)

\* 행동 이름 → 모델 행동 + 기록한 상태 대조(ValidatePostState). 기록한 필드는 모두 대조한다 — 대조하지 않을 필드는 계측에서 뺀다.
Apply(e) == CASE e.ev = "Inc" -> M!Inc /\ c' = e.st.c

Init == M!Init /\ done = {}
Next == \E i \in 1..N : Ready(i) /\ done' = done \cup {i} /\ Apply(Trace[i])

\* 이 불변식이 깨지면 = 모든 행동을 소비한 상태에 닿았다 = 모델이 실행을 설명한다(tlc_run --expect trace).
TraceIncomplete == done # 1..N
====
