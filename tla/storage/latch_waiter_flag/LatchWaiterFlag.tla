--------------------------- MODULE LatchWaiterFlag ---------------------------
(***************************************************************************)
(* CBRD-28163, PR #8587: a page latch waiter that gives up (latch timeout  *)
(* or interrupt) leaves the BCB wait queue in                              *)
(* pgbuf_timed_sleep_error_handling ().  develop left atomic_latch's       *)
(* waiter_exists set on every exit, so a page that only READ latches hold  *)
(* kept sending later READ requests to the queue (pgbuf_latch_bcb_upon_fix *)
(* () grants READ on READ only without the flag) until the last holder     *)
(* unlatched.  The PR clears the flag on the way out when no blocked       *)
(* reader or writer is left, as pgbuf_wakeup_reader_writer () does.        *)
(*                                                                         *)
(* Thread H holds the page READ-latched at the start (session A's scan).   *)
(* Each requester in Req asks for the latch mode kind[r] (Init picks every *)
(* combination), is granted or queues, and may give up its wait if it is   *)
(* in CanTimeout.  Every action is one critical section of the code -- the *)
(* BCB mutex section, or one CAS on the latch word -- so TLC walks every    *)
(* interleaving of fix, unfix, wakeup and give-up.  README.md maps the     *)
(* actions to file:line.                                                   *)
(*                                                                         *)
(* FIX = "develop": the give-up path leaves the flag as it is.             *)
(* FIX = "pr": it clears the flag when no blocked reader or writer is left.*)
(* FIX = "always": a mutant that clears it without the check.              *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS
    H,            \* the thread that holds the page READ-latched at the start
    Req,          \* the threads that request the page latch later
    CanTimeout,   \* the requesters whose latch wait may end by timeout or interrupt
    FIX           \* "develop", "pr" or "always"

ASSUME H \notin Req /\ CanTimeout \subseteq Req /\ FIX \in {"develop", "pr", "always"}

Thr == {H} \cup Req

VARIABLES
    mode,     \* atomic_latch.latch_mode: "N" (PGBUF_NO_LATCH), "R", "W"
    fcnt,     \* atomic_latch.fcnt
    wex,      \* atomic_latch.waiter_exists
    q,        \* the BCB wait queue (bufptr->next_wait_thrd chain), front first
    kind,     \* the latch mode each requester asks for (fixed by Init)
    req,      \* request_latch_mode of a queued requester: "R", "W", or "NO" once it gave up
    resumed,  \* resume_status == THREAD_PGBUF_RESUMED: a waker granted the latch and unlinked it
    pc

vars == <<mode, fcnt, wex, q, kind, req, resumed, pc>>

\* pgbuf_is_exist_blocked_reader_writer (): a queued READ or WRITE request
Blocked(s) == \E i \in 1 .. Len(s) : req[s[i]] \in {"R", "W"}

Remove(s, t) == SelectSeq(s, LAMBDA x : x # t)

\* pgbuf_wakeup_reader_writer () after the last fix went (mode "N", fcnt 0): unlink the waiters that
\* gave up, grant the front request, then every READ request while the latch is READ (a WRITE waiter
\* is passed over -- "look for other readers"), and stop at the first request after a WRITE grant.
RECURSIVE Wake(_, _, _, _, _)
Wake(m, f, rest, kept, g) ==
    IF rest = << >> THEN [m |-> m, f |-> f, q |-> kept, g |-> g]
    ELSE LET t == Head(rest) IN
         IF req[t] = "NO" THEN Wake(m, f, Tail(rest), kept, g)
         ELSE IF m = "N" THEN Wake(req[t], f + 1, Tail(rest), kept, g \cup {t})
         ELSE IF m = "R" /\ req[t] = "R" THEN Wake(m, f + 1, Tail(rest), kept, g \cup {t})
         ELSE IF m = "R" THEN Wake(m, f, Tail(rest), Append(kept, t), g)
         ELSE [m |-> m, f |-> f, q |-> kept \o rest, g |-> g]

\* case 3 of pgbuf_timed_sleep_error_handling (): the thread that gave up was at the front; hand the
\* READ latch to the READ requests now at the front, stop at the first other one.
RECURSIVE Succ(_, _, _, _)
Succ(m, f, rest, g) ==
    IF rest # << >> /\ m = "R" /\ req[Head(rest)] = "R"
    THEN Succ(m, f + 1, Tail(rest), g \cup {Head(rest)})
    ELSE [f |-> f, q |-> rest, g |-> g]

Init ==
    /\ kind \in [Req -> {"R", "W"}]
    /\ mode = "R" /\ fcnt = 1 /\ wex = FALSE /\ q = << >>
    /\ req = [r \in Req |-> "NO"]
    /\ resumed = [r \in Req |-> FALSE]
    /\ pc = [t \in Thr |-> IF t = H THEN "hold" ELSE "idle"]

\* pgbuf_latch_bcb_upon_fix () under the BCB mutex -- idle grant, READ on READ without the flag (the
\* lock-free READ fix grants the same case), or set the flag and queue (pgbuf_block_bcb ())
Fix(r) ==
    /\ pc[r] = "idle"
    /\ IF mode = "N"
         THEN /\ mode' = kind[r] /\ fcnt' = 1
              /\ pc' = [pc EXCEPT ![r] = "hold"]
              /\ UNCHANGED <<wex, q, req, resumed>>
         ELSE IF kind[r] = "R" /\ mode = "R" /\ ~wex
         THEN /\ fcnt' = fcnt + 1
              /\ pc' = [pc EXCEPT ![r] = "hold"]
              /\ UNCHANGED <<mode, wex, q, req, resumed>>
         ELSE /\ wex' = TRUE
              /\ q' = Append(q, r)
              /\ req' = [req EXCEPT ![r] = kind[r]]
              /\ pc' = [pc EXCEPT ![r] = "sleep"]
              /\ UNCHANGED <<mode, fcnt, resumed>>
    /\ UNCHANGED kind

\* pgbuf_unfix (): the lock-free READ unfix or pgbuf_unlatch_bcb_upon_unfix () (BCB mutex), which
\* runs pgbuf_wakeup_reader_writer () when the last fix goes; the wakeup clears the flag when no
\* blocked request is left and never sets it
Unfix(t) ==
    /\ pc[t] = "hold"
    /\ pc' = [pc EXCEPT ![t] = "done"]
    /\ IF fcnt > 1
         THEN /\ fcnt' = fcnt - 1
              /\ UNCHANGED <<mode, wex, q, req, resumed>>
         ELSE LET w == Wake("N", 0, q, << >>, {}) IN
              /\ mode' = w.m /\ fcnt' = w.f /\ q' = w.q
              /\ resumed' = [r \in Req |-> resumed[r] \/ r \in w.g]
              /\ wex' = (wex /\ Blocked(w.q))
              /\ UNCHANGED req
    /\ UNCHANGED kind

\* pgbuf_timed_sleep (): woken with THREAD_PGBUF_RESUMED -- the latch is ours
Woken(r) ==
    /\ pc[r] = "sleep" /\ resumed[r]
    /\ pc' = [pc EXCEPT ![r] = "hold"]
    /\ UNCHANGED <<mode, fcnt, wex, q, kind, req, resumed>>

\* pgbuf_timed_sleep (): the wait ends by timeout or interrupt before a waker granted the latch;
\* request_latch_mode = PGBUF_NO_LATCH (one step with the resume check -- see README)
GiveUpStart(r) ==
    /\ r \in CanTimeout
    /\ pc[r] = "sleep" /\ ~resumed[r]
    /\ req' = [req EXCEPT ![r] = "NO"]
    /\ pc' = [pc EXCEPT ![r] = "giveup"]
    /\ UNCHANGED <<mode, fcnt, wex, q, kind, resumed>>

\* pgbuf_timed_sleep_error_handling () under the BCB mutex: case 1 (already unlinked by a waker),
\* case 2 (not at the front: unlink), case 3 (at the front: unlink and hand READ on), then the exit
GiveUp(r) ==
    /\ pc[r] = "giveup"
    /\ LET first == Len(q) > 0 /\ Head(q) = r
           s == IF first THEN Succ(mode, fcnt, Tail(q), {})
                ELSE [f |-> fcnt, q |-> Remove(q, r), g |-> {}]
       IN /\ fcnt' = s.f
          /\ q' = s.q
          /\ resumed' = [x \in Req |-> resumed[x] \/ x \in s.g]
          /\ wex' = CASE FIX = "develop" -> wex
                      [] FIX = "pr" -> wex /\ Blocked(s.q)
                      [] FIX = "always" -> FALSE
    /\ pc' = [pc EXCEPT ![r] = "done"]
    /\ UNCHANGED <<mode, kind, req>>

Done ==
    /\ \A t \in Thr : pc[t] = "done"
    /\ UNCHANGED vars

Next ==
    \/ \E r \in Req : Fix(r) \/ Woken(r) \/ GiveUpStart(r) \/ GiveUp(r)
    \/ \E t \in Thr : Unfix(t)
    \/ Done

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
TypeOK ==
    /\ mode \in {"N", "R", "W"}
    /\ fcnt \in 0 .. Cardinality(Thr)
    /\ wex \in BOOLEAN
    /\ q \in Seq(Req)
    /\ kind \in [Req -> {"R", "W"}]
    /\ req \in [Req -> {"R", "W", "NO"}]
    /\ resumed \in [Req -> BOOLEAN]
    /\ pc \in [Thr -> {"idle", "hold", "sleep", "giveup", "done"}]

\* no give-up between its request_latch_mode write and its BCB mutex section
Quiet == \A r \in Req : pc[r] # "giveup"

\* the PR's relation (rule #028 form), one way: the flag is up only for a blocked reader or writer
FlagSound == Quiet => (wex => Blocked(q))

\* the other way: a blocked reader or writer always has the flag, or a READ fix would pass a queued
\* writer and the last unfix would skip the wakeup
FlagComplete == Blocked(q) => wex

\* what a session sees (CBRD-28163): a READ request on a page that only READ latches hold waits only
\* behind a queued WRITE request
NoNeedlessWait ==
    Quiet => \A r \in Req :
        (pc[r] = "sleep" /\ ~resumed[r] /\ req[r] = "R" /\ mode = "R")
            => \E i, j \in 1 .. Len(q) : i < j /\ q[j] = r /\ req[q[i]] = "W"

\* pgbuf_latch_bcb_upon_fix ()'s safety net for an idle BCB with the flag never fires
IdleClean == mode = "N" => (~wex /\ fcnt = 0 /\ q = << >>)
=============================================================================
