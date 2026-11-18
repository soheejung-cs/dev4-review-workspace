-------------------------- MODULE SetDomainDecide --------------------------
(***************************************************************************)
(* CBRD-28066: a collection domain the compilation leaves undecided, and  *)
(* the server's domain cache that hands one plan's collection domain to    *)
(* another.                                                                *)
(*                                                                         *)
(* Type checking gives a string expression over a bind a collation flag   *)
(* other than NORMAL: ENFORCE when pt_coerce_node_collation () wraps a     *)
(* MAYBE operand in a CAST, LEAVE for the default domain of a MAYBE        *)
(* (pt_upd_domain_info (), pt_wrap_with_cast_op ()).  XASL generation      *)
(* decides the domain of every scalar regu variable from its operands      *)
(* (pt_decide_function_collation () and its siblings, CBRD-27583) - the    *)
(* model takes those as decided - but builds the domain of a collection    *)
(* constructor (SET{}, an IN list, = ANY {}, a row value) from the parse   *)
(* tree data_type, whose element list type checking copied from the        *)
(* elements (pt_add_type_to_set ()).  A reader of a list column of a       *)
(* collection type (a derived table, a CTE) takes pt_reader_domain () of   *)
(* the parse tree domain and the producer's.                               *)
(*                                                                         *)
(* The server loads a plan by unpacking each domain (or_unpack_domain ()); *)
(* a collection domain is looked up in the domain cache                    *)
(* (tp_domain_find_set ()), which matches elements by type, precision and  *)
(* collation but not by collation flag, and caches the unpacked one when   *)
(* none matches.  The load check (stx_unpack_domain (), CBRD-27817)        *)
(* rejects the plan when an unpacked domain, or an element of it, is not   *)
(* NORMAL.  The cache lives as long as the server, across sessions.        *)
(*                                                                         *)
(* FixElem: XASL generation builds a collection constructor's domain from  *)
(* its operand regu variables' decided domains - Recursive: bottom-up, so  *)
(* a nested constructor's elements are decided too; else only the flags   *)
(* of the top elements.  FixReader: a reader of a collection column whose  *)
(* parse tree domain is undecided takes the producer's.  FixCache:          *)
(* tp_domain_find_set () matches the element collation flags too, as      *)
(* tp_domain_cache () does since CBRD-27016.  README.md maps each action   *)
(* to file:line.                                                           *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS
    FixElem,     \* decide a collection constructor's element domains at XASL generation
    Recursive,   \* ... bottom-up through nested constructors
    FixReader,   \* a reader of a collection column takes the producer's decided domain
    FixCache,    \* tp_domain_find_set () compares element collation flags
    Kinds,       \* element kinds the plans draw from, a subset of {"col", "enf", "leave"}
    Carriers,    \* plan shapes, a subset of {"ctor", "nested", "reader", "scalar"}
    MaxRuns      \* plans executed in one server's life (over all sessions)

(* Type checking's flag for an element kind: a column or a literal keeps   *)
(* its collation; NVL (?, 'x') and its siblings over a MAYBE bind carry    *)
(* the ENFORCE cast's flag; a default domain of a MAYBE carries LEAVE.     *)
TCFlag(k) == CASE k = "col" -> "N" [] k = "enf" -> "E" [] k = "leave" -> "L"

Str(f) == [t |-> "str", flag |-> f, elems |-> <<>>]
Coll(es) == [t |-> "coll", flag |-> "N", elems |-> es]

(* A domain is decided when it and every element are NORMAL             *)
(* (stx_domain_is_decided ()).  Depth is at most two.                    *)
DecidedScalar(d) == d.flag = "N"
Decided(d) == DecidedScalar(d) /\ \A i \in 1..Len(d.elems) :
                  DecidedScalar(d.elems[i]) /\ \A j \in 1..Len(d.elems[i].elems) : DecidedScalar(d.elems[i].elems[j])

(* The cache key: tp_domain_match () - everything but the flags.          *)
Shape(d) == [t |-> d.t, elems |-> [i \in 1..Len(d.elems) |->
               [t |-> d.elems[i].t, elems |-> [j \in 1..Len(d.elems[i].elems) |-> d.elems[i].elems[j].t]]]]

(* An operand regu variable's domain: a scalar is decided at XASL         *)
(* generation (pt_decide_* ()); a nested constructor is what its own      *)
(* constructor built.                                                     *)
RECURSIVE Build(_)
DecideTop(d) == [d EXCEPT !.elems = [i \in 1..Len(d.elems) |->
                   IF d.elems[i].t = "str" THEN Str("N") ELSE d.elems[i]]]
Build(tree) ==
    IF tree.t = "str" THEN tree
    ELSE LET parse == Coll([i \in 1..Len(tree.elems) |-> tree.elems[i]])   \* pt_add_type_to_set ()
         IN  IF ~FixElem THEN parse
             ELSE IF Recursive
                  THEN Coll([i \in 1..Len(tree.elems) |->
                          IF tree.elems[i].t = "str" THEN Str("N") ELSE Build(tree.elems[i])])
                  ELSE DecideTop(parse)

(* Plans: a shape and the kinds of its elements.                          *)
Plan(c, k1, k2) == [carrier |-> c, k1 |-> k1, k2 |-> k2]
Plans == {Plan(c, k1, k2) : c \in Carriers, k1 \in Kinds, k2 \in Kinds}

(* The parse tree of the collection a plan builds.                         *)
Tree(p) == CASE p.carrier = "ctor"   -> Coll(<<Str(TCFlag(p.k1)), Str(TCFlag(p.k2))>>)
             [] p.carrier = "nested" -> Coll(<<Coll(<<Str(TCFlag(p.k1)), Str("N")>>), Coll(<<Str(TCFlag(p.k2)), Str("N")>>)>>)
             [] p.carrier = "reader" -> Coll(<<Str(TCFlag(p.k1)), Str(TCFlag(p.k2))>>)
             [] p.carrier = "scalar" -> Str(TCFlag(p.k1))

(* pt_reader_domain (): a parse tree domain of the producer's type keeps  *)
(* its own, but a string in a collation the values give takes the         *)
(* producer's collation; FixReader extends it to collections.             *)
Reader(parse, producer) ==
    IF parse.t = "str" THEN (IF parse.flag # "N" THEN producer ELSE parse)
    ELSE IF FixReader /\ ~Decided(parse) THEN producer ELSE parse

(* The domains a plan packs into its stream.                              *)
Packed(p) ==
    CASE p.carrier = "scalar" -> <<Str("N")>>                       \* a scalar regu: decided
      [] p.carrier = "reader" -> <<Build(Tree(p)), Reader(Tree(p), Build(Tree(p)))>>
      [] OTHER                -> <<Build(Tree(p))>>

VARIABLES
    cache,      \* the server's cached collection domains
    runs,       \* the plans executed so far, in order: [plan, packed, unpacked, accepted]
    alone       \* for each plan executed, whether a fresh server accepts it

vars == <<cache, runs, alone>>

(* or_unpack_domain () -> tp_domain_find_set (): the first cached domain  *)
(* that matches, or the unpacked one, cached.                             *)
Match(c, d) == IF FixCache THEN c = d ELSE Shape(c) = Shape(d)
Lookup(cch, d) ==
    IF d.t = "str" THEN d                                           \* tp_domain_find_charbit () compares flags
    ELSE IF \E c \in cch : Match(c, d) THEN CHOOSE c \in cch : Match(c, d) ELSE d

UnpackAll(cch, ds) ==
    LET step[i \in 0..Len(ds)] ==
          IF i = 0 THEN [c |-> cch, out |-> <<>>]
          ELSE LET prev == step[i - 1]
                   u == Lookup(prev.c, ds[i])
               IN  [c |-> IF ds[i].t = "coll" THEN prev.c \cup {u} ELSE prev.c, out |-> Append(prev.out, u)]
    IN step[Len(ds)]

Init == /\ cache = {}
        /\ runs = <<>>
        /\ alone = <<>>

(* A session executes a plan: the client compiles it, the server loads it *)
(* (xcache_find_xasl_id_for_execute () -> stx_map_stream_to_xasl ()).    *)
Execute(p) ==
    /\ Len(runs) < MaxRuns
    /\ LET ds == Packed(p)
           r  == UnpackAll(cache, ds)
           r0 == UnpackAll({}, ds)
       IN  /\ cache' = r.c
           /\ runs' = Append(runs, [plan |-> p, packed |-> ds, unpacked |-> r.out,
                                    accepted |-> \A i \in 1..Len(r.out) : Decided(r.out[i])])
           /\ alone' = Append(alone, \A i \in 1..Len(r0.out) : Decided(r0.out[i]))

(* The server has run its plans: the model stops there.                  *)
Done == Len(runs) = MaxRuns /\ UNCHANGED vars

Next == (\E p \in Plans : Execute(p)) \/ Done
Spec == Init /\ [][Next]_vars

(* --- properties ------------------------------------------------------- *)
(* The issue: every plan loads (no ER_QPROC_DOMAIN_UNRESOLVED).           *)
NoUnresolved == \A i \in 1..Len(runs) : runs[i].accepted

(* The server reads the domains the client compiled.                      *)
CacheFaithful == \A i \in 1..Len(runs) : runs[i].unpacked = runs[i].packed

(* What a session gets does not depend on what other sessions ran        *)
(* before on the same server.                                             *)
HistoryIndependent == \A i \in 1..Len(runs) : runs[i].accepted = alone[i]

TypeOK == /\ Len(runs) = Len(alone)
          /\ \A i \in 1..Len(runs) : runs[i].plan \in Plans
=============================================================================
