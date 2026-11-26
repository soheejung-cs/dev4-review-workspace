---------------------------- MODULE BtreeLatchOrder ----------------------------
(* CBRD-28421: page latch order between B-tree scans, B-tree writers and heap pages.
   Each thread runs one code path ("program", see Prog) as a sequence of page fixes and unfixes.
   Page latch rules follow pgbuf_latch_bcb_upon_fix () / pgbuf_wakeup_reader_writer () as modelled in
   tla/storage/latch_waiter_flag (develop after CBRD-28163: waiter_exists == someone is queued).
   No timeout: a state where some thread waits and nothing can move is the
   page_latch_timeout_in_msecs expiry -> ER_PAGE_LATCH_TIMEDOUT / debug assert (0) of the issue.
   Pages: Root (non-leaf), leaves L1 < L2 (left to right), one heap page H. *)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS Progs,        \* sequence of program names, one thread each (the driver enumerates the combinations)
          FIX           \* "develop" | "datafilter" (proposed fix 1: subquery terms leave the key filter)

Pages == {"Root", "L1", "L2", "H"}
Threads == 1..Len(Progs)
Assign == Progs

F(p, m)  == [op |-> "fix", pg |-> p, m |-> m, c |-> FALSE]     \* PGBUF_UNCONDITIONAL_LATCH
FC(p, m) == [op |-> "fix", pg |-> p, m |-> m, c |-> TRUE]      \* PGBUF_CONDITIONAL_LATCH
U(p)     == [op |-> "unfix", pg |-> p, m |-> "N", c |-> FALSE]

\* index scan open: root, then the leaf, root let go (scan_open_index_scan () -> btree_find_boundary_leaf () /
\* btree_locate_key ()); the leaf stays held
Descend(l) == << F("Root", "R"), F(l, "R"), U("Root") >>
\* a whole index scan of a subquery that ends on leaf l
SubIdx(l)  == Descend(l) \o << U(l) >>
\* a whole heap scan of a subquery
SubHeap    == << F("H", "R"), U("H") >>

KeyFilter(x, sub) ==
  IF FIX = "develop"
  THEN Descend(x) \o sub \o << U(x) >>                      \* eval_key_filter () with leaf x held, btree.c:21991
  ELSE Descend(x) \o << U(x), F("H", "R") >> \o sub \o << U("H") >>
                                                            \* data filter after the leaf is let go, heap page held
                                                            \* (scan_next_index_lookup_heap -> eval_data_filter, scan_manager.c:7035)

Prog(n) ==
  CASE n = "FWD"    -> Descend("L1") \o << F("L2", "R"), U("L1"), U("L2") >>       \* next leaf unconditional, btree.c:21589
    [] n = "DESC"   -> Descend("L2") \o << FC("L1", "R"), U("L2"), U("L1") >>      \* prev leaf conditional, btree.c:21575
    [] n = "INS1"   -> << F("Root", "R"), F("L1", "W"), U("Root"), U("L1") >>      \* child leaf W under parent, btree.c:33592
    [] n = "INS2"   -> << F("Root", "R"), F("L2", "W"), U("Root"), U("L2") >>
    [] n = "INSW1"  -> << F("Root", "W"), F("L1", "W"), U("Root"), U("L1") >>      \* restart with nonleaf W after promote
    [] n = "INSW2"  -> << F("Root", "W"), F("L2", "W"), U("Root"), U("L2") >>      \*   failure btree.c:33385/33657/33687
    [] n = "SPLIT1" -> << F("Root", "W"), F("L1", "W"), F("L2", "W"),               \* btree_split_node -> btree_get_next_page
                          U("L2"), U("L1"), U("Root") >>                            \*   btree.c:18874
    [] n = "MERGE"  -> << F("Root", "W"), F("L1", "W"), F("L2", "W"),               \* btree_merge_node_and_advance
                          U("L2"), U("L1"), U("Root") >>                            \*   btree.c:37105 (vacuum / delete)
    [] n = "HUPD"   -> << F("H", "W"), U("H") >>                                    \* heap insert/update/delete/vacuum on H
    [] n = "HFS1"   -> << F("H", "R") >> \o SubIdx("L1") \o << U("H") >>           \* heap scan: eval_data_filter () with
    [] n = "HFS2"   -> << F("H", "R") >> \o SubIdx("L2") \o << U("H") >>           \*   H held, scan_manager.c:6124
    [] n = "BULK1"  -> << F("H", "W"), F("Root", "R"), F("L1", "W"),             \* locator_multi_insert_force (): new heap
                          U("Root"), U("L1"), U("H") >>                             \*   page W kept (heap_file.c:26874) over
    [] n = "BULK2"  -> << F("H", "W"), F("Root", "R"), F("L2", "W"),             \*   the index insert of
                          U("Root"), U("L2"), U("H") >>                             \*   locator_insert_force (), locator_sr.c:16843
    [] n = "KF11"   -> KeyFilter("L1", SubIdx("L1"))
    [] n = "KF12"   -> KeyFilter("L1", SubIdx("L2"))
    [] n = "KF21"   -> KeyFilter("L2", SubIdx("L1"))
    [] n = "KF22"   -> KeyFilter("L2", SubIdx("L2"))
    [] n = "KFH1"   -> KeyFilter("L1", SubHeap)
    [] n = "KFH2"   -> KeyFilter("L2", SubHeap)

VARIABLES mode,   \* [Pages -> {"N","R","W"}]
          cnt,    \* [Pages -> [Threads -> Nat]]  fix count per holder
          queue,  \* [Pages -> Seq(<<thread, mode>>)]  blocked requests, oldest first
          pc,     \* [Threads -> Nat] next op index
          st      \* [Threads -> {"run","wait","abort","done"}]
vars == << mode, cnt, queue, pc, st >>

P(t) == Prog(Assign[t])
Op(t) == P(t)[pc[t]]
Held(t) == { p \in Pages : cnt[p][t] > 0 }
Holders(p) == { t \in Threads : cnt[p][t] > 0 }

Init == /\ mode = [p \in Pages |-> "N"]
        /\ cnt = [p \in Pages |-> [t \in Threads |-> 0]]
        /\ queue = [p \in Pages |-> << >>]
        /\ pc = [t \in Threads |-> 1]
        /\ st = [t \in Threads |-> "run"]

Grant(t, p, m) == /\ mode' = [mode EXCEPT ![p] = IF cnt[p][t] > 0 THEN mode[p] ELSE m]
                  /\ cnt' = [cnt EXCEPT ![p][t] = @ + 1]
                  /\ pc' = [pc EXCEPT ![t] = @ + 1]
                  /\ UNCHANGED << queue, st >>

\* pgbuf_latch_bcb_upon_fix (): holder re-fix granted; idle page granted; READ on READ granted only when no waiter
CanGrant(t, p, m) == \/ cnt[p][t] > 0
                     \/ mode[p] = "N"
                     \/ (m = "R" /\ mode[p] = "R" /\ queue[p] = << >>)

DoFix(t) == /\ st[t] = "run" /\ pc[t] <= Len(P(t)) /\ Op(t).op = "fix"
            /\ LET p == Op(t).pg  m == Op(t).m IN
               IF CanGrant(t, p, m) THEN Grant(t, p, m)
               ELSE IF Op(t).c
                    THEN /\ st' = [st EXCEPT ![t] = "abort"]   \* ER_DESC_ISCAN_ABORTED: give up, let everything go
                         /\ UNCHANGED << mode, cnt, queue, pc >>
                    ELSE /\ queue' = [queue EXCEPT ![p] = Append(@, << t, m >>)]   \* pgbuf_block_bcb ()
                         /\ st' = [st EXCEPT ![t] = "wait"]
                         /\ UNCHANGED << mode, cnt, pc >>

\* unfix of p by t; when the last holder leaves, pgbuf_wakeup_reader_writer (): grant the head; when the head is a
\* reader, every queued reader too (writers stay queued); stop after a writer
Release(t, p, adv) ==
  LET c2 == [cnt EXCEPT ![p][t] = @ - 1]
      free == \A u \in Threads : c2[p][u] = 0
      q == queue[p]
      headW == Len(q) > 0 /\ q[1][2] = "W"
      wokenIdx == IF ~free \/ Len(q) = 0 THEN {}
                  ELSE IF headW THEN {1}
                  ELSE { i \in 1..Len(q) : q[i][2] = "R" }
      woken == { q[i][1] : i \in wokenIdx }
      rest == SelectSeq(q, LAMBDA e : e[1] \notin woken)
  IN /\ cnt' = [c2 EXCEPT ![p] = [u \in Threads |-> IF u \in woken THEN 1 ELSE c2[p][u]]]
     /\ mode' = [mode EXCEPT ![p] = IF ~free THEN @ ELSE IF woken = {} THEN "N" ELSE IF headW THEN "W" ELSE "R"]
     /\ queue' = [queue EXCEPT ![p] = rest]
     /\ pc' = [u \in Threads |-> IF u \in woken \/ (u = t /\ adv) THEN pc[u] + 1 ELSE pc[u]]
     /\ st' = [u \in Threads |-> IF u \in woken THEN "run" ELSE st[u]]

DoUnfix(t) == /\ st[t] = "run" /\ pc[t] <= Len(P(t)) /\ Op(t).op = "unfix"
              /\ Release(t, Op(t).pg, TRUE)

AbortRelease(t) == /\ st[t] = "abort" /\ Held(t) # {}
                   /\ \E p \in Held(t) : Release(t, p, FALSE)

Finish(t) == /\ \/ (st[t] = "run" /\ pc[t] > Len(P(t)))
                \/ (st[t] = "abort" /\ Held(t) = {})
             /\ st' = [st EXCEPT ![t] = "done"]
             /\ UNCHANGED << mode, cnt, queue, pc >>

AllDone == \A t \in Threads : st[t] = "done"
Terminating == AllDone /\ UNCHANGED vars

Next == \/ \E t \in Threads : DoFix(t) \/ DoUnfix(t) \/ AbortRelease(t) \/ Finish(t)
        \/ Terminating

Spec == Init /\ [][Next]_vars

TypeOK == /\ mode \in [Pages -> {"N", "R", "W"}]
          /\ st \in [Threads -> {"run", "wait", "abort", "done"}]
\* latch compatibility: a W page has one holder
LatchSafe == \A p \in Pages : mode[p] = "W" => Cardinality(Holders(p)) = 1
\* the deadlock, as an invariant (TLC's deadlock check finds the same states)
NoStuck == (ENABLED Next) \/ AllDone
=============================================================================
