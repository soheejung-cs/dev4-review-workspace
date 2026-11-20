---- MODULE TraceCheck ----
\* Code-conformance check for BitmapHeapSteps (skills/harness-review/tla.md §3-T), after
\* tools/tla/trace/selftest/TraceCheck.tla: VARIABLES, INSTANCE, MatchEvent and the silent steps differ.
\* The trace comes from test_trace.cpp run on the real barrier (px_scan_input_handler_index.cpp) and the real
\* scan_bitmap_marks (scan_manager.c) with instrument.patch.  The model worker of a record is args.w (the
\* scan_bitmap_worker the thread claimed, 1-based).  The steps of px_scan_task.cpp that the test drives but the
\* patch cannot record (the loop's S_END and error paths, reading a loaded partition, the merge of the lists, the
\* main thread) are silent model steps: TLC may take them anywhere, the recorded ones must match in order.
EXTENDS Naturals, Sequences, FiniteSets, TraceData
CONSTANT MUTV   \* "pr"; a BitmapHeapSteps mutant for the sensitivity check
VARIABLES mpc, found, pend, pub, overOpen, pending, intr, nextPart, myPart, heapTbl, overTaken, casSeen, base,
          emitted, started, failed, mainRes, freed, pc
mvars == <<mpc, found, pend, pub, overOpen, pending, intr, nextPart, myPart, heapTbl, overTaken, casSeen, base,
           emitted, started, failed, mainRes, freed>>

TraceOf(t) == Traces[t + 1]
WSet == {TraceOf(t)[1].args.w : t \in {u \in Threads : Len(TraceOf(u)) > 0}}
TOids == {1001, 2003, 65001, 70002}      \* test_trace.cpp: pageid * 1000 + slot
TPartOf == [o \in TOids |-> (o \div 1000) \div 64]   \* sector 0 -> partition 0, sector 1 -> partition 1

M == INSTANCE BitmapHeapSteps WITH W <- WSet, Oids <- TOids, NParts <- 2, PartOf <- TPartOf, Over <- {},
                                   CanFail <- WSet, StartChoices <- {WSet}, MUT <- MUTV, pc <- mpc

Pending(t) == pc[t] <= Len(TraceOf(t))
Cur(t) == TraceOf(t)[pc[t]]

ViablePIDs == { t \in Threads : Pending(t) /\
                  \A u \in Threads \ {t} : Pending(u) => ~(Cur(u).end < Cur(t).start) }

SeqSet(s) == {s[i] : i \in 1 .. Len(s)}

\* every recorded field is compared
MatchEvent(t, l) ==
    LET w == l.args.w IN
    CASE l.event = "Mark"     -> M!Mark(w) /\ pend'[w] = pend[w] \cup {l.args.oid}
      [] l.event = "EndMarks" -> M!EndMarks(w)
      [] l.event = "Leave"    -> M!Leave(w) /\ pending' = l.state.pending
      [] l.event = "WaitPass" -> M!WaitPass(w)
      [] l.event = "WaitStop" -> M!WaitStop(w)
      [] l.event = "TakePart" -> M!TakePart(w) /\ myPart'[w] = l.state.part
      [] l.event = "Load"     -> M!Load(w) /\ myPart[w] = l.args.part /\ heapTbl'[w] = SeqSet(l.state.heap)
      [] l.event = "OverTry"  -> M!OverTry(w) /\ (mpc'[w] = "merge") = l.state.won
      [] l.event = "Close"    -> M!Close(w)

\* the OIDs each worker's leaves gave = its Mark records (Init's choice of found is fixed to them)
FoundOf(w) == {o \in TOids : \E t \in Threads : \E i \in 1 .. Len(TraceOf(t)) :
                  TraceOf(t)[i].event = "Mark" /\ TraceOf(t)[i].args.w = w /\ TraceOf(t)[i].args.oid = o}

Init == M!Init /\ found = [w \in WSet |-> FoundOf(w)] /\ pc = [t \in Threads |-> 1]

Silent == /\ \/ \E w \in WSet : \/ M!IdxEnd(w) \/ M!IdxIntr(w) \/ M!IdxFail(w) \/ M!ReadPart(w)
                                \/ M!MergeStart(w) \/ M!MergeEnd(w) \/ M!OverRead(w)
             \/ M!Main
          /\ UNCHANGED pc

Next == \/ \E t \in ViablePIDs : MatchEvent(t, Cur(t)) /\ pc' = [pc EXCEPT ![t] = @ + 1]
        \/ Silent

TraceIncomplete == \E t \in Threads : Pending(t)
====
