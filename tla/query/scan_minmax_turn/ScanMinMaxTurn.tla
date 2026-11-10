-------------------------- MODULE ScanMinMaxTurn --------------------------
(***************************************************************************)
(* CBRD-28007, PR #8472: the index scan that only looks for MIN or MAX     *)
(* (ACCESS_SPEC_FLAG_ONLY_MIN_MAX_SCAN) reads forward to the first row     *)
(* that passes its data filter, turns back, and takes the first row that   *)
(* passes from the other end.                                              *)
(*                                                                         *)
(* Rows are the positions 1..n of the key range in index order, so the     *)
(* answer - MAX on an ascending index, MIN on a descending one - is the    *)
(* last position that passes.  Each action is one branch of the code;      *)
(* README.md maps them to file:line.                                       *)
(*                                                                         *)
(* FIX = FALSE is develop: S_GO_BACKWARD resets curr_oidno only.           *)
(* FIX = TRUE is the PR: it also points curr_oidp at the first OID, as     *)
(* S_BEFORE does.                                                          *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS
    MaxN,     \* most rows in the key range
    MaxCap,   \* largest OID buffer capacity (oid_list->capacity)
    FIX       \* TRUE: the PR, FALSE: develop

ASSUME MaxN \in Nat \ {0} /\ MaxCap \in Nat \ {0} /\ FIX \in BOOLEAN

NoRow == 0                    \* no row: MAX is NULL, or a buffer slot never filled
Slots == 0 .. (MaxCap - 1)

VARIABLES
    n,        \* rows in the key range
    cap,      \* OID buffer capacity of this scan
    pass,     \* the rows the data filter keeps
    dir,      \* "fwd" before the turn, "bwd" after it (use_desc_index flipped)
    nextKey,  \* the next position the B-tree range search reads in dir
    buf,      \* oid_list->oidp: slot -> row; slots past the batch keep the rows they had
    cnt,      \* isidp->oids_count: rows of the batch in buf
    oidno,    \* isidp->curr_oidno
    ptr,      \* the slot isidp->curr_oidp points at
    pc,       \* "before" | "check" | "next" | "goback" | "done"
    result    \* the accumulator of MAX

vars == <<n, cap, pass, dir, nextKey, buf, cnt, oidno, ptr, pc, result>>

Min(a, b) == IF a < b THEN a ELSE b

(* the batch call_get_next_index_oidset () reads next: at most cap rows from nextKey in dir *)
BatchLen == IF dir = "fwd" THEN (IF nextKey > n THEN 0 ELSE Min(cap, n - nextKey + 1))
                           ELSE (IF nextKey < 1 THEN 0 ELSE Min(cap, nextKey))
BatchRow(s) == IF dir = "fwd" THEN nextKey + s ELSE nextKey - s
ReadBatch ==
    /\ cnt' = BatchLen
    /\ buf' = [s \in Slots |-> IF s < BatchLen THEN BatchRow(s) ELSE buf[s]]
    /\ nextKey' = IF dir = "fwd" THEN nextKey + BatchLen ELSE nextKey - BatchLen

Init ==
    /\ n \in 1 .. MaxN
    /\ cap \in 1 .. MaxCap
    /\ pass \in SUBSET (1 .. n)
    /\ dir = "fwd"
    /\ nextKey = 1
    /\ buf = [s \in Slots |-> NoRow]
    /\ cnt = 0
    /\ oidno = 0
    /\ ptr = 0
    /\ pc = "before"
    /\ result = NoRow

(* S_BEFORE: the first batch, curr_oidno = 0, curr_oidp = the first OID *)
Before ==
    /\ pc = "before"
    /\ BatchLen > 0
    /\ ReadBatch
    /\ oidno' = 0
    /\ ptr' = 0
    /\ pc' = "check"
    /\ UNCHANGED <<n, cap, pass, dir, result>>

(* scan_next_index_lookup_heap (): fetch the row at curr_oidp and evaluate the data filter.  A row
   that passes goes to qexec_end_one_iteration () and then to the MIN/MAX check of qexec_intprt_fnc () *)
Check ==
    /\ pc = "check"
    /\ IF buf[ptr] \in pass
         THEN IF dir = "fwd"
                THEN \* the plain MAX accumulates the row; MAX is not ended in this direction, so the
                     \* scan turns: use_desc_index flipped, bts_reset_scan (), position = S_GO_BACKWARD
                     /\ result' = buf[ptr]
                     /\ dir' = "bwd"
                     /\ nextKey' = n
                     /\ pc' = "goback"
                     /\ UNCHANGED <<buf, cnt, oidno, ptr>>
                ELSE \* min_max_optimized: the first row after the turn replaces the accumulator, is_ended
                     /\ result' = buf[ptr]
                     /\ pc' = "done"
                     /\ UNCHANGED <<dir, nextKey, buf, cnt, oidno, ptr>>
         ELSE /\ pc' = "next"
              /\ UNCHANGED <<dir, nextKey, buf, cnt, oidno, ptr, result>>
    /\ UNCHANGED <<n, cap, pass>>

(* S_ON: the next OID of the batch; else the next batch with curr_oidp = the first OID;
   else S_END, and MAX keeps what it accumulated *)
Advance ==
    /\ pc = "next"
    /\ IF oidno < cnt - 1
         THEN /\ oidno' = oidno + 1
              /\ ptr' = oidno + 1
              /\ pc' = "check"
              /\ UNCHANGED <<buf, cnt, nextKey>>
         ELSE IF BatchLen = 0
                THEN /\ pc' = "done"
                     /\ UNCHANGED <<buf, cnt, nextKey, oidno, ptr>>
                ELSE /\ ReadBatch
                     /\ oidno' = 0
                     /\ ptr' = 0
                     /\ pc' = "check"
    /\ UNCHANGED <<n, cap, pass, dir, result>>

(* S_GO_BACKWARD: read the batch again from the other end, curr_oidno = 0 *)
GoBack ==
    /\ pc = "goback"
    /\ BatchLen > 0
    /\ ReadBatch
    /\ oidno' = 0
    /\ ptr' = IF FIX THEN 0 ELSE ptr
    /\ pc' = "check"
    /\ UNCHANGED <<n, cap, pass, dir, result>>

Done == pc = "done" /\ UNCHANGED vars

Next == Before \/ Check \/ Advance \/ GoBack \/ Done

Spec == Init /\ [][Next]_vars /\ WF_vars(Before \/ Check \/ Advance \/ GoBack)

-----------------------------------------------------------------------------
Last(S) == CHOOSE x \in S : \A y \in S : y <= x

TypeOK ==
    /\ n \in 1 .. MaxN
    /\ cap \in 1 .. MaxCap
    /\ pass \subseteq 1 .. n
    /\ dir \in {"fwd", "bwd"}
    /\ nextKey \in 0 .. MaxN + 1
    /\ buf \in [Slots -> 0 .. MaxN]
    /\ cnt \in 0 .. MaxCap
    /\ oidno \in Slots
    /\ ptr \in Slots
    /\ pc \in {"before", "check", "next", "goback", "done"}
    /\ result \in 0 .. MaxN

(* what the query returns: the last row that passes, NULL when none passes *)
MaxIsRight == pc = "done" => result = (IF pass = {} THEN NoRow ELSE Last(pass))

(* curr_oidp points at curr_oidno, as S_BEFORE and S_ON keep it *)
PtrAtOidno == pc = "check" => ptr = oidno

(* the scan reads only the OIDs of the batch in the buffer *)
PtrInBatch == pc = "check" => ptr < cnt

(* the scan ends *)
Ends == <>(pc = "done")
=============================================================================
