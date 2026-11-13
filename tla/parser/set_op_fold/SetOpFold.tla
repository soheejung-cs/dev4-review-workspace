---------------------------- MODULE SetOpFold ----------------------------
(***************************************************************************)
(* CBRD-28019: folding a set operation with an empty operand.              *)
(*                                                                         *)
(* A typing pass (pt_semantic_type ()) walks the tree post-order: each     *)
(* operand SELECT may be folded into a value when its WHERE is false       *)
(* (pt_eval_type (), pt_to_false_subquery ()), then each set operation     *)
(* decides how to fold (pt_check_union_is_foldable ()) and folds           *)
(* (pt_fold_union ()): into a value (AS_NULL), into one operand (AS_ARG1,  *)
(* AS_ARG2), or not at all.  At compile time mq_translate () then checks   *)
(* the operands of every set operation left (mq_set_union_query ()), and   *)
(* the statement executes.                                                 *)
(*                                                                         *)
(* The pass is the compile of a statement, or the typing again at          *)
(* execution of a statement that cannot be prepared, with its bound values *)
(* (db_execute_and_keep_statement_local ()).  The tree is a top set        *)
(* operation T over operands L and C; with Nested, L is a set operation N  *)
(* over A and B, as SQL A op B op C parses.  Each leaf is a SELECT: its    *)
(* rows (a bag over {1, 2}), whether its WHERE is false in this pass       *)
(* (pt_false_where ()), whether it is parenthesized (PT_IS_SUBQUERY),      *)
(* whether it has LIMIT 2 over its rows in value order (a block that       *)
(* cannot take DISTINCT, pt_select_takes_distinct ()), and a WITH clause.  *)
(*                                                                         *)
(* The semantic check (pt_semantic_check_local ()) folds a nested set     *)
(* operation, and types a parenthesized operand, before typing marks the   *)
(* operands of a set operation (pt_eval_type_pre ()).  EarlyMark: the PR   *)
(* marks them before the semantic check (pt_mark_union_leaf_nodes ());     *)
(* without it the compile folds and converts them unmarked (the worst      *)
(* case: an operand already false then, as a literal FALSE).  ValueGuard:  *)
(* the PR keeps a set operation for its DISTINCT, or folds it into an      *)
(* empty operand, only when that operand is still a query.                 *)
(*                                                                         *)
(* The flat tree may also carry the clause of T (TClauseChoice): LIMIT 2  *)
(* without ORDER BY, or an ORDER BY by a position, by a name of the select *)
(* list, or by a name outside it.  Folding moves the clause to the operand *)
(* left, where pt_check_order_by () matches the keys to its select list:   *)
(* a key outside becomes a hidden column, or stays a name when CREATE VIEW *)
(* checks the query (View, do_not_replace_orderby); typing turns the LIMIT *)
(* into GROUPBY_NUM () in the HAVING of a block with GROUP BY, before its  *)
(* DISTINCT.  A view stores the folded query, and each query of the view   *)
(* compiles it again.  PosFix, OrderKeepFix and GroupLimitFix are the      *)
(* third commit of the PR (self-review round 3).                           *)
(*                                                                         *)
(* Derived is the fourth commit: no operand is marked; only the set       *)
(* operations of a statement typed again at execution are (its top chain), *)
(* to fold into an empty operand.  A UNION or EXCEPT without ALL folds into *)
(* a SELECT DISTINCT from a derived table of the operand left, which keeps  *)
(* that block as it is, with the clause of T above the DISTINCT; so does an *)
(* operand left with LIMIT under a clause of T, or with WITH beside the     *)
(* WITH of T.  Folded so, an ORDER BY key outside the select list keeps T,  *)
(* which refuses it, as does an operand with INTO beside two WITH clauses.  *)
(*                                                                         *)
(* FIX = FALSE is develop, FIX = TRUE the PR.  README.md maps each action  *)
(* to file:line.                                                           *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets, TLC

CONSTANTS
    Nested,        \* TRUE: T (N (A, B), C); FALSE: T (A, C)
    ComplexChoice, \* the values a leaf's "has LIMIT 2" may take, a subset of BOOLEAN
    WithChoice,    \* the values a leaf's WITH may take
    KeepAtExec,    \* the PR's first two plans: a marked set operation is never folded at execution
    IntoFix,       \* the PR folds into an operand with INTO as develop does, without DISTINCT
    EarlyMark,     \* the PR marks the operands before the semantic check folds (pt_mark_union_leaf_nodes ())
    ValueGuard,    \* the PR never keeps, or folds into, an empty operand already folded into a value
    TClauseChoice, \* the clauses T may have: "none", "limit" (LIMIT 2 without ORDER BY), "pos", "in", "out" (ORDER BY
                   \* a position, a name of the select list, a name outside it; no LIMIT) -- flat tree only
    ViewChoice,    \* the values "the statement is the query of CREATE VIEW" may take -- flat tree only
    GroupChoice,   \* the values a leaf's GROUP BY may take
    PosFix,        \* the PR puts DISTINCT only when the keys moved to the operand are positions (pt_orderby_is_positions ())
    OrderKeepFix,  \* the PR keeps a set operation only with an ORDER BY of its select list (pt_orderby_matches_select_list ())
    GroupLimitFix, \* an operand with GROUP BY takes no DISTINCT under the LIMIT of T without ORDER BY
                   \* (pt_folded_operand_takes_distinct ())
    Derived,       \* the fourth commit: SELECT DISTINCT from a derived table of the operand left (pt_fold_union_as_derived ())
    FIX

ASSUME Nested \in BOOLEAN /\ FIX \in BOOLEAN /\ KeepAtExec \in BOOLEAN /\ IntoFix \in BOOLEAN
ASSUME EarlyMark \in BOOLEAN /\ ValueGuard \in BOOLEAN
ASSUME ComplexChoice \subseteq BOOLEAN /\ ComplexChoice # {}
ASSUME WithChoice \subseteq BOOLEAN /\ WithChoice # {}
ASSUME TClauseChoice \subseteq {"none", "limit", "pos", "in", "out"} /\ TClauseChoice # {}
ASSUME ViewChoice \subseteq BOOLEAN /\ ViewChoice # {} /\ GroupChoice \subseteq BOOLEAN /\ GroupChoice # {}
ASSUME PosFix \in BOOLEAN /\ OrderKeepFix \in BOOLEAN /\ GroupLimitFix \in BOOLEAN
ASSUME Derived \in BOOLEAN /\ (Derived => FIX)
ASSUME Nested => (TClauseChoice = {"none"} /\ ViewChoice = {FALSE})

Vals == {1, 2}
Bag == [Vals -> 0 .. 4]
Empty == [v \in Vals |-> 0]
\* rows a leaf may hold: {}, {1}, {1, 1}, {1, 1, 2}
RowChoices == { Empty, [v \in Vals |-> IF v = 1 THEN 1 ELSE 0],
                [v \in Vals |-> IF v = 1 THEN 2 ELSE 0], [v \in Vals |-> IF v = 1 THEN 2 ELSE 1] }

Min(a, b) == IF a < b THEN a ELSE b
Dedup(c) == [v \in Vals |-> IF c[v] > 0 THEN 1 ELSE 0]
\* LIMIT 2 over the rows in value order
Lim2(c) == IF c[1] >= 2 THEN [v \in Vals |-> IF v = 1 THEN 2 ELSE 0]
           ELSE IF c[1] = 1 THEN [v \in Vals |-> IF v = 1 THEN 1 ELSE Min(c[2], 1)]
           ELSE [v \in Vals |-> IF v = 1 THEN 0 ELSE Min(c[2], 2)]

SetOp(op, all, a, b) ==
    CASE op = "UNION"     -> IF all THEN [v \in Vals |-> a[v] + b[v]] ELSE Dedup([v \in Vals |-> a[v] + b[v]])
      [] op = "EXCEPT"    -> IF all THEN [v \in Vals |-> IF a[v] > b[v] THEN a[v] - b[v] ELSE 0]
                                    ELSE [v \in Vals |-> IF a[v] > 0 /\ b[v] = 0 THEN 1 ELSE 0]
      [] op = "INTERSECT" -> IF all THEN [v \in Vals |-> Min(a[v], b[v])] ELSE Dedup([v \in Vals |-> Min(a[v], b[v])])

Ops == {"UNION", "EXCEPT", "INTERSECT"}
Leaves == IF Nested THEN {"A", "B", "C"} ELSE {"A", "C"}
SetIds == IF Nested THEN {"T", "N"} ELSE {"T"}
Ids == Leaves \cup SetIds
KeptAtExecIds == SetIds


VARIABLES
    exec,       \* TRUE: typing again at execution with bound values; FALSE: the compile
    node,       \* id -> record; a set operation folded into a value becomes [kind |-> "VALUE"]
    top,        \* the id of the statement's top node
    expected,   \* the rows of the statement as SQL means them
    pc,         \* "mark" | "convert" | "foldN" | "foldT" | "translate" | "execute" | "done"
    asserted,   \* mq_set_union_query () met an operand that is neither a SELECT nor a set operation
    outcome,    \* "ROWS" | "ERROR" (exec: do_statement () unknown statement) | "KIND" (compile: a value statement)
    result,     \* the rows returned when outcome = "ROWS"
    scanned,    \* leaves with a false WHERE that the executor runs (reads their table)
    dev         \* what develop does with T, fixed at Init: [clause, folds (into an operand), rows (it returns),
                \* error (its derived table could not see an ORDER BY key outside the select list: Internal error)]

vars == << exec, node, top, expected, pc, asserted, outcome, result, scanned, dev >>

\* the rows of a node as the original SQL means them: a leaf is its own block
RECURSIVE Sem(_, _)
Sem(n, id) == IF n[id].kind = "LEAF"
              THEN (IF n[id].complex THEN Lim2(n[id].rows) ELSE n[id].rows)
              ELSE LET r == SetOp(n[id].op, n[id].all, Sem(n, n[id].a1), Sem(n, n[id].a2))
                   IN IF n[id].clause = "limit" THEN Lim2(r) ELSE r

\* the rows the executor returns for the tree as folded
\* a leaf that took the LIMIT of T: over its own LIMIT the fold wraps it in a derived table; otherwise typing makes
\* the LIMIT GROUPBY_NUM () in the HAVING of a GROUP BY block, which cuts the groups before DISTINCT, or ORDERBY_NUM ()
\* of a DISTINCT block, after it (pt_eval_type_pre ())
LeafRun(l) ==
    IF l.derived
    THEN LET own == IF l.complex THEN Lim2(l.rows) ELSE l.rows
             d == IF l.ddistinct THEN Dedup(own) ELSE own
         IN IF l.tclause = "limit" THEN Lim2(d) ELSE d
    ELSE IF l.tclause = "limit" /\ ~l.complex
    THEN (IF l.distinct THEN (IF l.group THEN Dedup(Lim2(l.rows)) ELSE Lim2(Dedup(l.rows))) ELSE Lim2(l.rows))
    ELSE LET b == IF l.distinct
                  THEN (IF l.complex THEN Lim2(Dedup(l.rows)) ELSE Dedup(l.rows))
                  ELSE (IF l.complex THEN Lim2(l.rows) ELSE l.rows)
         IN IF l.tclause = "limit" THEN Lim2(b) ELSE b

RECURSIVE Run(_, _)
Run(n, id) ==
    CASE n[id].kind = "VALUE" -> Empty
      [] n[id].kind = "LEAF" -> LeafRun(n[id])
      [] OTHER -> LET r0 == SetOp(n[id].op, n[id].all, Run(n, n[id].a1), Run(n, n[id].a2))
                      r == IF n[id].derived /\ n[id].ddistinct THEN Dedup(r0) ELSE r0
                  IN IF n[id].clause = "limit" THEN Lim2(r) ELSE r

\* the leaves the executor runs, with a false WHERE, below a set operation kept in the final tree
RECURSIVE Reach(_, _)
Reach(n, id) == CASE n[id].kind = "LEAF" -> {id}
                  [] n[id].kind = "VALUE" -> {}
                  [] OTHER -> Reach(n, n[id].a1) \cup Reach(n, n[id].a2)

Init ==
    /\ exec \in BOOLEAN
    /\ \E rows \in [Leaves -> RowChoices], fl \in [Leaves -> BOOLEAN], par \in [Leaves -> BOOLEAN],
          cx \in [Leaves -> ComplexChoice], wi \in [Leaves -> WithChoice],
          \* INTO is written on the first SELECT; only the flat tree gives A one
          into \in (IF Nested THEN {FALSE} ELSE BOOLEAN),
          opT \in Ops, allT \in BOOLEAN, withT \in BOOLEAN,
          opN \in (IF Nested THEN Ops ELSE {"UNION"}), allN \in (IF Nested THEN BOOLEAN ELSE {TRUE}),
          tcl \in TClauseChoice, view \in ViewChoice, gb \in [Leaves -> GroupChoice] :
          /\ \A x \in Leaves : /\ (fl[x] => rows[x] = Empty)
                               \* at execution the operands of the top chain are PT_IS_UNION_QUERY (mq_translate ())
                               /\ (exec => ~par[x])
          \* CREATE VIEW and a set operation refused for its ORDER BY are compiled, not executed with bound values
          /\ ((view \/ tcl = "out") => ~exec)
          /\ node = [x \in Ids |->
                      IF x \in Leaves
                      THEN [kind |-> "LEAF", rows |-> rows[x], false |-> fl[x], paren |-> par[x], complex |-> cx[x],
                            with |-> wi[x], mark |-> FALSE, distinct |-> FALSE, into |-> (x = "A" /\ into),
                            group |-> gb[x], tclause |-> "none", derived |-> FALSE, ddistinct |-> FALSE]
                      ELSE IF x = "T"
                      THEN [kind |-> "SETOP", op |-> opT, all |-> allT, with |-> withT, mark |-> FALSE,
                            a1 |-> IF Nested THEN "N" ELSE "A", a2 |-> "C", kept |-> "", clause |-> tcl, view |-> view,
                            derived |-> FALSE, ddistinct |-> FALSE]
                      ELSE [kind |-> "SETOP", op |-> opN, all |-> allN, with |-> FALSE, mark |-> FALSE,
                            a1 |-> "A", a2 |-> "B", kept |-> "", clause |-> "none", view |-> view,
                            derived |-> FALSE, ddistinct |-> FALSE]]
          \* develop folds the flat T into the operand that is not empty, unless both have a WITH clause; it returns the
          \* rows of that operand in its own block (no DISTINCT), with the clause of T (an ORDER BY keeps the rows)
          /\ LET act == IF fl["A"] THEN "C" ELSE "A"
                 own == IF cx[act] THEN Lim2(rows[act]) ELSE rows[act]
                 folds == ~Nested /\ fl["A"] # fl["C"] /\ (opT = "UNION" \/ (opT = "EXCEPT" /\ fl["C"]))
                          /\ ~(wi[act] /\ withT)
             IN dev = [clause |-> tcl,
                       folds |-> folds,
                       rows |-> IF tcl = "limit" THEN Lim2(own) ELSE own,
                       error |-> folds /\ cx[act] /\ tcl = "out"]
    /\ top = "T"
    /\ expected = Sem(node, "T")
    /\ pc = "mark"
    /\ asserted = FALSE /\ outcome = "" /\ result = Empty /\ scanned = {}

\* the PR: db_execute_and_keep_statement_local () marks the copied statement; the operands are marked before the
\* semantic check folds (EarlyMark: pt_mark_union_leaf_nodes ()), or by typing (pt_eval_type_pre ()) after the compile's
\* semantic check has folded and converted them; at execution the copy carries the marks of the compile
Mark ==
    /\ pc = "mark"
    /\ node' = [x \in Ids |->
                  IF ~FIX THEN node[x]
                  \* Derived: db_mark_set_operations_stay_queries () marks the set operations of the statement at execution
                  ELSE IF Derived THEN (IF x \in SetIds THEN [node[x] EXCEPT !.mark = exec] ELSE node[x])
                  ELSE IF x = "T" THEN [node[x] EXCEPT !.mark = exec]
                  ELSE [node[x] EXCEPT !.mark = EarlyMark \/ exec]]
    /\ pc' = "convert"
    /\ UNCHANGED << exec, top, expected, asserted, outcome, result, scanned, dev >>

\* pt_eval_type () PT_SELECT: a false subquery is folded into a value (pt_to_false_subquery ())
Convert ==
    /\ pc = "convert"
    /\ node' = [x \in Ids |->
                  IF x \in Leaves /\ node[x].false /\ node[x].paren /\ ~(FIX /\ node[x].mark)
                  THEN [kind |-> "VALUE"] ELSE node[x]]
    /\ pc' = IF Nested THEN "foldN" ELSE "foldT"
    /\ UNCHANGED << exec, top, expected, asserted, outcome, result, scanned, dev >>

IsNull(n, id) == CASE n[id].kind = "VALUE" -> TRUE
                   [] n[id].kind = "LEAF" -> n[id].false
                   [] OTHER -> FALSE
IsQuery(n, id) == n[id].kind # "VALUE"
\* a SELECT DISTINCT from a derived table counts (Derived): the set operation above folds to it as it is
Distinct(n, id) == IF n[id].kind = "LEAF" THEN n[id].distinct \/ n[id].ddistinct ELSE ~n[id].all \/ n[id].ddistinct
TakesDistinct(n, id) == n[id].kind = "LEAF" /\ ~n[id].complex
\* ... with the LIMIT of s moved to it: GROUPBY_NUM () would number its groups before DISTINCT
FoldedTakesDistinct(n, s, id) == TakesDistinct(n, id) /\ ~(GroupLimitFix /\ n[s].clause = "limit" /\ n[id].group)
HasInto(n, id) == n[id].kind = "LEAF" /\ n[id].into
\* Derived: clauses of the active node that those of s cannot join in one block (pt_fold_union_needs_derived ())
NeedsDerived(n, s, act) == /\ IsQuery(n, act)
                           /\ \/ (n[act].with /\ n[s].with)
                              \/ (n[act].kind = "LEAF" /\ n[act].complex /\ n[s].clause # "none")

\* pt_check_union_is_foldable ()
Decide(n, s) ==
    LET a1n == IsNull(n, n[s].a1)
        a2n == IsNull(n, n[s].a2)
        base == IF ~a1n /\ ~a2n THEN "NOTHING"
                ELSE IF a1n /\ a2n THEN "NULL"
                ELSE IF a1n THEN (IF n[s].op = "UNION" THEN "ARG2" ELSE "NULL")
                ELSE (IF n[s].op # "INTERSECT" THEN "ARG1" ELSE "NULL")
        guardOk(id) == IF Derived THEN IsQuery(n, id) ELSE ~ValueGuard \/ IsQuery(n, id)
        b2 == IF FIX /\ base = "NULL" /\ n[s].mark
              THEN (IF a1n /\ guardOk(n[s].a1) THEN "ARG1" ELSE IF a2n /\ guardOk(n[s].a2) THEN "ARG2" ELSE "NULL")
              ELSE base
        \* KeepAtExec: the statement's own set operations stay unfolded at execution (marked by the top's mark only)
        keepAtExec == FIX /\ KeepAtExec /\ exec /\ s \in KeptAtExecIds
        act == IF b2 = "ARG1" THEN n[s].a1 ELSE n[s].a2
        emp == IF b2 = "ARG1" THEN n[s].a2 ELSE n[s].a1
    IN IF keepAtExec /\ base # "NOTHING" THEN "NOTHING:keep"
       ELSE IF b2 \in {"ARG1", "ARG2"} /\ Derived
       THEN (IF NeedsDerived(n, s, act) /\ (n[s].clause = "out" \/ (n[act].with /\ n[s].with /\ HasInto(n, act)))
             THEN "NOTHING:derived" ELSE b2)
       ELSE IF b2 \in {"ARG1", "ARG2"}
       THEN (IF IsQuery(n, act) /\ n[act].with /\ n[s].with THEN "NOTHING:with"
             ELSE IF FIX /\ ~n[s].all /\ IsQuery(n, act) /\ ~Distinct(n, act) /\ ~IsNull(n, act)
                     /\ ~(IntoFix /\ HasInto(n, act)) /\ ~FoldedTakesDistinct(n, s, act) /\ guardOk(emp)
                     \* an empty operand with INTO (FOR UPDATE alike) folds as before too (pt_folds_without_distinct ())
                     /\ ~(FIX /\ ValueGuard /\ HasInto(n, emp))
                     \* a set operation refuses an ORDER BY key outside its select list: fold as before
                     /\ ~(OrderKeepFix /\ n[s].clause = "out") THEN "NOTHING:distinct"
             ELSE b2)
       ELSE b2

\* pt_fold_union (); the parent's pointer (or top) moves to the active node
Fold(n, s, parentOf) ==
    LET d == Decide(n, s)
        act == IF d = "ARG1" THEN n[s].a1 ELSE n[s].a2
        keep == FIX /\ ~n[s].all /\ ~Distinct(n, act) /\ ~IsNull(n, act) /\ ~(IntoFix /\ HasInto(n, act))
                /\ (~ValueGuard \/ FoldedTakesDistinct(n, s, act))
        \* the ORDER BY moved to the operand, matched by pt_check_order_by (): a key outside the select list becomes a
        \* hidden column, which keeps DISTINCT off; CREATE VIEW leaves it a name, which only PosFix sees
        orderOk == n[s].clause # "out" \/ (n[s].view /\ ~PosFix)
        \* Derived: DISTINCT on a SELECT from a derived table of the operand left, the clause of s above it
        keepD == ~n[s].all /\ IsQuery(n, act) /\ ~Distinct(n, act) /\ ~IsNull(n, act) /\ ~HasInto(n, act)
                 /\ n[s].clause # "out"
        asDerived == IsQuery(n, act) /\ (keepD \/ NeedsDerived(n, s, act))
    IN IF d = "NULL" THEN << [n EXCEPT ![s] = [kind |-> "VALUE"]], s >>
       ELSE IF d \in {"ARG1", "ARG2"} /\ Derived
       THEN << [x \in Ids |->
                  IF x = act /\ n[x].kind = "LEAF"
                  THEN [n[x] EXCEPT !.derived = n[x].derived \/ asDerived,
                                    !.ddistinct = n[x].ddistinct \/ (asDerived /\ keepD),
                                    !.tclause = n[s].clause, !.mark = n[s].mark]
                  ELSE IF x = act /\ n[x].kind = "SETOP"
                  THEN [n[x] EXCEPT !.derived = n[x].derived \/ asDerived,
                                    !.ddistinct = n[x].ddistinct \/ (asDerived /\ keepD),
                                    !.clause = n[s].clause, !.mark = n[s].mark]
                  ELSE IF x = parentOf THEN [n[x] EXCEPT !.a1 = IF n[x].a1 = s THEN act ELSE n[x].a1,
                                                         !.a2 = IF n[x].a2 = s THEN act ELSE n[x].a2]
                  ELSE n[x]], act >>
       ELSE IF d \in {"ARG1", "ARG2"}
       THEN << [x \in Ids |->
                  IF x = act /\ n[x].kind = "LEAF"
                  THEN [n[x] EXCEPT !.distinct = (n[x].distinct \/ (keep /\ orderOk)),
                                    !.tclause = n[s].clause,
                                    !.mark = IF FIX THEN n[s].mark ELSE n[x].mark]
                  ELSE IF x = act /\ n[x].kind = "SETOP"
                  THEN [n[x] EXCEPT !.clause = n[s].clause, !.mark = IF FIX THEN n[s].mark ELSE n[x].mark]
                  ELSE IF x = parentOf THEN [n[x] EXCEPT !.a1 = IF n[x].a1 = s THEN act ELSE n[x].a1,
                                                         !.a2 = IF n[x].a2 = s THEN act ELSE n[x].a2]
                  ELSE n[x]], act >>
       ELSE << [n EXCEPT ![s].kept = IF d = "NOTHING" THEN "nofold" ELSE d], s >>

FoldN ==
    /\ pc = "foldN"
    /\ LET r == Fold(node, "N", "T") IN node' = r[1]
    /\ pc' = "foldT"
    /\ UNCHANGED << exec, top, expected, asserted, outcome, result, scanned, dev >>

FoldT ==
    /\ pc = "foldT"
    /\ LET r == Fold(node, "T", "none") IN /\ node' = r[1] /\ top' = r[2]
    /\ pc' = "translate"
    /\ UNCHANGED << exec, expected, asserted, outcome, result, scanned, dev >>

\* the set operations left in the tree from the top
RECURSIVE SetsIn(_, _)
SetsIn(n, id) == IF n[id].kind = "SETOP" THEN {id} \cup SetsIn(n, n[id].a1) \cup SetsIn(n, n[id].a2) ELSE {}

\* mq_set_union_query (): at compile time, every operand left is a SELECT or a set operation
Translate ==
    /\ pc = "translate"
    \* a set operation left with an ORDER BY key outside its select list fails pt_check_order_by () before that
    /\ asserted' = (~exec /\ \E s \in SetsIn(node, top) : /\ node[s].clause # "out"
                                                          /\ (~IsQuery(node, node[s].a1) \/ ~IsQuery(node, node[s].a2)))
    /\ pc' = "execute"
    /\ UNCHANGED << exec, node, top, expected, outcome, result, scanned, dev >>

\* do_statement () / do_select_internal (): a value has no statement; a SELECT with a false WHERE on top is not run
Execute ==
    /\ pc = "execute"
    \* a set operation left with an ORDER BY key outside its select list is refused (pt_check_order_by ()); a SELECT
    \* DISTINCT left with such a key is refused too -- a view stores it, and every query of the view compiles it
    /\ outcome' = CASE node[top].kind = "VALUE" -> IF exec THEN "ERROR" ELSE "KIND"
                     [] node[top].kind = "SETOP" /\ node[top].clause = "out" -> "ORDER_ERROR"
                     [] node[top].kind = "LEAF" /\ node[top].tclause = "out" /\ node[top].distinct -> "DISTINCT_ORDER_ERROR"
                     [] OTHER -> "ROWS"
    /\ result' = IF node[top].kind = "VALUE" THEN Empty ELSE Run(node, top)
    /\ scanned' = IF node[top].kind = "LEAF" THEN {}
                  ELSE IF node[top].kind = "VALUE" THEN {}
                  ELSE {x \in Reach(node, top) : node[x].false}
    /\ pc' = "done"
    /\ UNCHANGED << exec, node, top, expected, asserted, dev >>

Next == Mark \/ Convert \/ FoldN \/ FoldT \/ Translate \/ Execute \/ (pc = "done" /\ UNCHANGED vars)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Properties, checked once the statement ran                              *)
(***************************************************************************)
\* the rows are those the SQL means -- a statement with INTO assigns one row instead (IntoFoldsLikeDevelop)
\* -- a set operation ordered by a column outside its select list is refused unless it folds (OutFoldsLikeDevelop)
AInto == node["A"].kind = "LEAF" /\ node["A"].into
ResultMatches == (pc = "done" /\ outcome = "ROWS" /\ ~AInto /\ dev.clause # "out") => result = expected
\* a SELECT with INTO that develop makes the statement (and so assigns) stays the statement: a kept set operation
\* does not assign the INTO of its first operand (do_select_internal () reads the statement's into_list)
DevelopFoldsToA == /\ node["T"].kind = "SETOP" /\ node["C"].kind = "LEAF" /\ node["C"].false /\ ~node["A"].false
                   /\ node["T"].op \in {"UNION", "EXCEPT"} /\ ~(node["A"].with /\ node["T"].with)
\* -- unless develop's derived table failed on an ORDER BY key outside the select list (dev.error)
IntoFoldsLikeDevelop == (pc = "done" /\ ~Nested /\ AInto /\ DevelopFoldsToA /\ ~dev.error) => top = "A"
\* typing again at execution keeps the query the client prepared
ExecStaysQuery == (pc = "done" /\ exec) => outcome # "ERROR"
\* no set operation is left with an operand folded into a value (debug assert)
\* -- Derived keeps, as develop does, a set operation whose operand left has INTO beside two WITH clauses (accepted)
IntoWithKept == /\ node["A"].kind = "LEAF" /\ node["A"].into /\ node["A"].with
                /\ node["T"].kind = "SETOP" /\ node["T"].with
NoValueOperand == pc = "done" => (~asserted \/ (Derived /\ IntoWithKept))
\* a value statement only where develop makes one too: every operand empty at compile time (the known [3])
KindOnlyAllEmpty == (pc = "done" /\ outcome = "KIND") => (~exec /\ expected = Empty)
\* an empty operand is read only below a set operation kept for a reason the PR accepts
ScanOnlyWhenKept == pc = "done" =>
    \A x \in scanned : \E s \in SetsIn(node, top) : /\ x \in Reach(node, s)
                                                     /\ node[s].kept \in {"NOTHING:distinct", "NOTHING:with", "NOTHING:derived",
                                                                          "nofold"}
\* at execution, a statement whose operands are all empty reads no table -- unless the top and an operand both
\* have a WITH clause, which the PR keeps unfolded as develop does (accepted, README)
AllFalse == \A x \in Leaves : node[x].kind = "LEAF" /\ node[x].false
TWith == node["T"].kind = "SETOP" /\ node["T"].with
ExecEmptyReadsNothing == (pc = "done" /\ exec /\ AllFalse /\ ~TWith) => scanned = {}
\* the ORDER BY fails a statement only where develop does not fold it either (develop keeps the set operation)
\* -- or where develop's derived table failed on it (an operand with LIMIT under ORDER BY: Internal error)
NoNewOrderError == (pc = "done" /\ outcome \in {"ORDER_ERROR", "DISTINCT_ORDER_ERROR"}) => (~dev.folds \/ dev.error)
\* a set operation ordered by a column outside its select list, which develop folds into an operand, folds the same
\* way: the rows of that operand without DISTINCT
OutFoldsLikeDevelop == (pc = "done" /\ dev.clause = "out" /\ dev.folds /\ ~dev.error) => (outcome = "ROWS" /\ result = dev.rows)
=============================================================================
