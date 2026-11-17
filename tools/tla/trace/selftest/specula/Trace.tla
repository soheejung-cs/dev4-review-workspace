---- MODULE Trace ----
\* selftest 의 Counter.tla 를 base 로 한 Specula 꼴 Trace 명세(concurrent-timebox-guide.md 그대로 + WF). tla.md §3-T "Specula 로 확인".
EXTENDS Counter, Json, Naturals, Sequences, TLC
CONSTANT TraceFile
traceData == JsonDeserialize(TraceFile)
traces == traceData.traces
VARIABLE pc
vars == <<c>>
traceVars == <<pc>>
allVars == <<vars, traceVars>>
ThreadsWithEvents == { tid \in DOMAIN traces : pc[tid] <= Len(traces[tid]) }
ViablePIDs ==
    { tid \in ThreadsWithEvents :
        ~ \E tid2 \in ThreadsWithEvents :
            /\ tid2 /= tid
            /\ traces[tid2][pc[tid2]].end < traces[tid][pc[tid]].start }
TraceInit == /\ Init /\ pc = [tid \in DOMAIN traces |-> 1]
MatchEvent(tid, logline) ==
    CASE logline.event = "Inc" -> /\ Inc /\ c' = logline.state.c
TraceNext ==
    \/ /\ ThreadsWithEvents /= {}
       /\ \E tid \in ViablePIDs :
            LET logline == traces[tid][pc[tid]] IN
            /\ MatchEvent(tid, logline)
            /\ pc' = [pc EXCEPT ![tid] = pc[tid] + 1]
    \/ /\ ThreadsWithEvents = {}
       /\ UNCHANGED allVars
TraceSpec == TraceInit /\ [][TraceNext]_allVars /\ WF_allVars(TraceNext)
TraceFullyConsumed == <>(ThreadsWithEvents = {})
====
