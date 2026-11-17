---- MODULE TraceCheck ----
\* 코드 대조 틀(tla.md §3-T). 명세 디렉터리의 trace/ 로 복사해 VARIABLES·INSTANCE·MatchEvent 만 바꾼다.
\* 구조와 이름은 Specula 의 Trace.tla(skills/harness-generation references/concurrent-timebox-guide.md)와 같다 —
\*   pc(스레드별 커서) · ViablePIDs(다음 차례가 될 수 있는 스레드) · MatchEvent(행동 이름 → 모델 행동 + 기록 상태 대조).
\* 다른 점 둘:
\*   1. 데이터: Specula 는 병합 JSON 을 Json 모듈(CommunityModules)의 JsonDeserialize 로 읽는다. 여기는 tools/tla/trace_to_tla.py 가
\*      같은 구조를 TLA 리터럴(TraceData)로 만든다 — tla2tools.jar 하나로 돈다. 같은 trace 의 병합 JSON 은 --merged 로 함께 나온다.
\*   2. 완료 판정: Specula 는 PROPERTY TraceFullyConsumed(<> 모두 소비)를 쓴다 — 구간이 겹쳐 한 순서만 막혀도 그 행동열이
\*      성질을 어겨 '불일치' 로 나온다. 여기는 "모두 소비한 상태에 닿는 순서가 하나라도 있나" 를 불변식 TraceIncomplete 의
\*      위반으로 본다(tlc_run --expect trace 가 교착 검사를 끄고 판정).
EXTENDS Naturals, Sequences, FiniteSets, TraceData
VARIABLES c, pc
M == INSTANCE Counter

TraceOf(t) == Traces[t + 1]
Pending(t) == pc[t] <= Len(TraceOf(t))
Cur(t) == TraceOf(t)[pc[t]]

\* 남은 행동이 있고, 다른 스레드의 다음 행동 중 이 행동이 시작하기 전에 끝난 것이 없으면 다음 차례가 될 수 있다.
\* 구간이 겹친 스레드들은 모두 viable — TLC 가 모든 순서를 시도한다(시간 상자).
ViablePIDs == { t \in Threads : Pending(t) /\
                  \A u \in Threads \ {t} : Pending(u) => ~(Cur(u).end < Cur(t).start) }

\* 행동 이름 → 모델 행동 + 기록 상태 대조(ValidatePostState). 기록한 필드는 모두 대조한다 — 대조하지 않을 필드는 계측에서 뺀다.
MatchEvent(t, l) == CASE l.event = "Inc" -> M!Inc /\ c' = l.state.c

Init == M!Init /\ pc = [t \in Threads |-> 1]
Next == \E t \in ViablePIDs : MatchEvent(t, Cur(t)) /\ pc' = [pc EXCEPT ![t] = @ + 1]

\* 이 불변식이 깨지면 = 모든 스레드의 행동을 다 소비한 상태에 닿았다 = 모델이 실행을 설명한다(tlc_run --expect trace).
TraceIncomplete == \E t \in Threads : Pending(t)
====
