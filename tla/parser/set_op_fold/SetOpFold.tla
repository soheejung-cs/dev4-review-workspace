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
    FIX

ASSUME Nested \in BOOLEAN /\ FIX \in BOOLEAN /\ KeepAtExec \in BOOLEAN /\ IntoFix \in BOOLEAN
ASSUME EarlyMark \in BOOLEAN /\ ValueGuard \in BOOLEAN
ASSUME ComplexChoice \subseteq BOOLEAN /\ ComplexChoice # {}
ASSUME WithChoice \subseteq BOOLEAN /\ WithChoice # {}

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
    scanned     \* leaves with a false WHERE that the executor runs (reads their table)

vars == << exec, node, top, expected, pc, asserted, outcome, result, scanned >>

\* the rows of a node as the original SQL means them: a leaf is its own block
RECURSIVE Sem(_, _)
Sem(n, id) == IF n[id].kind = "LEAF"
              THEN (IF n[id].complex THEN Lim2(n[id].rows) ELSE n[id].rows)
              ELSE SetOp(n[id].op, n[id].all, Sem(n, n[id].a1), Sem(n, n[id].a2))

\* the rows the executor returns for the tree as folded
RECURSIVE Run(_, _)
Run(n, id) ==
    CASE n[id].kind = "VALUE" -> Empty
      [] n[id].kind = "LEAF" ->
           IF n[id].distinct
           THEN (IF n[id].complex THEN Lim2(Dedup(n[id].rows)) ELSE Dedup(n[id].rows))
           ELSE (IF n[id].complex THEN Lim2(n[id].rows) ELSE n[id].rows)
      [] OTHER -> SetOp(n[id].op, n[id].all, Run(n, n[id].a1), Run(n, n[id].a2))

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
          opN \in (IF Nested THEN Ops ELSE {"UNION"}), allN \in (IF Nested THEN BOOLEAN ELSE {TRUE}) :
          /\ \A x \in Leaves : /\ (fl[x] => rows[x] = Empty)
                               \* at execution the operands of the top chain are PT_IS_UNION_QUERY (mq_translate ())
                               /\ (exec => ~par[x])
          /\ node = [x \in Ids |->
                      IF x \in Leaves
                      THEN [kind |-> "LEAF", rows |-> rows[x], false |-> fl[x], paren |-> par[x], complex |-> cx[x],
                            with |-> wi[x], mark |-> FALSE, distinct |-> FALSE, into |-> (x = "A" /\ into)]
                      ELSE IF x = "T"
                      THEN [kind |-> "SETOP", op |-> opT, all |-> allT, with |-> withT, mark |-> FALSE,
                            a1 |-> IF Nested THEN "N" ELSE "A", a2 |-> "C", kept |-> ""]
                      ELSE [kind |-> "SETOP", op |-> opN, all |-> allN, with |-> FALSE, mark |-> FALSE,
                            a1 |-> "A", a2 |-> "B", kept |-> ""]]
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
                  ELSE IF x = "T" THEN [node[x] EXCEPT !.mark = exec]
                  ELSE [node[x] EXCEPT !.mark = EarlyMark \/ exec]]
    /\ pc' = "convert"
    /\ UNCHANGED << exec, top, expected, asserted, outcome, result, scanned >>

\* pt_eval_type () PT_SELECT: a false subquery is folded into a value (pt_to_false_subquery ())
Convert ==
    /\ pc = "convert"
    /\ node' = [x \in Ids |->
                  IF x \in Leaves /\ node[x].false /\ node[x].paren /\ ~(FIX /\ node[x].mark)
                  THEN [kind |-> "VALUE"] ELSE node[x]]
    /\ pc' = IF Nested THEN "foldN" ELSE "foldT"
    /\ UNCHANGED << exec, top, expected, asserted, outcome, result, scanned >>

IsNull(n, id) == CASE n[id].kind = "VALUE" -> TRUE
                   [] n[id].kind = "LEAF" -> n[id].false
                   [] OTHER -> FALSE
IsQuery(n, id) == n[id].kind # "VALUE"
Distinct(n, id) == IF n[id].kind = "LEAF" THEN n[id].distinct ELSE ~n[id].all
TakesDistinct(n, id) == n[id].kind = "LEAF" /\ ~n[id].complex
HasInto(n, id) == n[id].kind = "LEAF" /\ n[id].into

\* pt_check_union_is_foldable ()
Decide(n, s) ==
    LET a1n == IsNull(n, n[s].a1)
        a2n == IsNull(n, n[s].a2)
        base == IF ~a1n /\ ~a2n THEN "NOTHING"
                ELSE IF a1n /\ a2n THEN "NULL"
                ELSE IF a1n THEN (IF n[s].op = "UNION" THEN "ARG2" ELSE "NULL")
                ELSE (IF n[s].op # "INTERSECT" THEN "ARG1" ELSE "NULL")
        guardOk(id) == ~ValueGuard \/ IsQuery(n, id)
        b2 == IF FIX /\ base = "NULL" /\ n[s].mark
              THEN (IF a1n /\ guardOk(n[s].a1) THEN "ARG1" ELSE IF a2n /\ guardOk(n[s].a2) THEN "ARG2" ELSE "NULL")
              ELSE base
        \* KeepAtExec: the statement's own set operations stay unfolded at execution (marked by the top's mark only)
        keepAtExec == FIX /\ KeepAtExec /\ exec /\ s \in KeptAtExecIds
        act == IF b2 = "ARG1" THEN n[s].a1 ELSE n[s].a2
        emp == IF b2 = "ARG1" THEN n[s].a2 ELSE n[s].a1
    IN IF keepAtExec /\ base # "NOTHING" THEN "NOTHING:keep"
       ELSE IF b2 \in {"ARG1", "ARG2"}
       THEN (IF IsQuery(n, act) /\ n[act].with /\ n[s].with THEN "NOTHING:with"
             ELSE IF FIX /\ ~n[s].all /\ IsQuery(n, act) /\ ~Distinct(n, act) /\ ~IsNull(n, act)
                     /\ ~(IntoFix /\ HasInto(n, act)) /\ ~TakesDistinct(n, act) /\ guardOk(emp) THEN "NOTHING:distinct"
             ELSE b2)
       ELSE b2

\* pt_fold_union (); the parent's pointer (or top) moves to the active node
Fold(n, s, parentOf) ==
    LET d == Decide(n, s)
        act == IF d = "ARG1" THEN n[s].a1 ELSE n[s].a2
        keep == FIX /\ ~n[s].all /\ ~Distinct(n, act) /\ ~IsNull(n, act) /\ ~(IntoFix /\ HasInto(n, act))
                /\ (~ValueGuard \/ TakesDistinct(n, act))
    IN IF d = "NULL" THEN << [n EXCEPT ![s] = [kind |-> "VALUE"]], s >>
       ELSE IF d \in {"ARG1", "ARG2"}
       THEN << [x \in Ids |->
                  IF x = act /\ n[x].kind = "LEAF"
                  THEN [n[x] EXCEPT !.distinct = (n[x].distinct \/ keep),
                                    !.mark = IF FIX THEN n[s].mark ELSE n[x].mark]
                  ELSE IF x = act /\ n[x].kind = "SETOP"
                  THEN [n[x] EXCEPT !.mark = IF FIX THEN n[s].mark ELSE n[x].mark]
                  ELSE IF x = parentOf THEN [n[x] EXCEPT !.a1 = IF n[x].a1 = s THEN act ELSE n[x].a1,
                                                         !.a2 = IF n[x].a2 = s THEN act ELSE n[x].a2]
                  ELSE n[x]], act >>
       ELSE << [n EXCEPT ![s].kept = IF d = "NOTHING" THEN "nofold" ELSE d], s >>

FoldN ==
    /\ pc = "foldN"
    /\ LET r == Fold(node, "N", "T") IN node' = r[1]
    /\ pc' = "foldT"
    /\ UNCHANGED << exec, top, expected, asserted, outcome, result, scanned >>

FoldT ==
    /\ pc = "foldT"
    /\ LET r == Fold(node, "T", "none") IN /\ node' = r[1] /\ top' = r[2]
    /\ pc' = "translate"
    /\ UNCHANGED << exec, expected, asserted, outcome, result, scanned >>

\* the set operations left in the tree from the top
RECURSIVE SetsIn(_, _)
SetsIn(n, id) == IF n[id].kind = "SETOP" THEN {id} \cup SetsIn(n, n[id].a1) \cup SetsIn(n, n[id].a2) ELSE {}

\* mq_set_union_query (): at compile time, every operand left is a SELECT or a set operation
Translate ==
    /\ pc = "translate"
    /\ asserted' = (~exec /\ \E s \in SetsIn(node, top) : ~IsQuery(node, node[s].a1) \/ ~IsQuery(node, node[s].a2))
    /\ pc' = "execute"
    /\ UNCHANGED << exec, node, top, expected, outcome, result, scanned >>

\* do_statement () / do_select_internal (): a value has no statement; a SELECT with a false WHERE on top is not run
Execute ==
    /\ pc = "execute"
    /\ outcome' = IF node[top].kind = "VALUE" THEN (IF exec THEN "ERROR" ELSE "KIND") ELSE "ROWS"
    /\ result' = IF node[top].kind = "VALUE" THEN Empty ELSE Run(node, top)
    /\ scanned' = IF node[top].kind = "LEAF" THEN {}
                  ELSE IF node[top].kind = "VALUE" THEN {}
                  ELSE {x \in Reach(node, top) : node[x].false}
    /\ pc' = "done"
    /\ UNCHANGED << exec, node, top, expected, asserted >>

Next == Mark \/ Convert \/ FoldN \/ FoldT \/ Translate \/ Execute \/ (pc = "done" /\ UNCHANGED vars)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Properties, checked once the statement ran                              *)
(***************************************************************************)
\* the rows are those the SQL means -- a statement with INTO assigns one row instead (IntoFoldsLikeDevelop)
AInto == node["A"].kind = "LEAF" /\ node["A"].into
ResultMatches == (pc = "done" /\ outcome = "ROWS" /\ ~AInto) => result = expected
\* a SELECT with INTO that develop makes the statement (and so assigns) stays the statement: a kept set operation
\* does not assign the INTO of its first operand (do_select_internal () reads the statement's into_list)
DevelopFoldsToA == /\ node["T"].kind = "SETOP" /\ node["C"].kind = "LEAF" /\ node["C"].false /\ ~node["A"].false
                   /\ node["T"].op \in {"UNION", "EXCEPT"} /\ ~(node["A"].with /\ node["T"].with)
IntoFoldsLikeDevelop == (pc = "done" /\ ~Nested /\ AInto /\ DevelopFoldsToA) => top = "A"
\* typing again at execution keeps the query the client prepared
ExecStaysQuery == (pc = "done" /\ exec) => outcome # "ERROR"
\* no set operation is left with an operand folded into a value (debug assert)
NoValueOperand == pc = "done" => ~asserted
\* a value statement only where develop makes one too: every operand empty at compile time (the known [3])
KindOnlyAllEmpty == (pc = "done" /\ outcome = "KIND") => (~exec /\ expected = Empty)
\* an empty operand is read only below a set operation kept for a reason the PR accepts
ScanOnlyWhenKept == pc = "done" =>
    \A x \in scanned : \E s \in SetsIn(node, top) : /\ x \in Reach(node, s)
                                                     /\ node[s].kept \in {"NOTHING:distinct", "NOTHING:with", "nofold"}
\* at execution, a statement whose operands are all empty reads no table -- unless the top and an operand both
\* have a WITH clause, which the PR keeps unfolded as develop does (accepted, README)
AllFalse == \A x \in Leaves : node[x].kind = "LEAF" /\ node[x].false
TWith == node["T"].kind = "SETOP" /\ node["T"].with
ExecEmptyReadsNothing == (pc = "done" /\ exec /\ AllFalse /\ ~TWith) => scanned = {}
=============================================================================
