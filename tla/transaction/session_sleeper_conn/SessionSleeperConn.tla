------------------------- MODULE SessionSleeperConn -------------------------
(***************************************************************************)
(* CBRD-27960, PR #8427: a thread T that interrupts transaction X reads    *)
(* the conn_entry of other worker threads.  No lock that T holds guards    *)
(* that field: a worker sets it when it takes a request and clears it      *)
(* when it ends one, and a connection that closed goes back to the free    *)
(* list, where the next client takes it.                                   *)
(*                                                                         *)
(*  site B  logtb_set_tran_index_interrupt (): W := the worker whose       *)
(*          tran_index is X; wake W; W's conn_entry->session_id goes to    *)
(*          logtb_interrupt_session_sleepers ()                            *)
(*  site A  logtb_interrupt_session_sleeper_mapfunc () on every worker Z,  *)
(*          under Z's tran_index_lock: Z's conn_entry NULL check, then     *)
(*          ->session_id                                                   *)
(*                                                                         *)
(* VARIANT  "develop"   both sites read conn_entry twice (NULL check and   *)
(*                      dereference); site B reads it after the wake.      *)
(*          "pr"        both sites read it once; site B reads the pointer  *)
(*                      before the wake and session_id after it.           *)
(*          "proposal"  "pr", and site B also reads session_id before the  *)
(*                      wake (review thread r4489520248).                  *)
(*          "validate"  "proposal", and site B drops that session_id when  *)
(*                      the connection's client_id is no longer the one    *)
(*                      tdes->client_id of X had (a sketch, not in code).  *)
(* Environment switches: DISCONNECT (X's client disconnects, so c1 can be  *)
(* freed and reused), CWWAKES (the close path wakes W itself - TRUE is the *)
(* code), REUSE (W may take a request of another session after ending      *)
(* X's), WSTART (W suspended in SLEEP() or running).                       *)
(* README.md maps every action to file:line.                               *)
(***************************************************************************)
EXTENDS Naturals, Sequences

CONSTANTS
    VARIANT,     \* "develop" | "pr" | "proposal" | "validate"
    DISCONNECT,  \* TRUE: X's client disconnects while T interrupts X
    CWWAKES,     \* TRUE: the close path wakes W in SLEEP() (the code); FALSE: counterfactual
    REUSE,       \* TRUE: W may take the next request of session S3 once it ends X's
    WSTART       \* subset of {"sleep", "run"}: W waits in SLEEP() for X, or runs X's request

ASSUME VARIANT \in {"develop", "pr", "proposal", "validate"}
ASSUME DISCONNECT \in BOOLEAN /\ CWWAKES \in BOOLEAN /\ REUSE \in BOOLEAN
ASSUME WSTART \subseteq {"sleep", "run"} /\ WSTART # {}

NULL  == "null"
NONE  == "none"
EMPTY == 0                       \* DB_EMPTY_SESSION
S1 == 1                          \* the session of X
S2 == 2                          \* the session of a new client
S3 == 3                          \* another session: V serves it, W may take its next request
Sessions == {EMPTY, S1, S2, S3}
Conns == {"c1", "c2", "c3"}      \* c1: X's client, c2: free at the start, c3: S3's client
XClient == 1                     \* client_id of c1 while it serves X = tdes->client_id of X

VARIABLES
    wpc, wConn, wTran,           \* W: the worker that serves X
    vpc, vConn,                  \* V: a worker of S3 that keeps taking and ending requests
    connSess, connClient, freeList,
    cwpc,                        \* the connection worker that closes c1
    ncpc, ncConn,                \* a new client
    tpc, tconn, tsid, msid, victims, mlock, mconn   \* T (mconn: the PR mapfunc's one read)

wvars  == << wpc, wConn, wTran >>
vvars  == << vpc, vConn >>
cvars  == << connSess, connClient, freeList >>
ncvars == << ncpc, ncConn >>
tvars  == << tpc, tconn, tsid, msid, victims, mlock, mconn >>
vars   == << wvars, vvars, cvars, cwpc, ncvars, tvars >>

Init ==
    /\ wpc \in WSTART
    /\ wConn = "c1"
    /\ wTran = "X"
    /\ vpc = "vrun"
    /\ vConn = "c3"
    /\ connSess = [c \in Conns |-> IF c = "c1" THEN S1 ELSE IF c = "c3" THEN S3 ELSE EMPTY]
    /\ connClient = [c \in Conns |-> IF c = "c1" THEN 1 ELSE IF c = "c3" THEN 3 ELSE 0]
    /\ freeList = << "c2" >>
    /\ cwpc = "open"
    /\ ncpc = "none"
    /\ ncConn = NULL
    /\ tpc = "find"
    /\ tconn = NULL
    /\ tsid = EMPTY
    /\ msid = EMPTY
    /\ victims = {}
    /\ mlock = NONE
    /\ mconn = NULL

(* ---------------- W: ends X's request, may take one of S3 ---------------- *)

(* css_internal_request_handler (): css_set_thread_info () gives the thread back its old tran_index *)
WEnd ==
    /\ wpc = "run"
    /\ wpc' = "ended"
    /\ wTran' = NONE
    /\ UNCHANGED << wConn, vvars, cvars, cwpc, ncvars, tvars >>

(* css_server_task::execute (): conn_entry = NULL, with no lock held *)
WNull ==
    /\ wpc = "ended"
    /\ wpc' = "nulled"
    /\ wConn' = NULL
    /\ UNCHANGED << wTran, vvars, cvars, cwpc, ncvars, tvars >>

(* TS_FREE, then end_working_task (): c1 no longer has W's task *)
WFreed ==
    /\ wpc = "nulled"
    /\ wpc' = "idle"
    /\ UNCHANGED << wConn, wTran, vvars, cvars, cwpc, ncvars, tvars >>

(* the next request, of S3: conn_entry = &m_conn before the lock *)
WTake ==
    /\ REUSE
    /\ wpc = "idle"
    /\ wpc' = "conn3"
    /\ wConn' = "c3"
    /\ UNCHANGED << wTran, vvars, cvars, cwpc, ncvars, tvars >>

(* TS_RUN, then tran_index set under tran_index_lock *)
WRun3 ==
    /\ wpc = "conn3"
    /\ mlock # "W"
    /\ wpc' = "run3"
    /\ wTran' = "X3"
    /\ UNCHANGED << wConn, vvars, cvars, cwpc, ncvars, tvars >>

(* ---------------- V: a worker of S3, ends and takes requests ------------- *)

VNull ==
    /\ vpc = "vrun"
    /\ vpc' = "vnulled"
    /\ vConn' = NULL
    /\ UNCHANGED << wvars, cvars, cwpc, ncvars, tvars >>

VFreed ==
    /\ vpc = "vnulled"
    /\ vpc' = "vidle"
    /\ UNCHANGED << vConn, wvars, cvars, cwpc, ncvars, tvars >>

VTake ==
    /\ vpc = "vidle"
    /\ vpc' = "vconn"
    /\ vConn' = "c3"
    /\ UNCHANGED << wvars, cvars, cwpc, ncvars, tvars >>

VRun ==
    /\ vpc = "vconn"
    /\ mlock # "V"
    /\ vpc' = "vrun"
    /\ UNCHANGED << vConn, wvars, cvars, cwpc, ncvars, tvars >>

(* ---------------- the connection worker closes c1 ------------------------ *)

(* CONN_CLOSING, then net_server_wakeup_workers () interrupts X and wakes W in SLEEP() *)
CWClose ==
    /\ DISCONNECT
    /\ cwpc = "open"
    /\ cwpc' = "closing"
    /\ wpc' = IF CWWAKES /\ wpc = "sleep" THEN "run" ELSE wpc
    /\ UNCHANGED << wConn, wTran, vvars, cvars, ncvars, tvars >>

(* no worker of X and no task on c1 is left; css_free_conn (): DB_EMPTY_SESSION, head of the free list *)
CWFree ==
    /\ cwpc = "closing"
    /\ wpc \notin {"sleep", "run", "ended", "nulled"}
    /\ cwpc' = "freed"
    /\ connSess' = [connSess EXCEPT !["c1"] = EMPTY]
    /\ freeList' = << "c1" >> \o freeList
    /\ UNCHANGED << connClient, wvars, vvars, ncvars, tvars >>

(* ---------------- a new client ------------------------------------------ *)

(* css_make_conn () takes the head of the free list; css_initialize_conn () gives a new client_id *)
NCConnect ==
    /\ ncpc = "none"
    /\ freeList # << >>
    /\ ncpc' = "conn"
    /\ ncConn' = Head(freeList)
    /\ freeList' = Tail(freeList)
    /\ connClient' = [connClient EXCEPT ![Head(freeList)] = 2]
    /\ connSess' = [connSess EXCEPT ![Head(freeList)] = EMPTY]
    /\ UNCHANGED << wvars, vvars, cwpc, tvars >>

(* session_set_conn_entry_data () *)
NCSession ==
    /\ ncpc = "conn"
    /\ ncpc' = "sess"
    /\ connSess' = [connSess EXCEPT ![ncConn] = S2]
    /\ UNCHANGED << ncConn, connClient, freeList, wvars, vvars, cwpc, tvars >>

(* ---------------- T, site B --------------------------------------------- *)

(* logtb_find_thread_by_tran_index_except_me (X): reads tran_index with no lock *)
TFind ==
    /\ tpc = "find"
    /\ tpc' = IF wTran = "X" THEN (IF VARIANT = "develop" THEN "wake" ELSE "rconn") ELSE "done"
    /\ UNCHANGED << tconn, tsid, msid, victims, mlock, mconn, wvars, vvars, cvars, cwpc, ncvars >>

(* PR: tran_conn = tran_worker->conn_entry *)
TReadConn ==
    /\ tpc = "rconn"
    /\ tconn' = wConn
    /\ tpc' = IF VARIANT = "pr" \/ wConn = NULL THEN "wake" ELSE "rsid"
    /\ UNCHANGED << tsid, msid, victims, mlock, mconn, wvars, vvars, cvars, cwpc, ncvars >>

(* PR: tran_conn->session_id after the wake; proposal and validate: before it *)
TReadSid ==
    /\ tpc = "rsid"
    /\ tsid' = connSess[tconn]
    /\ tpc' = CASE VARIANT = "pr" -> "map"
                [] VARIANT = "proposal" -> "wake"
                [] OTHER -> "valid"
    /\ UNCHANGED << tconn, msid, victims, mlock, mconn, wvars, vvars, cvars, cwpc, ncvars >>

(* validate (sketch): tran_conn->client_id != tdes->client_id -> DB_EMPTY_SESSION *)
TValidate ==
    /\ tpc = "valid"
    /\ tsid' = IF connClient[tconn] = XClient THEN tsid ELSE EMPTY
    /\ tpc' = "wake"
    /\ UNCHANGED << tconn, msid, victims, mlock, mconn, wvars, vvars, cvars, cwpc, ncvars >>

(* thread_check_suspend_reason_and_wakeup (): signals W if it waits in SLEEP(); does not wait for W *)
TWake ==
    /\ tpc = "wake"
    /\ wpc' = IF wpc = "sleep" THEN "run" ELSE wpc
    /\ tpc' = CASE VARIANT = "develop" -> "dchk"
                [] VARIANT = "pr" -> (IF tconn = NULL THEN "done" ELSE "rsid")
                [] OTHER -> "map"
    /\ UNCHANGED << wConn, wTran, tconn, tsid, msid, victims, mlock, mconn, vvars, cvars, cwpc, ncvars >>

(* develop: tran_worker->conn_entry != NULL *)
TDevChk ==
    /\ tpc = "dchk"
    /\ tpc' = IF wConn = NULL THEN "done" ELSE "dderef"
    /\ UNCHANGED << tconn, tsid, msid, victims, mlock, mconn, wvars, vvars, cvars, cwpc, ncvars >>

(* develop: tran_worker->conn_entry->session_id reads the field again *)
TDevDeref ==
    /\ tpc = "dderef"
    /\ IF wConn = NULL
         THEN /\ tpc' = "crashB"
              /\ UNCHANGED tsid
         ELSE /\ tsid' = connSess[wConn]
              /\ tpc' = "map"
    /\ UNCHANGED << tconn, msid, victims, mlock, mconn, wvars, vvars, cvars, cwpc, ncvars >>

(* ---------------- T, site A: logtb_interrupt_session_sleepers () ------- *)

(* returns at once on DB_EMPTY_SESSION, else maps the mapfunc over the workers *)
TMap ==
    /\ tpc = "map"
    /\ msid' = tsid
    /\ IF tsid = EMPTY
         THEN /\ tpc' = "done"
              /\ UNCHANGED mlock
         ELSE /\ tpc' = "mW"
              /\ mlock' = "W"
    /\ UNCHANGED << tconn, tsid, victims, mconn, wvars, vvars, cvars, cwpc, ncvars >>

ToV   == tpc' = "mV" /\ mlock' = "V"
ToEnd == tpc' = "done" /\ mlock' = NONE

(* a match sets tdes->interrupt of the thread's transaction and wakes it *)
MatchW(c) ==
    IF c # NULL /\ connSess[c] = msid
      THEN /\ victims' = victims \cup {msid}
           /\ wpc' = IF wpc = "sleep" THEN "run" ELSE wpc
      ELSE UNCHANGED << victims, wpc >>

MatchV(c) ==
    IF c # NULL /\ connSess[c] = msid
      THEN victims' = victims \cup {msid}
      ELSE UNCHANGED victims

(* site A on W, under W's tran_index_lock (2847). develop: the status (2848-2850), then conn_entry twice (2851).
   The PR: conn_entry once (2852), then the status (2853-2855) and the compare on that value (2856). *)
TMapW ==
    /\ tpc = "mW"
    /\ IF VARIANT = "develop"
         THEN IF wpc \in {"idle", "conn3"}
                THEN /\ ToV
                     /\ UNCHANGED mconn
                ELSE /\ tpc' = "mWchk"
                     /\ UNCHANGED << mlock, mconn >>
         ELSE /\ mconn' = wConn
              /\ tpc' = "mWcmp"
              /\ UNCHANGED mlock
    /\ UNCHANGED << tconn, tsid, msid, victims, wvars, vvars, cvars, cwpc, ncvars >>

(* develop: thread_ref.conn_entry != NULL *)
TMapWChk ==
    /\ tpc = "mWchk"
    /\ IF wConn = NULL
         THEN ToV
         ELSE /\ tpc' = "mWderef"
              /\ UNCHANGED mlock
    /\ UNCHANGED << tconn, tsid, msid, victims, mconn, wvars, vvars, cvars, cwpc, ncvars >>

(* develop: thread_ref.conn_entry->session_id reads the field again *)
TMapWDeref ==
    /\ tpc = "mWderef"
    /\ IF wConn = NULL
         THEN /\ tpc' = "crashA"
              /\ UNCHANGED << mlock, victims, wpc >>
         ELSE /\ MatchW(wConn)
              /\ ToV
    /\ UNCHANGED << tconn, tsid, msid, mconn, wConn, wTran, vvars, cvars, cwpc, ncvars >>

(* PR: the status, then conn != NULL && conn->session_id == session_id on the value read *)
TMapWCmp ==
    /\ tpc = "mWcmp"
    /\ IF wpc \in {"idle", "conn3"}
         THEN UNCHANGED << victims, wpc >>
         ELSE MatchW(mconn)
    /\ ToV
    /\ UNCHANGED << tconn, tsid, msid, mconn, wConn, wTran, vvars, cvars, cwpc, ncvars >>

(* site A on V, as on W *)
TMapV ==
    /\ tpc = "mV"
    /\ IF VARIANT = "develop"
         THEN IF vpc \in {"vidle", "vconn"}
                THEN /\ ToEnd
                     /\ UNCHANGED mconn
                ELSE /\ tpc' = "mVchk"
                     /\ UNCHANGED << mlock, mconn >>
         ELSE /\ mconn' = vConn
              /\ tpc' = "mVcmp"
              /\ UNCHANGED mlock
    /\ UNCHANGED << tconn, tsid, msid, victims, wvars, vvars, cvars, cwpc, ncvars >>

TMapVChk ==
    /\ tpc = "mVchk"
    /\ IF vConn = NULL
         THEN ToEnd
         ELSE /\ tpc' = "mVderef"
              /\ UNCHANGED mlock
    /\ UNCHANGED << tconn, tsid, msid, victims, mconn, wvars, vvars, cvars, cwpc, ncvars >>

TMapVDeref ==
    /\ tpc = "mVderef"
    /\ IF vConn = NULL
         THEN /\ tpc' = "crashA"
              /\ UNCHANGED << mlock, victims >>
         ELSE /\ MatchV(vConn)
              /\ ToEnd
    /\ UNCHANGED << tconn, tsid, msid, mconn, wvars, vvars, cvars, cwpc, ncvars >>

TMapVCmp ==
    /\ tpc = "mVcmp"
    /\ IF vpc \in {"vidle", "vconn"}
         THEN UNCHANGED victims
         ELSE MatchV(mconn)
    /\ ToEnd
    /\ UNCHANGED << tconn, tsid, msid, mconn, wvars, vvars, cvars, cwpc, ncvars >>

(* T has finished, or the server crashed: the model stops there (not a deadlock) *)
TDone == tpc \in {"done", "crashA", "crashB"} /\ UNCHANGED vars

Next ==
    \/ WEnd \/ WNull \/ WFreed \/ WTake \/ WRun3
    \/ VNull \/ VFreed \/ VTake \/ VRun
    \/ CWClose \/ CWFree
    \/ NCConnect \/ NCSession
    \/ TFind \/ TReadConn \/ TReadSid \/ TValidate \/ TWake \/ TDevChk \/ TDevDeref
    \/ TMap \/ TMapW \/ TMapWChk \/ TMapWDeref \/ TMapWCmp
    \/ TMapV \/ TMapVChk \/ TMapVDeref \/ TMapVCmp
    \/ TDone

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
TypeOK ==
    /\ wpc \in {"sleep", "run", "ended", "nulled", "idle", "conn3", "run3"}
    /\ wConn \in Conns \cup {NULL}
    /\ wTran \in {"X", "X3", NONE}
    /\ vpc \in {"vrun", "vnulled", "vidle", "vconn"}
    /\ vConn \in {"c3", NULL}
    /\ connSess \in [Conns -> Sessions]
    /\ connClient \in [Conns -> 0 .. 3]
    /\ freeList \in Seq(Conns)
    /\ cwpc \in {"open", "closing", "freed"}
    /\ ncpc \in {"none", "conn", "sess"}
    /\ ncConn \in Conns \cup {NULL}
    /\ tpc \in {"find", "rconn", "rsid", "valid", "wake", "dchk", "dderef", "map",
                "mW", "mWchk", "mWderef", "mWcmp", "mV", "mVchk", "mVderef", "mVcmp",
                "done", "crashA", "crashB"}
    /\ tconn \in Conns \cup {NULL}
    /\ tsid \in Sessions
    /\ msid \in Sessions
    /\ victims \subseteq Sessions
    /\ mlock \in {NONE, "W", "V"}
    /\ mconn \in Conns \cup {NULL}

(* the server does not crash: no NULL is dereferenced at either site *)
NoNullDeref == tpc \notin {"crashA", "crashB"}
NoCrashA == tpc # "crashA"
NoCrashB == tpc # "crashB"

(* review thread r4489520248: the session id that site B hands to the mapper is X's session *)
SidIsTarget == msid \in {EMPTY, S1}

(* what a user sees: only workers of X's session get their transaction interrupted *)
OnlyTargetInterrupted == victims \subseteq {S1}

(* sanity, expected to be violated: site B still hands X's session to the mapper *)
NeverTargetsS1 == msid # S1
=============================================================================
