---- MODULE Trace ----
\* Specula-form Trace spec (concurrent-timebox-guide.md) for LatchWaiterFlag -- read by the MCP tracedebugger
\* (run_trace_validation) from trace_to_tla.py --merged JSON.  MatchEvent is TraceCheck.tla's; the model's own pc is
\* renamed mpc because Specula's trace cursor is pc.  FIX is fixed per config (TraceFix).
EXTENDS Json, IOUtils, Naturals, Sequences, TLC
CONSTANTS TraceFile, TraceFix
\* the MCP (run_trace_validation[_parallel]) passes the trace in the environment variable JSON (Specula's Trace.tla
\* idiom); TraceFile is only the fallback for a hand run.  Reading the constant alone made the MCP's trace_file
\* argument a no-op -- every trace of a parallel run would have been judged as the same traces/merged.json.
JsonFile == IF "JSON" \in DOMAIN IOEnv THEN IOEnv.JSON ELSE TraceFile
traceData == JsonDeserialize(JsonFile)
traces == traceData.traces
VARIABLES mode, fcnt, wex, q, kind, req, resumed, mpc, pc
vars == <<mode, fcnt, wex, q, kind, req, resumed, mpc>>
traceVars == <<pc>>
allVars == <<vars, traceVars>>
M == INSTANCE LatchWaiterFlag WITH H <- 0, Req <- {1, 2}, CanTimeout <- {1, 2}, FIX <- TraceFix, pc <- mpc

ThreadsWithEvents == { tid \in DOMAIN traces : pc[tid] <= Len(traces[tid]) }
ViablePIDs ==
    { tid \in ThreadsWithEvents :
        ~ \E tid2 \in ThreadsWithEvents :
            /\ tid2 /= tid
            /\ traces[tid2][pc[tid2]].end < traces[tid][pc[tid]].start }
\* the latch mode each requester asked for, read from its Fix record (args.kind) -- Init's choice of kind is fixed to it;
\* otherwise the branches with the other kinds deadlock and Specula's TraceSpec reports them
FixKind(r) == IF \E tid \in DOMAIN traces : \E i \in 1 .. Len(traces[tid]) : traces[tid][i].event = "Fix" /\ traces[tid][i].args.who = r
              THEN CHOOSE k \in {"R", "W"} : \E tid \in DOMAIN traces : \E i \in 1 .. Len(traces[tid]) :
                     traces[tid][i].event = "Fix" /\ traces[tid][i].args.who = r /\ traces[tid][i].args.kind = k
              ELSE "R"
TraceInit == /\ M!Init /\ kind = [r \in {1, 2} |-> FixKind(r)] /\ pc = [tid \in DOMAIN traces |-> 1]

Post(l) == /\ mode' = l.state.mode /\ fcnt' = l.state.fcnt /\ wex' = l.state.wex /\ q' = l.state.q
MatchEvent(tid, l) ==
    LET w == l.args.who IN
    CASE l.event = "Fix" /\ w = 0  -> /\ UNCHANGED vars
                                      /\ l.state.mode = mode /\ l.state.fcnt = fcnt /\ l.state.wex = wex /\ l.state.q = q
      [] l.event = "Fix"           -> M!Fix(w) /\ kind[w] = l.args.kind /\ Post(l)
      [] l.event = "Unfix"         -> M!Unfix(w) /\ Post(l)
      [] l.event = "Woken"         -> M!Woken(w)
      [] l.event = "GiveUpStart"   -> M!GiveUpStart(w)
      [] l.event = "GiveUp"        -> M!GiveUp(w) /\ Post(l)

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
