------------------------- MODULE ClearAllListsSibling -------------------------
(***************************************************************************)
(* CBRD-27959, PR #8436: after each outer row of the block it runs,        *)
(* qexec_intprt_fnc () clears the list files that the row's correlated     *)
(* subqueries left (qexec_clear_all_lists ()).  develop walked the block's *)
(* next as well.  The next of an operand of a set operation is the next    *)
(* operand in the parent's aptr list, and the parallel subquery executor   *)
(* runs those operands as jobs: the root thread runs the first one and the *)
(* other jobs go to a queue that a worker and the root pop.                *)
(*                                                                         *)
(* Operands 1..NOps are the aptr list in order (next (i) = i + 1).  An     *)
(* operand in hasD has an IN subquery D_i, correlated with its outer       *)
(* table, on the dptr_list of its inner scan and linked to the IN term.    *)
(* Each outer row of the operand clears D_i's list (qexec_execute_dptr_list *)
(* ()), the IN term runs D_i (a new list: a temp file from the pool, then  *)
(* tuple writes) and reads it, and the end-of-row clear destroys it, which *)
(* gives the temp file back.  Each action is one branch of the code;       *)
(* README.md maps them to file:line.                                       *)
(*                                                                         *)
(* FIX = FALSE is develop: the end-of-row clear of operand i also clears   *)
(* D_j of every operand j after i.  FIX = TRUE is the PR: D_i only.        *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    NOps,          \* operands in the parent's aptr list
    Rows,          \* outer rows each operand reads
    NWorkers,      \* px workers next to the root thread; 0 = no px executor (serial)
    TFs,           \* temp file structures (QMGR_TEMP_FILE)
    NONE,          \* a list id without a temp file (tfile_vfid == NULL)
    DChoices,      \* the sets of operands that may have the correlated subquery
    ReuseChoices,  \* qexec_is_list_reusable_by_truncate () of D: FALSE = destroyed and opened again each row
    FIX            \* TRUE: the PR, FALSE: develop

ASSUME NOps \in Nat \ {0} /\ Rows \in Nat \ {0} /\ NWorkers \in Nat
ASSUME NONE \notin TFs /\ ReuseChoices \subseteq BOOLEAN /\ FIX \in BOOLEAN

Ops == 1 .. NOps
Thr == 0 .. NWorkers                 \* thread 0 is the root
FREE == 0                            \* the owner of a temp file structure in the pool
Errs == {"none", "null", "uaf", "shared", "double", "stale"}
PCs == {"idle", "scan", "prerd", "prefr", "open", "w1", "w2", "probe",
        "endrd", "endfr", "jendrd", "jendfr", "done"}

(* the values a cfg gives DChoices through <- *)
AllD == SUBSET Ops
FirstOnly == {{1}}
SecondOnly == {{2}}

VARIABLES
    hasD,     \* the operands with the correlated subquery
    reuse,    \* the subquery's list is truncated and reused instead of destroyed
    lid,      \* D_i->list_id->tfile_vfid
    content,  \* the outer row whose tuples D_i's list holds (0: none)
    owner,    \* temp file structure -> the subquery whose list holds it, FREE in the pool
    nextJob,  \* the next operand the job queue gives out
    job,      \* the operand a thread runs (0: none)
    row,      \* the outer row of that operand
    pc,
    ptr,      \* the tfile_vfid that qfile_destroy_list () read
    tgt,      \* the subquery the current end-of-row clear works on
    err,      \* the first failure seen
    foreign   \* a thread freed the list of a subquery of another job

vars == <<hasD, reuse, lid, content, owner, nextJob, job, row, pc, ptr, tgt, err, foreign>>

Min(S) == CHOOSE m \in S : \A k \in S : m <= k

(* the subqueries the end-of-row clear of operand i works on: develop walks i, i + 1, .., NOps
   (qexec_clear_all_lists () over next), the PR i only *)
Targets(i) == IF FIX THEN hasD \cap {i} ELSE hasD \cap (i .. NOps)
NextTarget(i, j) == LET S == {k \in Targets(i) : k > j} IN IF S = {} THEN 0 ELSE Min(S)

(* what the owner meets when it uses D_i's list *)
Access(i) == IF lid[i] = NONE THEN "null"
             ELSE IF owner[lid[i]] = FREE THEN "uaf"
             ELSE IF owner[lid[i]] # i THEN "shared"
             ELSE "none"

Fail(e) == err' = IF err = "none" /\ e # "none" THEN e ELSE err

Init ==
    /\ hasD \in DChoices
    /\ reuse \in ReuseChoices
    /\ lid = [i \in Ops |-> NONE]
    /\ content = [i \in Ops |-> 0]
    /\ owner = [f \in TFs |-> FREE]
    /\ nextJob = 2
    /\ job = [t \in Thr |-> IF t = 0 THEN 1 ELSE 0]
    /\ row = [t \in Thr |-> IF t = 0 THEN 1 ELSE 0]
    /\ pc = [t \in Thr |-> IF t = 0 THEN "scan" ELSE "idle"]
    /\ ptr = [t \in Thr |-> NONE]
    /\ tgt = [t \in Thr |-> 0]
    /\ err = "none"
    /\ foreign = FALSE

(* the row is done: the next outer row, or the end of the job *)
RowDone(t) ==
    IF row[t] < Rows
    THEN /\ row' = [row EXCEPT ![t] = row[t] + 1]
         /\ pc' = [pc EXCEPT ![t] = "scan"]
         /\ UNCHANGED job
    ELSE IF NWorkers > 0 /\ job[t] \in hasD
         THEN /\ pc' = [pc EXCEPT ![t] = "jendrd"]
              /\ UNCHANGED <<row, job>>
         ELSE /\ pc' = [pc EXCEPT ![t] = "idle"]
              /\ job' = [job EXCEPT ![t] = 0]
              /\ row' = [row EXCEPT ![t] = 0]

(* qexec_clear_all_lists () after the row: its first target, or nothing to clear *)
EndRow(t) ==
    LET n == NextTarget(job[t], 0) IN
    IF n = 0
    THEN RowDone(t) /\ UNCHANGED tgt
    ELSE /\ tgt' = [tgt EXCEPT ![t] = n]
         /\ pc' = [pc EXCEPT ![t] = "endrd"]
         /\ UNCHANGED <<row, job>>

(* qfile_destroy_list () after it read tfile_vfid: qmgr_free_list_temp_file () gives the temp file back
   and qfile_clear_list_id () empties the list id.  A temp file that is not this list's any more (given
   back already, or given back and handed to another list) is freed twice. *)
Free(t, j) ==
    LET p == ptr[t] IN
    /\ IF p # NONE
       THEN /\ owner' = [owner EXCEPT ![p] = FREE]
            /\ Fail(IF owner[p] # j THEN "double" ELSE "none")
            /\ foreign' = (foreign \/ j # job[t])
       ELSE UNCHANGED <<owner, err, foreign>>
    /\ lid' = [lid EXCEPT ![j] = NONE]
    /\ content' = [content EXCEPT ![j] = 0]

(* the job queue: the root ran the first operand (m_job); then the root and the worker pop the rest *)
Pop(t) ==
    /\ pc[t] = "idle"
    /\ IF nextJob <= NOps
       THEN /\ job' = [job EXCEPT ![t] = nextJob]
            /\ row' = [row EXCEPT ![t] = 1]
            /\ pc' = [pc EXCEPT ![t] = "scan"]
            /\ nextJob' = nextJob + 1
       ELSE /\ pc' = [pc EXCEPT ![t] = "done"]
            /\ UNCHANGED <<job, row, nextJob>>
    /\ UNCHANGED <<hasD, reuse, lid, content, owner, ptr, tgt, err, foreign>>

(* scan_next_scan (): the next outer row (and the inner row it joins) *)
Scan(t) ==
    /\ pc[t] = "scan"
    /\ IF job[t] \in hasD
       THEN /\ pc' = [pc EXCEPT ![t] = "prerd"]
            /\ UNCHANGED <<row, job, tgt>>
       ELSE EndRow(t)
    /\ UNCHANGED <<hasD, reuse, lid, content, owner, nextJob, ptr, err, foreign>>

(* qexec_execute_dptr_list () -> qexec_clear_head_lists_with_truncate () on D_i *)
PreRd(t) ==
    LET i == job[t] IN
    /\ pc[t] = "prerd"
    /\ IF reuse
       THEN \* qfile_truncate_list (): the list keeps its temp file, its tuples go
            /\ IF lid[i] # NONE THEN Fail(Access(i)) ELSE UNCHANGED err
            /\ content' = [content EXCEPT ![i] = 0]
            /\ pc' = [pc EXCEPT ![t] = "open"]
            /\ UNCHANGED ptr
       ELSE \* qexec_clear_xasl_head () -> qfile_destroy_list () reads tfile_vfid
            /\ ptr' = [ptr EXCEPT ![t] = lid[i]]
            /\ pc' = [pc EXCEPT ![t] = "prefr"]
            /\ UNCHANGED <<content, err>>
    /\ UNCHANGED <<hasD, reuse, lid, owner, nextJob, job, row, tgt, foreign>>

PreFr(t) ==
    /\ pc[t] = "prefr"
    /\ Free(t, job[t])
    /\ pc' = [pc EXCEPT ![t] = "open"]
    /\ UNCHANGED <<hasD, reuse, nextJob, job, row, ptr, tgt>>

(* the IN term runs the linked subquery (status initial): qexec_start_mainblock_iterations () opens its
   list (qfile_open_list () -> qmgr_create_new_temp_file ()); a truncated list is written as it is *)
Open(t) ==
    LET i == job[t] IN
    /\ pc[t] = "open"
    /\ IF reuse /\ lid[i] # NONE
       THEN /\ Fail(Access(i))
            /\ UNCHANGED <<lid, owner>>
       ELSE \E f \in TFs :
              /\ owner[f] = FREE
              /\ lid' = [lid EXCEPT ![i] = f]
              /\ owner' = [owner EXCEPT ![f] = i]
              /\ UNCHANGED err
    /\ pc' = [pc EXCEPT ![t] = "w1"]
    /\ UNCHANGED <<hasD, reuse, content, nextJob, job, row, ptr, tgt, foreign>>

(* the subquery's tuples: qfile_add_tuple_to_list () -> qmgr_get_new_page (list_id->tfile_vfid) *)
Write(t) ==
    LET i == job[t] IN
    /\ pc[t] \in {"w1", "w2"}
    /\ Fail(Access(i))
    /\ IF pc[t] = "w2"
       THEN /\ content' = [content EXCEPT ![i] = row[t]]
            /\ pc' = [pc EXCEPT ![t] = "probe"]
       ELSE /\ pc' = [pc EXCEPT ![t] = "w2"]
            /\ UNCHANGED content
    /\ UNCHANGED <<hasD, reuse, lid, owner, nextJob, job, row, ptr, tgt, foreign>>

(* eval_pred (): the IN term checks the subquery's status and tuple_cnt, then reads its list *)
Probe(t) ==
    LET i == job[t] IN
    /\ pc[t] = "probe"
    /\ Fail(IF lid[i] = NONE \/ content[i] # row[t] THEN "stale" ELSE Access(i))
    /\ EndRow(t)
    /\ UNCHANGED <<hasD, reuse, lid, content, owner, nextJob, ptr, foreign>>

(* qexec_clear_all_lists () -> qexec_clear_scan_all_lists () -> qexec_clear_head_lists () ->
   qexec_clear_xasl_head () -> qfile_destroy_list () reads tfile_vfid of the target *)
EndRd(t) ==
    /\ pc[t] = "endrd"
    /\ ptr' = [ptr EXCEPT ![t] = lid[tgt[t]]]
    /\ pc' = [pc EXCEPT ![t] = "endfr"]
    /\ UNCHANGED <<hasD, reuse, lid, content, owner, nextJob, job, row, tgt, err, foreign>>

EndFr(t) ==
    LET n == NextTarget(job[t], tgt[t]) IN
    /\ pc[t] = "endfr"
    /\ Free(t, tgt[t])
    /\ IF n = 0
       THEN RowDone(t) /\ UNCHANGED tgt
       ELSE /\ tgt' = [tgt EXCEPT ![t] = n]
            /\ pc' = [pc EXCEPT ![t] = "endrd"]
            /\ UNCHANGED <<row, job>>
    /\ UNCHANGED <<hasD, reuse, nextJob, ptr>>

(* the job's end: execute_job_internal () -> qexec_clear_xasl_for_parallel_aptr () clears the operand's
   subtree, D_i included, and not its next *)
JEndRd(t) ==
    /\ pc[t] = "jendrd"
    /\ ptr' = [ptr EXCEPT ![t] = lid[job[t]]]
    /\ pc' = [pc EXCEPT ![t] = "jendfr"]
    /\ UNCHANGED <<hasD, reuse, lid, content, owner, nextJob, job, row, tgt, err, foreign>>

JEndFr(t) ==
    /\ pc[t] = "jendfr"
    /\ Free(t, job[t])
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ job' = [job EXCEPT ![t] = 0]
    /\ row' = [row EXCEPT ![t] = 0]
    /\ UNCHANGED <<hasD, reuse, nextJob, ptr, tgt>>

Done == \A t \in Thr : pc[t] = "done"

Terminating == Done /\ UNCHANGED vars

Step(t) ==
    \/ Pop(t) \/ Scan(t) \/ PreRd(t) \/ PreFr(t) \/ Open(t) \/ Write(t) \/ Probe(t)
    \/ EndRd(t) \/ EndFr(t) \/ JEndRd(t) \/ JEndFr(t)

Next == Terminating \/ \E t \in Thr : Step(t)

Spec == Init /\ [][Next]_vars /\ WF_vars(\E t \in Thr : Step(t))

-----------------------------------------------------------------------------
TypeOK ==
    /\ hasD \subseteq Ops
    /\ reuse \in BOOLEAN
    /\ lid \in [Ops -> TFs \cup {NONE}]
    /\ content \in [Ops -> 0 .. Rows]
    /\ owner \in [TFs -> Ops \cup {FREE}]
    /\ nextJob \in 1 .. NOps + 1
    /\ job \in [Thr -> 0 .. NOps]
    /\ row \in [Thr -> 0 .. Rows]
    /\ pc \in [Thr -> PCs]
    /\ ptr \in [Thr -> TFs \cup {NONE}]
    /\ tgt \in [Thr -> 0 .. NOps]
    /\ err \in Errs
    /\ foreign \in BOOLEAN

(* what the user sees: no NULL list, no freed or shared temp file under a list in use, no temp file
   given back twice, and the IN term reads the tuples of its own row *)
Safe == err = "none"

(* what the PR claims: the per-row clear of a job frees only the lists of that job's subqueries *)
OnlyOwnLists == ~foreign

(* every temp file is back in the pool when every thread is done *)
NoLeak == Done => \A f \in TFs : owner[f] = FREE

Ends == <>Done
=============================================================================
