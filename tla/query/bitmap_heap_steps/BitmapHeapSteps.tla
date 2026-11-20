-------------------------- MODULE BitmapHeapSteps --------------------------
(***************************************************************************)
(* CUBRID/cubrid#8627 "Bitmap heap scan": the parallel index scan's        *)
(* workers record the OIDs their leaves give (index step), meet at a       *)
(* barrier, then take heap partitions one by one with a fetch_add and read *)
(* the recorded pages of each (heap step); the OIDs whose slot is beyond   *)
(* an entry's width go to per-worker lists that one worker, the winner of  *)
(* a compare_exchange, merges and reads.                                   *)
(*                                                                         *)
(* One action = one step of the code: a mark, end_marks, the barrier's     *)
(* decrement (under m_bm_mutex), one check of its wait loop, one fetch_add,*)
(* one load of a partition, the reading of a loaded partition, the CAS,    *)
(* the merge.  Any worker in CanFail may fail at any of its steps; a       *)
(* failure raises the scan's interrupt as every error path of the task     *)
(* does.  Workers outside `started` were never pushed (start_tasks () ran  *)
(* out of memory half way) -- the main thread then ends the job.           *)
(*                                                                         *)
(* The feature is new: there is no develop counterpart (before = n/a).     *)
(* MUT picks the PR ("pr") or a mutant that breaks one mechanism, to show  *)
(* the properties can fail:                                                *)
(*   "leave_first"  the barrier is left before end_marks publishes         *)
(*   "wait_no_intr" the barrier's wait loop ignores the interrupt          *)
(*   "part_split"   take_partition as a load and a store                   *)
(*   "cas_split"    take_over's compare_exchange as a load and a store     *)
(*   "main_silent"  the main thread does not raise JOB_ENDED when          *)
(*                  start_tasks () failed                                  *)
(* README.md maps the actions to file:line.                                *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    W,            \* the workers (tasks) of the scan = m_parallelism
    Oids,         \* the OIDs the index can give
    NParts,       \* heap partitions (m_bm_nparts)
    PartOf,       \* [Oids -> 0..NParts-1]: the partition of the OID's page
    Over,         \* the OIDs whose slot is beyond SCAN_BITMAP_WORDS * 64
    CanFail,      \* the workers that may hit an error
    StartChoices, \* the sets of workers start_tasks () may have pushed
    MUT

ASSUME /\ CanFail \subseteq W /\ Over \subseteq Oids
       /\ \A s \in StartChoices : s \subseteq W
       /\ MUT \in {"pr", "leave_first", "wait_no_intr", "part_split", "cas_split", "main_silent"}

NoW == 0
PartOfDef == [o \in Oids |-> o % NParts]   \* the partition of an OID's page (cfg: PartOf <- PartOfDef)
NParts1 == NParts   \* "none left" value of a taken partition

VARIABLES
    pc,         \* [W -> phase]
    found,      \* [W -> SUBSET Oids]: what the worker's leaves give (Init picks every split)
    pend,       \* [W -> SUBSET Oids]: recorded, not yet visible to other workers (tbl without
                \*   tbl_start/tbl_order, a batch not written, an open list)
    pub,        \* [W -> SUBSET Oids]: published by end_marks
    overOpen,   \* [W -> BOOLEAN]: the worker's over list still has its last page fixed
    pending,    \* m_bm_index_pending
    intr,       \* the scan's interrupt: "none", "err" (a worker), "ended" (main: JOB_ENDED)
    nextPart,   \* m_bm_next_part
    myPart,     \* [W -> 0..NParts]: the partition taken / read by part_split
    heapTbl,    \* [W -> SUBSET Oids]: heap_tbl after load
    overTaken,  \* m_bm_over_taken
    casSeen,    \* [W -> BOOLEAN]: cas_split's load
    base,       \* [W -> W \cup {NoW}]: the worker whose list the taker merges into
    emitted,    \* [Oids -> Nat]: rows the heap step handed on (drain_slot_oids)
    started,    \* the workers that run
    failed,     \* history: a worker hit an error
    mainRes,    \* "wait", "ok", "err": what the main thread returns
    freed       \* the input handler (and its scan_bitmap_marks) is destroyed

vars == <<pc, found, pend, pub, overOpen, pending, intr, nextPart, myPart, heapTbl,
          overTaken, casSeen, base, emitted, started, failed, mainRes, freed>>

Phases == {"idx", "endm", "leave", "leave_stop", "wait", "heap", "take2", "load", "read",
           "over", "over2", "merge", "merging", "overread", "close", "done", "unstarted"}
IndexPhases == {"idx", "endm", "leave", "leave_stop"}
HeapPhases == {"heap", "take2", "load", "read", "over", "over2", "merge", "merging", "overread"}

AllFound == UNION {found[w] : w \in W}
AllPub == UNION {pub[w] : w \in W}

TypeOK ==
    /\ pc \in [W -> Phases]
    /\ pend \in [W -> SUBSET Oids] /\ pub \in [W -> SUBSET Oids]
    /\ overOpen \in [W -> BOOLEAN]
    /\ pending \in 0..Cardinality(W)
    /\ intr \in {"none", "err", "ended"}
    /\ nextPart \in 0..(NParts + Cardinality(W))
    /\ emitted \in [Oids -> 0..Cardinality(W)]
    /\ mainRes \in {"wait", "ok", "err"}

Init ==
    /\ found \in [W -> SUBSET Oids]
    /\ started \in StartChoices
    /\ pc = [w \in W |-> IF w \in started THEN "idx" ELSE "unstarted"]
    /\ pend = [w \in W |-> {}] /\ pub = [w \in W |-> {}]
    /\ overOpen = [w \in W |-> FALSE]
    /\ pending = Cardinality(W)           \* bitmap_init_on_main: m_bm_index_pending = parallelism
    /\ intr = "none"
    /\ nextPart = 0
    /\ myPart = [w \in W |-> 0]
    /\ heapTbl = [w \in W |-> {}]
    /\ overTaken = FALSE
    /\ casSeen = [w \in W |-> FALSE]
    /\ base = [w \in W |-> NoW]
    /\ emitted = [o \in Oids |-> 0]
    /\ failed = FALSE
    /\ mainRes = "wait"
    /\ freed = FALSE

Go(w, p) == pc' = [pc EXCEPT ![w] = p]
\* every error path of the task: move the message, set the interrupt if none yet, stop
Raise == /\ intr' = IF intr = "none" THEN "err" ELSE intr
         /\ failed' = TRUE

(* ---------------- index step (loop () until its S_END) ---------------- *)
\* leaf_slot_walker::process_oid -> scan_bitmap_marks::mark
Mark(w) ==
    /\ pc[w] = "idx"
    /\ \E o \in found[w] \ pend[w] :
         /\ pend' = [pend EXCEPT ![w] = @ \cup {o}]
         /\ overOpen' = IF o \in Over THEN [overOpen EXCEPT ![w] = TRUE] ELSE overOpen
    /\ UNCHANGED <<pc, found, pub, pending, intr, nextPart, myPart, heapTbl, overTaken, casSeen,
                   base, emitted, started, failed, mainRes, freed>>

\* loop (): get_next_page_with_fix () S_END, drain_late_joiner_chains () -> bitmap_heap_step ()
IdxEnd(w) ==
    /\ pc[w] = "idx" /\ found[w] \subseteq pend[w]
    /\ Go(w, IF MUT = "leave_first" THEN "leave" ELSE "endm")
    /\ UNCHANGED <<found, pend, pub, overOpen, pending, intr, nextPart, myPart, heapTbl, overTaken,
                   casSeen, base, emitted, started, failed, mainRes, freed>>

\* loop (): the interrupt check at the top of each page -> break (no barrier leave)
IdxIntr(w) ==
    /\ pc[w] = "idx" /\ intr # "none"
    /\ Go(w, "close")
    /\ UNCHANGED <<found, pend, pub, overOpen, pending, intr, nextPart, myPart, heapTbl, overTaken,
                   casSeen, base, emitted, started, failed, mainRes, freed>>

\* loop (): S_ERROR of a page / set_page / drain_slot_oids / drain_late_joiner_chains -> break
IdxFail(w) ==
    /\ pc[w] = "idx" /\ w \in CanFail
    /\ Raise /\ Go(w, "close")
    /\ UNCHANGED <<found, pend, pub, overOpen, pending, nextPart, myPart, heapTbl, overTaken,
                   casSeen, base, emitted, started, mainRes, freed>>

\* bitmap_heap_step (): end_marks () -- the list closed, tbl_start/tbl_order, the last batch written
EndMarks(w) ==
    /\ pc[w] = "endm"
    /\ pub' = [pub EXCEPT ![w] = @ \cup pend[w]]
    /\ pend' = [pend EXCEPT ![w] = {}]
    /\ overOpen' = [overOpen EXCEPT ![w] = FALSE]
    /\ Go(w, IF MUT = "leave_first" THEN "wait" ELSE "leave")
    /\ UNCHANGED <<found, pending, intr, nextPart, myPart, heapTbl, overTaken, casSeen, base,
                   emitted, started, failed, mainRes, freed>>

\* end_marks () fails -> raise () ; stop, the barrier is still left
EndFail(w) ==
    /\ pc[w] = "endm" /\ w \in CanFail
    /\ Raise
    /\ Go(w, IF MUT = "leave_first" THEN "close" ELSE "leave_stop")
    /\ UNCHANGED <<found, pend, pub, overOpen, pending, nextPart, myPart, heapTbl, overTaken,
                   casSeen, base, emitted, started, mainRes, freed>>

\* bitmap_leave_index_step (): --m_bm_index_pending under m_bm_mutex
Leave(w) ==
    /\ pc[w] \in {"leave", "leave_stop"}
    /\ pending' = pending - 1
    /\ Go(w, IF pc[w] = "leave_stop" THEN "close"
             ELSE IF MUT = "leave_first" THEN "endm" ELSE "wait")
    /\ UNCHANGED <<found, pend, pub, overOpen, intr, nextPart, myPart, heapTbl, overTaken, casSeen,
                   base, emitted, started, failed, mainRes, freed>>

\* bitmap_wait_index_step (): the loop's condition with every worker gone -> true
WaitPass(w) ==
    /\ pc[w] = "wait" /\ pending = 0 /\ intr = "none"
    /\ Go(w, "heap")
    /\ UNCHANGED <<found, pend, pub, overOpen, pending, intr, nextPart, myPart, heapTbl, overTaken,
                   casSeen, base, emitted, started, failed, mainRes, freed>>

\* ... the interrupt is up -> false -> return (the PR); the mutant only looks at it once nobody is left
WaitStop(w) ==
    /\ pc[w] = "wait" /\ intr # "none"
    /\ MUT # "wait_no_intr" \/ pending = 0
    /\ Go(w, "close")
    /\ UNCHANGED <<found, pend, pub, overOpen, pending, intr, nextPart, myPart, heapTbl, overTaken,
                   casSeen, base, emitted, started, failed, mainRes, freed>>

(* ---------------- heap step ---------------- *)
\* take_partition (): fetch_add on m_bm_next_part
TakePart(w) ==
    /\ pc[w] = "heap" /\ MUT # "part_split"
    /\ nextPart' = nextPart + 1
    /\ myPart' = [myPart EXCEPT ![w] = IF nextPart < NParts THEN nextPart ELSE NParts1]
    /\ Go(w, IF nextPart < NParts THEN "load" ELSE "over")
    /\ UNCHANGED <<found, pend, pub, overOpen, pending, intr, heapTbl, overTaken, casSeen, base,
                   emitted, started, failed, mainRes, freed>>

TakeRead(w) ==   \* part_split only
    /\ pc[w] = "heap" /\ MUT = "part_split"
    /\ myPart' = [myPart EXCEPT ![w] = IF nextPart < NParts THEN nextPart ELSE NParts1]
    /\ Go(w, "take2")
    /\ UNCHANGED <<found, pend, pub, overOpen, pending, intr, nextPart, heapTbl, overTaken, casSeen,
                   base, emitted, started, failed, mainRes, freed>>

TakeWrite(w) ==  \* part_split only
    /\ pc[w] = "take2"
    /\ nextPart' = myPart[w] + 1
    /\ Go(w, IF myPart[w] < NParts THEN "load" ELSE "over")
    /\ UNCHANGED <<found, pend, pub, overOpen, pending, intr, myPart, heapTbl, overTaken, casSeen,
                   base, emitted, started, failed, mainRes, freed>>

\* load (part, 0): every worker's entries and records of the partition into heap_tbl
Load(w) ==
    /\ pc[w] = "load"
    /\ heapTbl' = [heapTbl EXCEPT ![w] = {o \in AllPub \ Over : PartOf[o] = myPart[w]}]
    /\ Go(w, "read")
    /\ UNCHANGED <<found, pend, pub, overOpen, pending, intr, nextPart, myPart, overTaken, casSeen,
                   base, emitted, started, failed, mainRes, freed>>

\* the pages of heap_tbl in order: interrupted () each 64 pages, read_page -> drain_slot_oids
ReadPart(w) ==
    /\ pc[w] = "read"
    /\ IF intr # "none"
         THEN /\ Go(w, "close") /\ UNCHANGED emitted
         ELSE /\ emitted' = [o \in Oids |-> IF o \in heapTbl[w] THEN emitted[o] + 1 ELSE emitted[o]]
              /\ Go(w, "heap")
    /\ UNCHANGED <<found, pend, pub, overOpen, pending, intr, nextPart, myPart, heapTbl, overTaken,
                   casSeen, base, started, failed, mainRes, freed>>

HeapFail(w) ==   \* load (), set_bitmap_page (), a row's error, take_over (), next_over ()
    /\ pc[w] \in {"load", "read", "merging", "overread"} /\ w \in CanFail
    /\ Raise /\ Go(w, "close")
    /\ UNCHANGED <<found, pend, pub, overOpen, pending, nextPart, myPart, heapTbl, overTaken,
                   casSeen, base, emitted, started, mainRes, freed>>

\* take_over (): compare_exchange_strong on m_bm_over_taken (bitmap_heap_step: `!stop && ...`)
OverTry(w) ==
    /\ pc[w] = "over" /\ MUT # "cas_split"
    /\ overTaken' = TRUE
    /\ Go(w, IF overTaken THEN "close" ELSE "merge")
    /\ UNCHANGED <<found, pend, pub, overOpen, pending, intr, nextPart, myPart, heapTbl, casSeen,
                   base, emitted, started, failed, mainRes, freed>>

OverLoad(w) ==   \* cas_split only
    /\ pc[w] = "over" /\ MUT = "cas_split"
    /\ casSeen' = [casSeen EXCEPT ![w] = overTaken]
    /\ Go(w, "over2")
    /\ UNCHANGED <<found, pend, pub, overOpen, pending, intr, nextPart, myPart, heapTbl, overTaken,
                   base, emitted, started, failed, mainRes, freed>>

OverStore(w) ==  \* cas_split only
    /\ pc[w] = "over2"
    /\ overTaken' = TRUE
    /\ Go(w, IF casSeen[w] THEN "close" ELSE "merge")
    /\ UNCHANGED <<found, pend, pub, overOpen, pending, intr, nextPart, myPart, heapTbl, casSeen,
                   base, emitted, started, failed, mainRes, freed>>

\* take_over (): the first worker whose list has tuples is the base (none -> false)
MergeStart(w) ==
    /\ pc[w] = "merge"
    /\ LET has == {j \in W : pub[j] \cap Over # {}}
       IN IF has = {}
            THEN /\ Go(w, "close") /\ UNCHANGED base
            ELSE /\ base' = [base EXCEPT ![w] = CHOOSE j \in has : \A k \in has : j <= k]
                 /\ Go(w, "merging")
    /\ UNCHANGED <<found, pend, pub, overOpen, pending, intr, nextPart, myPart, heapTbl, overTaken,
                   casSeen, emitted, started, failed, mainRes, freed>>

\* ... qfile_append_list () of the others into base->over, qfile_sort_list (Q_DISTINCT), open the scan
MergeEnd(w) ==
    /\ pc[w] = "merging"
    /\ Go(w, "overread")
    /\ UNCHANGED <<found, pend, pub, overOpen, pending, intr, nextPart, myPart, heapTbl, overTaken,
                   casSeen, base, emitted, started, failed, mainRes, freed>>

\* next_over () ... S_END; interrupted () every 1024 rows
OverRead(w) ==
    /\ pc[w] = "overread"
    /\ IF intr # "none"
         THEN UNCHANGED emitted
         ELSE emitted' = [o \in Oids |-> IF o \in AllPub \cap Over THEN emitted[o] + 1 ELSE emitted[o]]
    /\ Go(w, "close")
    /\ UNCHANGED <<found, pend, pub, overOpen, pending, intr, nextPart, myPart, heapTbl, overTaken,
                   casSeen, base, started, failed, mainRes, freed>>

\* task::finalize () -> slot_iterator_index::finalize () -> close_worker (own worker)
Close(w) ==
    /\ pc[w] = "close"
    /\ overOpen' = [overOpen EXCEPT ![w] = FALSE]
    /\ Go(w, "done")
    /\ UNCHANGED <<found, pend, pub, pending, intr, nextPart, myPart, heapTbl, overTaken, casSeen,
                   base, emitted, started, failed, mainRes, freed>>

(* ---------------- main thread ---------------- *)
AllDone == \A w \in started : pc[w] = "done"

\* manager::next (): start_tasks () failed half way -> S_ERROR ... manager::end (): JOB_ENDED, release_workers ()
MainStartFail ==
    /\ started # W /\ mainRes = "wait"
    /\ mainRes' = "err"
    /\ intr' = IF MUT = "main_silent" THEN intr ELSE "ended"
    /\ UNCHANGED <<pc, found, pend, pub, overOpen, pending, nextPart, myPart, heapTbl, overTaken,
                   casSeen, base, emitted, started, failed, freed>>

\* manager::next (): the result handler's read / wait_workers () once every worker is done
MainCollect ==
    /\ started = W /\ mainRes = "wait" /\ AllDone
    /\ mainRes' = IF intr = "none" THEN "ok" ELSE "err"
    /\ UNCHANGED <<pc, found, pend, pub, overOpen, pending, intr, nextPart, myPart, heapTbl,
                   overTaken, casSeen, base, emitted, started, failed, freed>>

\* manager::end () -> release_workers () (waits) ... reset ()/close (): ~input_handler_index
MainFree ==
    /\ mainRes # "wait" /\ AllDone /\ ~freed
    /\ freed' = TRUE
    /\ UNCHANGED <<pc, found, pend, pub, overOpen, pending, intr, nextPart, myPart, heapTbl,
                   overTaken, casSeen, base, emitted, started, failed, mainRes>>

Finished == freed /\ UNCHANGED vars

Progress(w) ==
    \/ Mark(w) \/ IdxEnd(w) \/ IdxIntr(w) \/ EndMarks(w) \/ Leave(w) \/ WaitPass(w) \/ WaitStop(w)
    \/ TakePart(w) \/ TakeRead(w) \/ TakeWrite(w) \/ Load(w) \/ ReadPart(w)
    \/ OverTry(w) \/ OverLoad(w) \/ OverStore(w) \/ MergeStart(w) \/ MergeEnd(w) \/ OverRead(w)
    \/ Close(w)
Fail(w) == IdxFail(w) \/ EndFail(w) \/ HeapFail(w)
Main == MainStartFail \/ MainCollect \/ MainFree

Next == \/ \E w \in W : Progress(w) \/ Fail(w)
        \/ Main
        \/ Finished

Spec == Init /\ [][Next]_vars /\ \A w \in W : WF_vars(Progress(w)) /\ WF_vars(Main)

(* ---------------- properties ---------------- *)
\* user-visible: no row twice, whatever happens
NoDup == \A o \in Oids : emitted[o] <= 1
\* user-visible: a result the main thread returns as complete holds every row the index gave
ResultOK == mainRes = "ok" => /\ ~failed
                               /\ \A o \in Oids : emitted[o] = IF o \in AllFound THEN 1 ELSE 0
\* an error is never turned into a success
ErrNotLost == failed => mainRes # "ok"
\* the barrier: no worker reads the shared marks while a worker still writes them
BarrierSafe == (\E w \in W : pc[w] \in HeapPhases) => \A j \in W : pc[j] \notin IndexPhases
\* qfile_append_list () asserts both lists closed: no list still has its last page fixed at the merge
OverClosedAtMerge == (\E w \in W : pc[w] = "merging") => \A j \in W : ~overOpen[j]
\* only one worker merges and reads the lists
OneTaker == Cardinality({w \in W : pc[w] \in {"merge", "merging", "overread"}}) <= 1
\* the marks are destroyed only once no worker can touch them
FreeLast == freed => \A w \in W : pc[w] \in {"done", "unstarted"}
\* observation: the taker rewrites base->over (qfile_copy_list_id in the sort) while base's owner
\* runs close_worker () on that same QFILE_LIST_ID
OverStructNotShared ==
    ~ \E t, j \in W : t # j /\ pc[t] = "merging" /\ base[t] = j /\ pc[j] = "close"

Ends == <>freed
=============================================================================
