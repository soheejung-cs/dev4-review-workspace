---- MODULE TraceCheck ----
\* Code-conformance check for LatchWaiterFlag (skills/harness-review/tla.md §3-T), copied from
\* tools/tla/trace/selftest/TraceCheck.tla: only VARIABLES, INSTANCE and MatchEvent differ.
\* Threads of the trace are tla_trace.h's tids; the model thread of each record is args.who (thread entry index:
\* 0 = H, 1 = r1, 2 = r2 -- test_trace.cpp).  H's first Fix (the idle grant) is the model's Init: it must leave
\* the BCB as Init has it and changes no model variable.
EXTENDS Naturals, Sequences, FiniteSets, TraceData
CONSTANT FIXV
VARIABLES mode, fcnt, wex, q, kind, req, resumed, mpc, pc
mvars == <<mode, fcnt, wex, q, kind, req, resumed, mpc>>
M == INSTANCE LatchWaiterFlag WITH H <- 0, Req <- {1, 2}, CanTimeout <- {1, 2}, FIX <- FIXV, pc <- mpc

TraceOf(t) == Traces[t + 1]
Pending(t) == pc[t] <= Len(TraceOf(t))
Cur(t) == TraceOf(t)[pc[t]]

ViablePIDs == { t \in Threads : Pending(t) /\
                  \A u \in Threads \ {t} : Pending(u) => ~(Cur(u).end < Cur(t).start) }

\* every recorded field is compared (ValidatePostState)
Post(l) == /\ mode' = l.state.mode /\ fcnt' = l.state.fcnt /\ wex' = l.state.wex /\ q' = l.state.q

MatchEvent(t, l) ==
    LET w == l.args.who IN
    CASE l.event = "Fix" /\ w = 0  -> /\ UNCHANGED mvars
                                      /\ l.state.mode = mode /\ l.state.fcnt = fcnt /\ l.state.wex = wex /\ l.state.q = q
      [] l.event = "Fix"           -> M!Fix(w) /\ kind[w] = l.args.kind /\ Post(l)
      [] l.event = "Unfix"         -> M!Unfix(w) /\ Post(l)
      [] l.event = "Woken"         -> M!Woken(w)
      [] l.event = "GiveUpStart"   -> M!GiveUpStart(w)
      [] l.event = "GiveUp"        -> M!GiveUp(w) /\ Post(l)

\* the latch mode each requester asked for, read from its Fix record (args.kind) -- Init's choice of kind is fixed to it
FixKind(r) == IF \E t \in Threads : \E i \in 1 .. Len(TraceOf(t)) : TraceOf(t)[i].event = "Fix" /\ TraceOf(t)[i].args.who = r
              THEN CHOOSE k \in {"R", "W"} : \E t \in Threads : \E i \in 1 .. Len(TraceOf(t)) :
                     TraceOf(t)[i].event = "Fix" /\ TraceOf(t)[i].args.who = r /\ TraceOf(t)[i].args.kind = k
              ELSE "R"
Init == M!Init /\ kind = [r \in {1, 2} |-> FixKind(r)] /\ pc = [t \in Threads |-> 1]
Next == \E t \in ViablePIDs : MatchEvent(t, Cur(t)) /\ pc' = [pc EXCEPT ![t] = @ + 1]

TraceIncomplete == \E t \in Threads : Pending(t)
====
