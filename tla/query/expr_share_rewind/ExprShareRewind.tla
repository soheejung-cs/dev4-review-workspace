-------------------------- MODULE ExprShareRewind --------------------------
(***************************************************************************)
(* CBRD-27994, PR #8469: the filter-slot cell count of the expression     *)
(* compiler's build context across marks and rewinds.                     *)
(*                                                                         *)
(* A consumer program over a heap scan (a value pointer list's projection, *)
(* an aggregate list's operands) may read an expression the scan's         *)
(* compiled data filter already computes: expr_compile_node_arith () gives *)
(* the node a cell wired to the filter's result slot (cell_src             *)
(* EXPR_CSRC_SHARE), flags it (cell_shared[]) and counts it (n_shared).    *)
(* A root or a subtree the compiler gives up on is rewound to the mark     *)
(* taken before it (expr_build_rewind ()).  The image the client writes    *)
(* carries n_shared, and the server's reader refuses an image whose        *)
(* n_shared exceeds its cells (expr_image_get_prog ()).                    *)
(*                                                                         *)
(* One compile of a list (expr_prog_compile_roots_impl ()): roots one by   *)
(* one, each a nest of node frames that build cells; a root is rejected   *)
(* (rewound) or accepted -- a projection list re-checks that something     *)
(* computes the root's cell (only_compute_roots) -- then the program is    *)
(* finished, kept or dropped by the consumer's rule, written and read.     *)
(* Each action is one branch of the code; README.md maps them to          *)
(* file:line.                                                              *)
(*                                                                         *)
(* FIX = FALSE is develop: the rewind drops the cells but leaves their     *)
(* flags and their count.  FIX = TRUE is the PR: it takes both back for    *)
(* every rewound cell that read a filter slot.                             *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS
    MaxCells,   \* the cell table (EXPR_MAX_STEPS), scaled down
    MaxDepth,   \* most marks open at once: the root's, then one per node frame (expr_compile_node () depth)
    MaxRoots,   \* most roots in a list
    MaxFrames,  \* most node frames one root opens -- a root is a finite tree
    FIX         \* TRUE: the PR, FALSE: develop

ASSUME /\ MaxCells \in Nat \ {0}
       /\ MaxDepth \in Nat \ {0, 1}
       /\ MaxRoots \in Nat \ {0}
       /\ MaxFrames \in Nat
       /\ FIX \in BOOLEAN

Idx == 0 .. (MaxCells - 1)

VARIABLES
    proj,       \* TRUE: a projection list (only_compute_roots), FALSE: aggregate operands (wired-only roots kept)
    nRoots,     \* roots in the list
    nCells,     \* bctx->n_cells
    src,        \* bctx->cell_src[]: "VAL" a wired leaf no step computes, "STEP" a computing step's,
                \* "SHARE" a filter slot's, "NONE" where no cell was ever built
    flag,       \* bctx->cell_shared[]
    nShared,    \* bctx->n_shared
    marks,      \* the open marks' n_cells, outermost first: the root's, then each node frame's
    full,       \* bctx->full: a table ran out under the node being compiled
    frames,     \* node frames the current root opened
    rootsDone,  \* roots compiled so far
    kept,       \* the cells of the roots the program covers (root_cells[])
    pc,         \* "idle" between roots | "root" | "done" (image written and read) | "none" (no program)
    accepted    \* pc = "done": the server's reader took the image

vars == <<proj, nRoots, nCells, src, flag, nShared, marks, full, frames, rootsDone, kept, pc, accepted>>

Live == {j \in Idx : j < nCells}
NShareCells == Cardinality({j \in Live : src[j] = "SHARE"})
NCompute == Cardinality({j \in Live : src[j] = "STEP"})

Init ==
    /\ proj \in BOOLEAN
    /\ nRoots \in 1 .. MaxRoots
    /\ nCells = 0
    /\ src = [j \in Idx |-> "NONE"]
    /\ flag = [j \in Idx |-> FALSE]
    /\ nShared = 0
    /\ marks = << >>
    /\ full = FALSE
    /\ frames = 0
    /\ rootsDone = 0
    /\ kept = {}
    /\ pc = "idle"
    /\ accepted = FALSE

(* expr_build_rewind () back to a mark taken at n_cells m: the cells go (n_cells = m); FIX also takes back the
   flag and the count of every rewound cell that read a filter slot *)
RewoundFlag(m) == IF FIX THEN [j \in Idx |-> IF m <= j /\ j < nCells THEN FALSE ELSE flag[j]] ELSE flag
RewoundCount(m) == IF FIX THEN nShared - Cardinality({j \in Idx : m <= j /\ j < nCells /\ flag[j]}) ELSE nShared

(* expr_prog_compile_roots_impl (): expr_build_mark () before the root *)
RootBegin ==
    /\ pc = "idle"
    /\ rootsDone < nRoots
    /\ marks' = <<nCells>>
    /\ frames' = 0
    /\ pc' = "root"
    /\ UNCHANGED <<proj, nRoots, nCells, src, flag, nShared, full, rootsDone, kept, accepted>>

(* expr_compile_node (): an arithmetic node -- depth + 1, expr_build_mark () *)
NodeBegin ==
    /\ pc = "root"
    /\ ~full
    /\ Len(marks) < MaxDepth
    /\ frames < MaxFrames
    /\ marks' = Append(marks, nCells)
    /\ frames' = frames + 1
    /\ UNCHANGED <<proj, nRoots, nCells, src, flag, nShared, full, rootsDone, kept, pc, accepted>>

(* expr_new_cell () for a leaf (TYPE_CONSTANT / TYPE_DBVAL: EXPR_CSRC_VALUE / EXPR_CSRC_LITERAL) or for a computing
   step's cell (expr_new_step_with_cell ()): cell_shared[] is not written.  A full table makes no cell. *)
NewCell(k) ==
    /\ pc = "root"
    /\ ~full
    /\ (k = "VAL" \/ Len(marks) >= 2)   \* a computing step belongs to a node frame; a leaf may be the root itself
    /\ IF nCells < MaxCells
         THEN /\ src' = [src EXCEPT ![nCells] = k]
              /\ nCells' = nCells + 1
              /\ UNCHANGED full
         ELSE /\ full' = TRUE
              /\ UNCHANGED <<src, nCells>>
    /\ UNCHANGED <<proj, nRoots, flag, nShared, marks, frames, rootsDone, kept, pc, accepted>>

(* expr_compile_node_arith (): the filter computes this node -- a cell wired to its slot, flagged and counted *)
ShareHit ==
    /\ pc = "root"
    /\ ~full
    /\ Len(marks) >= 2
    /\ IF nCells < MaxCells
         THEN /\ src' = [src EXCEPT ![nCells] = "SHARE"]
              /\ flag' = [flag EXCEPT ![nCells] = TRUE]
              /\ nShared' = nShared + 1
              /\ nCells' = nCells + 1
              /\ UNCHANGED full
         ELSE /\ full' = TRUE
              /\ UNCHANGED <<src, flag, nShared, nCells>>
    /\ UNCHANGED <<proj, nRoots, marks, frames, rootsDone, kept, pc, accepted>>

(* expr_compile_node () returns without rewinding: the node compiled, or it failed and its caller fails in turn
   (a full table under the root's own node, depth 1, is not nested) *)
NodeEnd ==
    /\ pc = "root"
    /\ Len(marks) >= 2
    /\ (~full \/ Len(marks) = 2)
    /\ marks' = SubSeq(marks, 1, Len(marks) - 1)
    /\ UNCHANGED <<proj, nRoots, nCells, src, flag, nShared, full, frames, rootsDone, kept, pc, accepted>>

(* expr_compile_node (): a table ran out under a node below the root's own (depth > 1) -- expr_build_rewind () to
   the node's mark, then the subtree becomes a program of its own behind one step (expr_compile_nested ()), whose
   compile may decline (no step) or find no room (full again) *)
NodeNest ==
    /\ pc = "root"
    /\ full
    /\ Len(marks) >= 3
    /\ LET m == marks[Len(marks)]
       IN /\ flag' = RewoundFlag(m)
          /\ nShared' = RewoundCount(m)
          /\ \/ /\ m < MaxCells
                /\ src' = [src EXCEPT ![m] = "STEP"]
                /\ nCells' = m + 1
                /\ full' = FALSE
             \/ /\ nCells' = m
                /\ full' = (m >= MaxCells)
                /\ UNCHANGED src
    /\ marks' = SubSeq(marks, 1, Len(marks) - 1)
    /\ UNCHANGED <<proj, nRoots, frames, rootsDone, kept, pc, accepted>>

(* expr_prog_compile_roots_impl (): the root's compile returned -1 -- expr_build_rewind () to the root's mark *)
RootReject ==
    /\ pc = "root"
    /\ Len(marks) = 1
    /\ flag' = RewoundFlag(marks[1])
    /\ nShared' = RewoundCount(marks[1])
    /\ nCells' = marks[1]
    /\ marks' = << >>
    /\ full' = FALSE
    /\ rootsDone' = rootsDone + 1
    /\ pc' = "idle"
    /\ UNCHANGED <<proj, nRoots, src, frames, kept, accepted>>

(* expr_prog_compile_roots_impl (): the root's compile returned cell c -- one this root built, or an earlier
   root's through the CSE table.  A projection list keeps the root only when a computing step publishes c or c is
   flagged as read from the filter (only_compute_roots); otherwise it is rewound like a rejected one. *)
RootAccept ==
    /\ pc = "root"
    /\ Len(marks) = 1
    /\ ~full
    /\ \E c \in Live :
         IF proj /\ ~(src[c] = "STEP" \/ flag[c])
           THEN /\ flag' = RewoundFlag(marks[1])
                /\ nShared' = RewoundCount(marks[1])
                /\ nCells' = marks[1]
                /\ UNCHANGED kept
           ELSE /\ kept' = kept \cup {c}
                /\ UNCHANGED <<flag, nShared, nCells>>
    /\ marks' = << >>
    /\ full' = FALSE
    /\ rootsDone' = rootsDone + 1
    /\ pc' = "idle"
    /\ UNCHANGED <<proj, nRoots, src, frames, accepted>>

(* expr_prog_finish (), the consumer's keep rule -- a projection list drops a program that computes nothing and
   reads no filter slot (expr_valptr_prog_keep ()), an aggregate list keeps any program covering a root
   (expr_agg_prog_keep ()) -- the image (expr_prog_image_pack ()) and the server's reader
   (expr_image_get_prog ()): n_shared must lie in [0, n_cells] *)
Finish ==
    /\ pc = "idle"
    /\ rootsDone = nRoots
    /\ IF kept # {} /\ (proj => (NCompute > 0 \/ nShared > 0))
         THEN /\ pc' = "done"
              /\ accepted' = (nShared <= nCells)
         ELSE /\ pc' = "none"
              /\ UNCHANGED accepted
    /\ UNCHANGED <<proj, nRoots, nCells, src, flag, nShared, marks, full, frames, rootsDone, kept>>

Done == pc \in {"done", "none"} /\ UNCHANGED vars

Next ==
    \/ RootBegin \/ NodeBegin \/ NewCell("VAL") \/ NewCell("STEP") \/ ShareHit \/ NodeEnd \/ NodeNest
    \/ RootReject \/ RootAccept \/ Finish \/ Done

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
TypeOK ==
    /\ proj \in BOOLEAN
    /\ nRoots \in 1 .. MaxRoots
    /\ nCells \in 0 .. MaxCells
    /\ src \in [Idx -> {"NONE", "VAL", "STEP", "SHARE"}]
    /\ flag \in [Idx -> BOOLEAN]
    /\ nShared \in Nat
    /\ marks \in Seq(0 .. MaxCells)
    /\ Len(marks) <= MaxDepth
    /\ full \in BOOLEAN
    /\ frames \in 0 .. MaxFrames
    /\ rootsDone \in 0 .. MaxRoots
    /\ kept \subseteq Idx
    /\ pc \in {"idle", "root", "done", "none"}
    /\ accepted \in BOOLEAN

(* the PR's claim, which it asserts (debug) where the program is finished: n_shared counts the cells that read a
   filter slot -- here at every state *)
CountMatches == nShared = NShareCells

(* cell_shared[] says exactly which live cells read a filter slot: an index past n_cells is never flagged, so a
   cell built there later does not pass for a computed one *)
FlagIsSrc == \A j \in Idx : flag[j] <=> (j < nCells /\ src[j] = "SHARE")

(* the server takes the image the client wrote -- else a debug server aborts in expr_valptr_prog_settle () /
   expr_agg_prog_settle () (`image_size == 0 || prog != NULL') and a release one interprets the list *)
ImageAccepted == pc = "done" => accepted

(* a projection list keeps a root for the program only when something computes its cell (only_compute_roots) *)
KeptRootComputes == proj => \A c \in kept : src[c] \in {"STEP", "SHARE"}

(* the consumer runs a program -- a projection list keeps it (expr_valptr_prog_keep ()), the server keeps an
   aggregate list's as active rather than plain operands (qdata_agg_operand_prog_compile ()) -- only when it computes
   something or reads what the filter computed *)
Active == kept # {} /\ (NCompute > 0 \/ nShared > 0)
ActiveComputes == (pc = "done" /\ accepted /\ Active) => \E j \in Live : src[j] \in {"STEP", "SHARE"}
=============================================================================
