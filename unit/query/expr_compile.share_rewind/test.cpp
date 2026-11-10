/*
 * PR#8469 (CBRD-27994) -- expr_build_rewind () and the filter-slot cells it rewinds (src/query/expr_compile.c, server
 * mode).
 *
 * A consumer program over a heap scan (the projection of a value pointer list, an aggregate's operands) may read an
 * expression the scan's compiled data filter already computes: expr_compile_node_arith () gives the node a cell wired
 * to the filter's result slot (cell_src EXPR_CSRC_SHARE), flags the cell (cell_shared[]) and counts it (n_shared) --
 * one hit, three records.  A root or subtree the compiler then rejects is rewound to the mark taken before it
 * (expr_build_rewind ()).  The base rewinds the cells but leaves the flag and the count; the PR takes both back for
 * every rewound cell that read a filter slot, and expr_prog_finish () asserts (debug) n_shared == the
 * EXPR_CSRC_SHARE cells.
 *
 *   contract  C1  a hit is one EXPR_CSRC_SHARE cell wired to the filter's slot, its flag and one count
 *             C2  mark, hit, rewind: n_shared back to the mark's, the rewound index unflagged
 *             C3  a hit before the mark keeps its flag and count while a rejected LEAST after the mark is rewound
 *             C4  rewinding cells that read no filter slot leaves n_shared alone
 *             C5  a plain cell built at a rewound filter-slot index starts unflagged
 *             E0  control: [c1, CAST (c1 AS DOUBLE)] -- counts its one filter-slot cell, the image unpacks
 *             E1  the issue's list [c1, LEAST (CAST (c1 AS DOUBLE), CASE c1 WHEN c2 THEN c0 ELSE c0 END),
 *                 CAST (c1 AS DOUBLE)] over WHERE CAST (c1 AS DOUBLE) > 0: the program counts the one filter-slot cell
 *                 it carries (the invariant of expr_prog_finish ()'s new assert) and its image unpacks into it
 *             E2  [LEAST (...), c1]: the plain column at the rewound index keeps the consumer's path
 *                 (only_compute_roots) -- no program
 *   defect    F1  (direct) the rewound cell stays counted and flagged
 *             F1  (image) the issue's list: n_shared 2 over one cell; expr_prog_image_unpack () refuses the image
 *             F2  [LEAST (...), c1]: the plain column passes for computed -- a program with n_shared 1 and no
 *                 filter-slot cell, whose image the reader accepts
 *
 * The lists compile as both sides compile a value pointer list's projection (expr_valptr_prog_image_at_generation (),
 * expr_valptr_prog_settle ()): allow_fallback_roots false, allow_wired_only false, only_compute_roots true, then
 * expr_valptr_prog_keep (); here over the server's environment (expr_prog_compile_roots ()), the filter's registry
 * built by hand (one entry: the filter's CAST (c1 AS DOUBLE), its result slot).  The rewind does not depend on the
 * environment.
 *
 * Real code: the whole TU -- compile, mark and rewind, finish, materialize, image pack and unpack.  Faked:
 *   tp_domain_match ()  object_domain.c tp_domain_match_internal (): NULL -> 0, the same object -> 1 ("both cached");
 *                       every domain here is one object per type, as the server's domain cache holds them, so no other
 *                       branch is reached (a call that would need one fails the case)
 *   domain_compare_is_exact_number ()  domain_rules.c, copied (an integer against a NUMERIC)
 *   or_pack_int () / or_unpack_int ()  object_representation.c: OR_PUT_INT / OR_GET_INT and 4 bytes on
 *   fetch_arith_op ()   a debug build's expr_compile_generic () asserts the opcode has a body; a non-NULL marker
 *                       (no generic step survives in these programs, so none is bound)
 * TP_DOMAIN / PR_TYPE: zeroed objects with type->id set; the paths read TP_DOMAIN_TYPE () and collation_flag
 * (0 = TP_DOMAIN_COLL_NORMAL) only.
 *
 * mutants/ (head, --patch): M1 keep the flag -> C2 C3 C5 E2 FAIL; M2 keep the count -> C2 C3 C5 E1 FAIL; M3 rewind
 * from index 0 -> C3 C4 FAIL; M4 uncount every rewound cell -> C4 E0 E1 FAIL.  Under debug flags M2 (E1) and M4 (E0)
 * abort in expr_prog_finish () on `prog->n_shared == n_share_cells'.
 */
#include "catch.hpp"

#include "dbtype.h"
#include "regu_var.hpp"
#include "xasl.h"
#include "xasl_predicate.hpp"
#include "object_domain.h"
#include "object_primitive.h"
#include "object_representation.h"
#include "domain_rules.h"
#include "fetch_arith_ops.h"
#include "expr_compile.h"

#include <cstdlib>
#include <cstring>
#include <vector>

/* ================================================================ fakes */

static int g_domain_match_other = 0;	/* calls the same-object branch does not answer */

int
tp_domain_match (const TP_DOMAIN * dom1, const TP_DOMAIN * dom2, TP_MATCH exact)
{
  (void) exact;
  if (dom1 == NULL || dom2 == NULL)
    {
      return 0;
    }
  if (dom1 == dom2)
    {
      return 1;
    }
  g_domain_match_other++;
  return 0;
}

bool
domain_compare_is_exact_number (DB_TYPE type1, DB_TYPE type2)
{
  auto is_integer =[](DB_TYPE t)
  {
    return t == DB_TYPE_SHORT || t == DB_TYPE_INTEGER || t == DB_TYPE_BIGINT;
  };
  return (type1 == DB_TYPE_NUMERIC && is_integer (type2)) || (type2 == DB_TYPE_NUMERIC && is_integer (type1));
}

char *
or_pack_int (char *ptr, int number)
{
  if (ptr == NULL)
    {
      return NULL;
    }
  OR_PUT_INT (ptr, number);
  return ptr + OR_INT_SIZE;
}

char *
or_unpack_int (char *ptr, int *number)
{
  if (ptr == NULL)
    {
      *number = 0;
      return NULL;
    }
  *number = OR_GET_INT (ptr);
  return ptr + OR_INT_SIZE;
}

static int
fake_arith_body (THREAD_ENTRY * thread_p, FETCH_ARITH_ARGS * a)
{
  (void) thread_p;
  (void) a;
  return ER_FAILED;
}

FETCH_ARITH_OP
fetch_arith_op (OPERATOR_TYPE opcode)
{
  (void) opcode;
  return fake_arith_body;
}

/* ================================================================ hooks (appendix) */

extern void *unitseam_filter_with_share (const REGU_VARIABLE * const *regus, DB_VALUE * const *slots, int n);
extern void *unitseam_bctx_new (const void *share_spec);
extern void unitseam_bctx_free (void *bctx);
extern int unitseam_compile (void *bctx, REGU_VARIABLE * regu);
extern void *unitseam_mark (const void *bctx);
extern void unitseam_rewind (void *bctx, const void *mark);
extern void unitseam_mark_free (void *mark);
extern int unitseam_n_cells (const void *bctx);
extern int unitseam_n_shared (const void *bctx);
extern bool unitseam_cell_shared (const void *bctx, int cell);
extern int unitseam_cell_src (const void *bctx, int cell);
extern const DB_VALUE *unitseam_cell_addr (const void *bctx, int cell);
extern int unitseam_n_share_cells (const void *bctx);
extern EXPR_PROG *unitseam_valptr_prog_keep (EXPR_PROG * prog, const int *idx, int n);

/* ================================================================ the scene */

namespace
{
  /* one object per type, as the server's domain cache holds them */
  struct domains
  {
    alignas (PR_TYPE) unsigned char type_mem[3][sizeof (PR_TYPE)];
    TP_DOMAIN d_int, d_bigint, d_double;

    domains ()
    {
      memset (type_mem, 0, sizeof (type_mem));
      memset (&d_int, 0, sizeof (d_int));
      memset (&d_bigint, 0, sizeof (d_bigint));
      memset (&d_double, 0, sizeof (d_double));
      reinterpret_cast < PR_TYPE * >(type_mem[0])->id = DB_TYPE_INTEGER;
      reinterpret_cast < PR_TYPE * >(type_mem[1])->id = DB_TYPE_BIGINT;
      reinterpret_cast < PR_TYPE * >(type_mem[2])->id = DB_TYPE_DOUBLE;
      d_int.type = reinterpret_cast < PR_TYPE * >(type_mem[0]);
      d_bigint.type = reinterpret_cast < PR_TYPE * >(type_mem[1]);
      d_double.type = reinterpret_cast < PR_TYPE * >(type_mem[2]);
    }
  };

  domains & D ()
  {
    static domains d;
    return d;
  }

  /* CREATE TABLE t (c0 DOUBLE, c1 INT, c2 BIGINT) scanned with WHERE CAST (c1 AS DOUBLE) > 0: the scan fetches c0,
   * c1, c2 into value-list slots (its regu list maps each attribute to its slot), its compiled data filter computes
   * CAST (c1 AS DOUBLE) into a result slot and offers it in the share registry.  The consumer's nodes read the
   * columns as TYPE_CONSTANT leaves over those slots; the filter names c1 as TYPE_ATTR_ID. */
  struct scene
  {
    DB_VALUE v_c0, v_c1, v_c2;	/* the scan's value-list slots */
    DB_VALUE filter_slot;	/* the filter's result slot of CAST (c1 AS DOUBLE) */
    REGU_VARIABLE regu[48];
    ARITH_TYPE arith[24];
    PRED_EXPR pred[8];
    regu_variable_list_node fetch[3];	/* the scan's cls_regu_list_pred: attribute -> slot */
    PRED_EXPR where;		/* the data filter's root: only its compiled form's fields are read */
    alignas (ACCESS_SPEC_TYPE) unsigned char spec_mem[sizeof (ACCESS_SPEC_TYPE)];
    ACCESS_SPEC_TYPE *spec;
    REGU_VARIABLE *filter_cast;
    void *filter;
    int n_regu = 0, n_arith = 0, n_pred = 0;

    scene ()
    {
      memset (&v_c0, 0, sizeof (v_c0));
      memset (&v_c1, 0, sizeof (v_c1));
      memset (&v_c2, 0, sizeof (v_c2));
      memset (&filter_slot, 0, sizeof (filter_slot));
      memset ((void *) regu, 0, sizeof (regu));
      memset ((void *) arith, 0, sizeof (arith));
      memset ((void *) pred, 0, sizeof (pred));
      memset ((void *) fetch, 0, sizeof (fetch));
      memset ((void *) &where, 0, sizeof (where));
      memset (spec_mem, 0, sizeof (spec_mem));

      DB_VALUE *slots[3] = { &v_c0, &v_c1, &v_c2 };
      for (int i = 0; i < 3; i++)
	{
	  fetch[i].value.type = TYPE_ATTR_ID;
	  fetch[i].value.vfetch_to = slots[i];
	  fetch[i].value.value.attr_descr.id = i;
	  fetch[i].next = (i < 2) ? &fetch[i + 1] : NULL;
	}

      filter_cast = node (T_CAST, &D ().d_double, NULL, attr (1, &D ().d_int));
      DB_VALUE *fslot = &filter_slot;
      filter = unitseam_filter_with_share (&filter_cast, &fslot, 1);

      where.type = T_EVAL_TERM;
      where.pe.m_eval_term.et_type = T_COMP_EVAL_TERM;
      where.pe.m_eval_term.et.et_comp.lhs = filter_cast;
      where.pe.m_eval_term.et.et_comp.rel_op = R_GT;
      where.scan_prog = filter;
      where.scan_prog_state = 1;	/* active: the scan compiled its filter */
      where.scan_prog_gen = 7;

      spec = reinterpret_cast < ACCESS_SPEC_TYPE * >(spec_mem);
      spec->type = TARGET_CLASS;
      spec->where_pred = &where;
      spec->s.cls_node.cls_regu_list_pred = &fetch[0];
      spec->s.cls_node.cls_regu_list_rest = NULL;
    }

    ~scene ()
    {
      expr_scan_pred_free (filter);
    }

    REGU_VARIABLE *attr (int id, TP_DOMAIN * d)
    {
      REGU_VARIABLE *r = &regu[n_regu++];
      r->type = TYPE_ATTR_ID;
      r->domain = d;
      r->value.attr_descr.id = id;
      return r;
    }

    /* a column as the consumer reads it: the scan's value-list slot */
    REGU_VARIABLE *col (DB_VALUE * slot, TP_DOMAIN * d)
    {
      REGU_VARIABLE *r = &regu[n_regu++];
      r->type = TYPE_CONSTANT;
      r->domain = d;
      r->value.dbvalptr = slot;
      return r;
    }

    REGU_VARIABLE *c0 ()
    {
      return col (&v_c0, &D ().d_double);
    }
    REGU_VARIABLE *c1 ()
    {
      return col (&v_c1, &D ().d_int);
    }
    REGU_VARIABLE *c2 ()
    {
      return col (&v_c2, &D ().d_bigint);
    }

    REGU_VARIABLE *node (OPERATOR_TYPE op, TP_DOMAIN * d, REGU_VARIABLE * l, REGU_VARIABLE * r, PRED_EXPR * p = NULL)
    {
      ARITH_TYPE *a = &arith[n_arith++];
      a->opcode = op;
      a->domain = d;
      a->leftptr = l;
      a->rightptr = r;
      a->pred = p;
      a->fails = NO_ERROR;
      REGU_VARIABLE *x = &regu[n_regu++];
      x->type = TYPE_INARITH;
      x->domain = d;
      x->value.arithptr = a;
      return x;
    }

    /* CAST (c1 AS DOUBLE) as the consumer writes it: the filter computes the same expression */
    REGU_VARIABLE *cast_c1 ()
    {
      return node (T_CAST, &D ().d_double, NULL, c1 ());
    }

    /* CASE c1 WHEN c2 THEN c0 ELSE c0 END: its predicate c1 = c2 compares an INTEGER with a BIGINT, which the
     * compiler leaves interpreted (expr_compile_pred (): differing sides), so the CASE -- and a node over it -- is
     * rejected */
    REGU_VARIABLE *simple_case ()
    {
      PRED_EXPR *p = &pred[n_pred++];
      p->type = T_EVAL_TERM;
      p->pe.m_eval_term.et_type = T_COMP_EVAL_TERM;
      p->pe.m_eval_term.et.et_comp.lhs = c1 ();
      p->pe.m_eval_term.et.et_comp.rhs = c2 ();
      p->pe.m_eval_term.et.et_comp.rel_op = R_EQ;
      return node (T_CASE, &D ().d_double, c0 (), c0 (), p);
    }

    /* LEAST (CAST (c1 AS DOUBLE), CASE c1 WHEN c2 THEN c0 ELSE c0 END): a generic step over its two operands; the
     * first is a filter-slot hit, the second is rejected, so the whole node is */
    REGU_VARIABLE *least_rejected ()
    {
      return node (T_LEAST, &D ().d_double, cast_c1 (), simple_case ());
    }

    /* the value pointer list's projection program, as both sides compile and keep it */
    EXPR_PROG *compile_list (REGU_VARIABLE ** roots, int n, int *idx)
    {
      EXPR_PROG *prog = expr_prog_compile_roots (NULL, roots, n, NULL, false, false, true, idx, spec);
      return unitseam_valptr_prog_keep (prog, idx, n);
    }
  };

  /* a build context over the scene's scan */
  struct bctx_guard
  {
    void *b;
    explicit bctx_guard (const scene & s):b (unitseam_bctx_new (s.spec))
    {
    }
    ~bctx_guard ()
    {
      unitseam_bctx_free (b);
    }
  };

  struct mark_guard
  {
    void *m;
    explicit mark_guard (const void *bctx):m (unitseam_mark (bctx))
    {
    }
    ~mark_guard ()
    {
      unitseam_mark_free (m);
    }
  };

  int n_share_cells (const EXPR_PROG * prog)
  {
    int n = 0;
    for (int i = 0; i < prog->n_cells; i++)
      {
	n += (prog->cell_src[i] == EXPR_CSRC_SHARE) ? 1 : 0;
      }
    return n;
  }

  /* the image round trip: the client packs the program it compiled, the server unpacks it over the same roots and
   * the same scan; NULL when the reader refuses it */
  struct round_trip
  {
    char *image = NULL;
    int size = 0;
    std::vector < int >idx;
    EXPR_PROG *bound = NULL;

    round_trip (const EXPR_PROG * prog, REGU_VARIABLE ** roots, int n, const int *root_idx, const void *spec)
      : idx (n, -9)
    {
      image = expr_prog_image_pack (prog, roots, n, root_idx, &size);
      if (image != NULL)
	{
	  bound = expr_prog_image_unpack (image, size, roots, n, idx.data (), spec);
	}
    }
    ~round_trip ()
    {
      expr_prog_free (bound);
      free (image);
    }
  };
}

/* ================================================================ contract: the build context */

TEST_CASE ("contract C1: a filter-slot hit is one EXPR_CSRC_SHARE cell wired to the filter's slot, its flag and one count",
	   "[contract]")
{
  scene s;
  bctx_guard g (s);

  int cell = unitseam_compile (g.b, s.cast_c1 ());

  REQUIRE (cell == 0);
  CHECK (unitseam_n_cells (g.b) == 1);
  CHECK (unitseam_cell_src (g.b, cell) == EXPR_CSRC_SHARE);
  CHECK (unitseam_cell_addr (g.b, cell) == &s.filter_slot);
  CHECK (unitseam_cell_shared (g.b, cell));
  CHECK (unitseam_n_shared (g.b) == 1);
  CHECK (g_domain_match_other == 0);
}

TEST_CASE ("contract C2: rewinding past a filter-slot cell takes n_shared back to the mark's and unflags the index",
	   "[contract]")
{
  scene s;
  bctx_guard g (s);
  mark_guard m (g.b);

  int cell = unitseam_compile (g.b, s.cast_c1 ());
  REQUIRE (cell == 0);
  REQUIRE (unitseam_n_shared (g.b) == 1);

  unitseam_rewind (g.b, m.m);

  CHECK (unitseam_n_cells (g.b) == 0);
  CHECK (unitseam_n_shared (g.b) == 0);
  CHECK_FALSE (unitseam_cell_shared (g.b, cell));
}

TEST_CASE ("contract C3: a filter-slot cell before the mark keeps its flag and count while a rejected LEAST after the "
	   "mark is rewound", "[contract]")
{
  scene s;
  bctx_guard g (s);

  int kept = unitseam_compile (g.b, s.cast_c1 ());
  REQUIRE (kept == 0);

  mark_guard m (g.b);
  int rejected = unitseam_compile (g.b, s.least_rejected ());
  REQUIRE (rejected == -1);
  /* the LEAST's first operand hit the filter's slot at index 1 before its second operand was rejected */
  REQUIRE (unitseam_n_cells (g.b) == 2);
  REQUIRE (unitseam_cell_src (g.b, 1) == EXPR_CSRC_SHARE);
  REQUIRE (unitseam_n_shared (g.b) == 2);

  unitseam_rewind (g.b, m.m);

  CHECK (unitseam_n_cells (g.b) == 1);
  CHECK (unitseam_cell_src (g.b, kept) == EXPR_CSRC_SHARE);
  CHECK (unitseam_cell_shared (g.b, kept));
  CHECK (unitseam_n_shared (g.b) == 1);
  CHECK_FALSE (unitseam_cell_shared (g.b, 1));
  CHECK (unitseam_n_shared (g.b) == unitseam_n_share_cells (g.b));
}

TEST_CASE ("contract C4: rewinding cells that read no filter slot leaves n_shared alone", "[contract]")
{
  scene s;
  bctx_guard g (s);

  REQUIRE (unitseam_compile (g.b, s.cast_c1 ()) == 0);

  mark_guard m (g.b);
  REQUIRE (unitseam_compile (g.b, s.c1 ()) == 1);
  REQUIRE (unitseam_compile (g.b, s.c2 ()) == 2);
  REQUIRE (unitseam_compile (g.b, s.c0 ()) == 3);
  REQUIRE (unitseam_cell_src (g.b, 1) == EXPR_CSRC_VALUE);

  unitseam_rewind (g.b, m.m);

  CHECK (unitseam_n_cells (g.b) == 1);
  CHECK (unitseam_n_shared (g.b) == 1);
  CHECK (unitseam_cell_shared (g.b, 0));
}

TEST_CASE ("contract C5: a plain cell built at a rewound filter-slot index starts unflagged", "[contract]")
{
  scene s;
  bctx_guard g (s);
  mark_guard m (g.b);

  REQUIRE (unitseam_compile (g.b, s.cast_c1 ()) == 0);
  unitseam_rewind (g.b, m.m);

  int cell = unitseam_compile (g.b, s.c1 ());
  REQUIRE (cell == 0);
  CHECK (unitseam_cell_src (g.b, cell) == EXPR_CSRC_VALUE);
  CHECK_FALSE (unitseam_cell_shared (g.b, cell));
  CHECK (unitseam_n_shared (g.b) == 0);
}

/* ================================================================ contract: the projection list and its image */

TEST_CASE ("contract E0: control -- [c1, CAST (c1 AS DOUBLE)] counts its one filter-slot cell and its image unpacks",
	   "[contract]")
{
  scene s;
  REGU_VARIABLE *roots[2] = { s.c1 (), s.cast_c1 () };
  int idx[2] = { -9, -9 };

  EXPR_PROG *prog = s.compile_list (roots, 2, idx);

  REQUIRE (prog != NULL);
  CHECK (idx[0] == -1);
  CHECK (idx[1] == 0);
  CHECK (prog->n_cells == 1);
  CHECK (n_share_cells (prog) == 1);
  CHECK (prog->n_shared == n_share_cells (prog));
  {
    round_trip rt (prog, roots, 2, idx, s.spec);
    REQUIRE (rt.bound != NULL);
    CHECK (rt.bound->n_shared == 1);
    CHECK (rt.bound->cells[0] == &s.filter_slot);
    CHECK (rt.idx == std::vector < int > ({-1, 0}));
  }
  expr_prog_free (prog);
}

TEST_CASE ("contract E1: the CBRD-27994 list counts the one filter-slot cell it carries (expr_prog_finish ()'s assert) "
	   "and its image unpacks into that program", "[contract]")
{
  scene s;
  REGU_VARIABLE *roots[3] = { s.c1 (), s.least_rejected (), s.cast_c1 () };
  int idx[3] = { -9, -9, -9 };

  EXPR_PROG *prog = s.compile_list (roots, 3, idx);

  REQUIRE (prog != NULL);
  CHECK (idx[0] == -1);
  CHECK (idx[1] == -1);
  CHECK (idx[2] == 0);
  CHECK (prog->n_cells == 1);
  CHECK (prog->cell_src[0] == EXPR_CSRC_SHARE);
  CHECK (prog->n_shared == n_share_cells (prog));
  CHECK (prog->n_shared == 1);
  {
    round_trip rt (prog, roots, 3, idx, s.spec);
    REQUIRE (rt.image != NULL);
    CHECK (rt.bound != NULL);
    if (rt.bound != NULL)
      {
	CHECK (rt.bound->n_cells == 1);
	CHECK (rt.bound->n_shared == 1);
	CHECK (rt.bound->cell_src[0] == EXPR_CSRC_SHARE);
	CHECK (rt.bound->cells[0] == &s.filter_slot);
	CHECK (rt.bound->share_gen == s.where.scan_prog_gen);
	CHECK (rt.idx == std::vector < int > ({-1, -1, 0}));
      }
  }
  expr_prog_free (prog);
}

TEST_CASE ("contract E2: a plain column compiled at the index a rejected root's filter-slot cell was rewound from keeps "
	   "the consumer's path (only_compute_roots) -- no program", "[contract]")
{
  scene s;
  REGU_VARIABLE *roots[2] = { s.least_rejected (), s.c1 () };
  int idx[2] = { -9, -9 };

  EXPR_PROG *prog = s.compile_list (roots, 2, idx);

  CHECK (idx[0] == -1);
  CHECK (idx[1] == -1);
  CHECK (prog == NULL);
  expr_prog_free (prog);
}

/* ================================================================ defect (PASS = the defect is there) */

TEST_CASE ("PR8469 F1: expr_build_rewind () leaves a rewound filter-slot cell counted in n_shared and flagged in "
	   "cell_shared[]", "[defect]")
{
  scene s;
  bctx_guard g (s);
  mark_guard m (g.b);

  int cell = unitseam_compile (g.b, s.cast_c1 ());
  REQUIRE (cell == 0);

  unitseam_rewind (g.b, m.m);

  CHECK (unitseam_n_cells (g.b) == 0);
  CHECK (unitseam_n_shared (g.b) == 1);
  CHECK (unitseam_cell_shared (g.b, cell));
}

TEST_CASE ("PR8469 F1: the CBRD-27994 list -- n_shared 2 over one filter-slot cell, and expr_prog_image_unpack () "
	   "refuses the image", "[defect]")
{
  scene s;
  REGU_VARIABLE *roots[3] = { s.c1 (), s.least_rejected (), s.cast_c1 () };
  int idx[3] = { -9, -9, -9 };

  EXPR_PROG *prog = s.compile_list (roots, 3, idx);

  REQUIRE (prog != NULL);
  INFO ("n_cells " << prog->n_cells << ", n_shared " << prog->n_shared << ", filter-slot cells " <<
	n_share_cells (prog));
  CHECK (prog->n_cells == 1);
  CHECK (n_share_cells (prog) == 1);
  CHECK (prog->n_shared == 2);
  {
    round_trip rt (prog, roots, 3, idx, s.spec);
    REQUIRE (rt.image != NULL);
    CHECK (rt.bound == NULL);
  }
  expr_prog_free (prog);
}

TEST_CASE ("PR8469 F2: a plain column compiled at a rewound filter-slot index passes for computed -- a program with "
	   "n_shared 1 and no filter-slot cell, whose image the reader accepts", "[defect]")
{
  scene s;
  REGU_VARIABLE *roots[2] = { s.least_rejected (), s.c1 () };
  int idx[2] = { -9, -9 };

  EXPR_PROG *prog = s.compile_list (roots, 2, idx);

  REQUIRE (prog != NULL);
  CHECK (idx[0] == -1);
  CHECK (idx[1] == 0);
  CHECK (prog->n_cells == 1);
  CHECK (prog->cell_src[0] == EXPR_CSRC_VALUE);
  CHECK (prog->n_compute == 0);
  CHECK (n_share_cells (prog) == 0);
  CHECK (prog->n_shared == 1);
  {
    round_trip rt (prog, roots, 2, idx, s.spec);
    REQUIRE (rt.image != NULL);
    CHECK (rt.bound != NULL);
    if (rt.bound != NULL)
      {
	CHECK (rt.bound->n_shared == 1);
	CHECK (n_share_cells (rt.bound) == 0);
      }
  }
  expr_prog_free (prog);
}
