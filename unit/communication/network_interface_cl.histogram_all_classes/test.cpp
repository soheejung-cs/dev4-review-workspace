/*
 * PR#8166 (CBRD-27249) -- update_histogram_for_all_classes (random_seed, skip_mops, n_skip)
 *
 * Unit under test: src/communication/network_interface_cl.c (SA_MODE) -- the client histogram pass of
 * UPDATE STATISTICS ON ALL CLASSES / optimizedb. It lists every top-level class (locator_get_all_class_mops
 * with the TU's own is_top_level_class filter), builds the all-columns histogram of each through
 * update_or_drop_histogram_helper (which also refreshes the class statistics), and prints the summary line.
 * The PR adds the skip list (optimizedb --no-histogram-classes): classes whose OID is in it are left out and
 * counted as "(k excluded)", entries that match no top-level class get a stderr warning, and LIST_MOPS is now
 * freed on every exit (the PR base never freed it).
 *
 * Fakes (other TUs) follow the real branch this path takes -- see the comment on each. The class listing
 * calls the real is_top_level_class on each fake class object, so "top-level" is the TU's own judgement.
 *
 * The PR changed the signature; the call is selected by the arity of the revision's own declaration and the
 * skip-list cases ([new]) return early where it does not exist.
 */
#include "catch.hpp"
#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

#include "error_code.h"
#include "dbtype_def.h"
#include "work_space.h"
#include "class_object.h"
#include "authenticate.h"
#include "schema_manager.h"
#include "locator_cl.h"
#include "execute_schema.h"
#include "network_interface_cl.h"
#include "parser.h"

/* ---- arity dispatch ---- */
template < typename F > struct arity;
template < typename R, typename ... A > struct arity <R (*)(A ...)>
{
  static constexpr int value = sizeof ... (A);
};

static constexpr int H_ARITY = arity < decltype (&update_histogram_for_all_classes) >::value;	/* 1 old base, 2 PR#8194 head, 3 develop (#8166), 4 PR#8194 rebased */

template < typename F > static int
call_h (F f, int seed, MOP * skip, int n)
{
  if constexpr (arity < F >::value == 4)
    {
      return f (seed, skip, n, (INT64) 0);	/* PR#8194 rebased on develop: skip list + run start */
    }
  else if constexpr (arity < F >::value == 3)
    {
      return f (seed, skip, n);
    }
  else if constexpr (arity < F >::value == 2)
    {
      return f (seed, (INT64) 0);	/* PR#8194 on its pre-#8166 base: run start, no skip list */
    }
  else
    {
      return f (seed);
    }
}

/* ---- the fake database ---- */
struct FakeClass
{
  const char *name;
  SM_CLASS_TYPE type;
  int partition;		/* -1 none, else DB_PARTITIONED_CLASS / DB_PARTITION_CLASS */
  int n_attrs;
  DB_OBJECT mop;
  SM_PARTITION part;
  alignas (SM_CLASS) unsigned char cls[sizeof (SM_CLASS)];
  alignas (8) unsigned char attrs[4][16];	/* opaque DB_ATTRIBUTE chain: only walked, never read */
};

static FakeClass g_db[] = {
  {"dba.t1", SM_CLASS_CT, -1, 2},
  {"dba.v1", SM_VCLASS_CT, -1, 1},			/* a view: not listed */
  {"dba.t2", SM_CLASS_CT, -1, 3},
  {"dba.tp", SM_CLASS_CT, DB_PARTITIONED_CLASS, 1},	/* partitioned class: listed */
  {"dba.tp__p__p0", SM_CLASS_CT, DB_PARTITION_CLASS, 1},	/* its partition: not listed */
  {"_db_class", SM_CLASS_CT, -1, 1},			/* a system class: listed */
};
static const int N_DB = sizeof (g_db) / sizeof (g_db[0]);

static void
db_init ()
{
  for (int i = 0; i < N_DB; i++)
    {
      FakeClass & c = g_db[i];
      memset (&c.mop, 0, sizeof (c.mop));
      memset (&c.part, 0, sizeof (c.part));
      memset (c.cls, 0, sizeof (c.cls));
      c.mop.oid_info.oid.volid = 0;
      c.mop.oid_info.oid.pageid = 300;
      c.mop.oid_info.oid.slotid = (short) (i + 1);
      c.mop.object = c.cls;
      SM_CLASS *k = reinterpret_cast < SM_CLASS * >(c.cls);
      k->class_type = c.type;
      if (c.partition >= 0)
	{
	  c.part.class_partition_type = (DB_CLASS_PARTITION_TYPE) c.partition;
	  k->partition = &c.part;
	}
    }
}

static FakeClass *
by_mop (MOP m)
{
  for (auto &c : g_db)
    {
      if (&c.mop == m)
	{
	  return &c;
	}
    }
  return NULL;
}

static MOP
mop_of (const char *name)
{
  for (auto &c : g_db)
    {
      if (strcmp (c.name, name) == 0)
	{
	  return &c.mop;
	}
    }
  return NULL;
}

/* ---- recorders and injected results ---- */
struct HistCall
{
  std::string name;
  int bucket_count;
  bool with_fullscan;
  int random_seed;
  void *target_columns;
  DO_HISTOGRAM what;
  bool quiet;
  bool auth_disabled;
};

static std::vector < HistCall > g_hist;
static std::vector < LIST_MOPS * >g_listed, g_freed;
static const char *g_fail_name;	/* the helper fails for this class */
static int g_fail_rc;
static const char *g_nofind_name;	/* db_find_class fails for this name */
static int g_last_er;
static bool g_list_fails;

alignas (authenticate_context) static unsigned char g_au_storage[sizeof (authenticate_context)];

static void
reset ()
{
  db_init ();
  g_hist.clear ();
  g_listed.clear ();
  g_freed.clear ();
  g_fail_name = NULL;
  g_fail_rc = NO_ERROR;
  g_nofind_name = NULL;
  g_last_er = NO_ERROR;
  g_list_fails = false;
  memset (g_au_storage, 0, sizeof (g_au_storage));
}

/* ---- fakes ---- */

authenticate_context *
au_ctx (void)
{
  return reinterpret_cast < authenticate_context * >(g_au_storage);
}

/* locator_cl.c locator_get_all_class_mops -> locator_fun_get_all_mops (sm_Root_class_mop, ...): every class
 * object is offered to fun and kept when it returns true; the LIST_MOPS is malloc'ed (sizeof (*list) +
 * n * sizeof (MOP)) and must be released with locator_free_list_mops. NULL on failure. */
LIST_MOPS *
locator_get_all_class_mops (DB_FETCH_MODE purpose, int (*fun) (MOBJ class_obj))
{
  if (g_list_fails)
    {
      g_last_er = ER_FAILED;
      return NULL;
    }
  LIST_MOPS *l = (LIST_MOPS *) malloc (sizeof (LIST_MOPS) + N_DB * sizeof (MOP));
  l->num = 0;
  for (auto &c : g_db)
    {
      if (fun == NULL || (*fun) ((MOBJ) c.cls))
	{
	  l->mops[l->num++] = &c.mop;
	}
    }
  g_listed.push_back (l);
  return l;
}

/* locator_cl.c: NULL the entries, free the array */
void
locator_free_list_mops (LIST_MOPS * mops)
{
  g_freed.push_back (mops);
  if (mops != NULL)
    {
      free (mops);
    }
}

/* schema_manager.c: the cached class's name */
const char *
sm_get_ch_name (MOP op)
{
  FakeClass *c = by_mop (op);
  return c ? c->name : NULL;
}

/* db_info.c: the name of a cached class gives back its MOP; an unknown name sets ER_LC_UNKNOWN_CLASSNAME */
DB_OBJECT *
db_find_class (const char *name)
{
  if (name == NULL || (g_nofind_name != NULL && strcmp (name, g_nofind_name) == 0))
    {
      g_last_er = ER_LC_UNKNOWN_CLASSNAME;
      return NULL;
    }
  return mop_of (name);
}

int
er_errid (void)
{
  return g_last_er;
}

/* execute_schema.c: the all-columns histogram build -- recorded; fails for g_fail_name */
int
update_or_drop_histogram_helper (PARSER_CONTEXT * parser, DB_OBJECT * const obj, bool quiet,
				 PT_HISTOGRAM_INFO * const info, DO_HISTOGRAM do_histogram, int *out_skipped)
{
  FakeClass *c = by_mop (obj);
  std::string n = c ? c->name : "?";
  g_hist.push_back (HistCall{n, info->bucket_count, info->with_fullscan, info->random_seed,
			     (void *) info->target_columns, do_histogram, quiet, au_ctx ()->disable_auth_check});
  if (out_skipped != NULL)
    {
      *out_skipped = 0;
    }
  if (g_fail_name != NULL && n == g_fail_name)
    {
      return g_fail_rc;
    }
  return NO_ERROR;
}

/* db_info.c: the attribute chain of a class (only walked to count columns) */
DB_ATTRIBUTE *
db_get_attributes_force (DB_OBJECT * obj)
{
  FakeClass *c = by_mop (obj);
  return (c != NULL && c->n_attrs > 0) ? (DB_ATTRIBUTE *) c->attrs[0] : NULL;
}

DB_ATTRIBUTE *
db_attribute_next (DB_ATTRIBUTE * attribute)
{
  for (auto &c : g_db)
    {
      for (int i = 0; i < c.n_attrs; i++)
	{
	  if ((void *) c.attrs[i] == (void *) attribute)
	    {
	      return (i + 1 < c.n_attrs) ? (DB_ATTRIBUTE *) c.attrs[i + 1] : NULL;
	    }
	}
    }
  return NULL;
}

/* ---- driver: capture stdout/stderr ---- */
struct Out
{
  int rc;
  std::string out, err;
};

static std::string
slurp (FILE * f)
{
  std::string s;
  char buf[512];
  size_t k;
  rewind (f);
  while ((k = fread (buf, 1, sizeof (buf), f)) > 0)
    {
      s.append (buf, k);
    }
  return s;
}

static Out
run_h (int seed, MOP * skip, int n)
{
  fflush (stdout);
  fflush (stderr);
  int so = dup (1), se = dup (2);
  FILE *fo = tmpfile (), *fe = tmpfile ();
  dup2 (fileno (fo), 1);
  dup2 (fileno (fe), 2);

  int rc = call_h (&update_histogram_for_all_classes, seed, skip, n);

  fflush (stdout);
  fflush (stderr);
  dup2 (so, 1);
  dup2 (se, 2);
  close (so);
  close (se);
  Out o{rc, slurp (fo), slurp (fe)};
  fclose (fo);
  fclose (fe);
  return o;
}

static std::string
hist_names ()
{
  std::string s;
  for (auto &h : g_hist)
    {
      s += (s.empty ()? "" : ",") + h.name;
    }
  return s;
}

#define NEW_CODE_ONLY() \
  do { if (H_ARITY != 3 && H_ARITY != 4) { FAIL ("no skip list in this revision (new code, not on the PR base)"); } } while (0)

/* ================================================================================================ */

TEST_CASE ("no skip list: every top-level class (not views, not partitions) gets its all-columns histogram",
	   "[contract]")
{
  reset ();
  Out o = run_h (5, NULL, 0);

  CHECK (o.rc == NO_ERROR);
  CHECK (hist_names () == "dba.t1,dba.t2,dba.tp,_db_class");
  for (auto &h : g_hist)
    {
      CHECK (h.bucket_count == -1);
      CHECK (h.with_fullscan == false);
      CHECK (h.random_seed == 5);
      CHECK (h.target_columns == NULL);
      CHECK (h.what == DO_HISTOGRAM_CREATE);
      CHECK (h.quiet);
      CHECK (h.auth_disabled);
    }
  CHECK_FALSE (au_ctx ()->disable_auth_check);
  CHECK (o.out == "Statistics updated successfully: 4 tables, 7 columns.\n");
  CHECK (o.err.empty ());
}

TEST_CASE ("the helper answering ER_OBJ_INVALID_ARGUMENTS for a class does not stop the pass", "[contract]")
{
  reset ();
  g_fail_name = "dba.t1";
  g_fail_rc = ER_OBJ_INVALID_ARGUMENTS;
  Out o = run_h (0, NULL, 0);

  CHECK (o.rc == NO_ERROR);
  CHECK (hist_names () == "dba.t1,dba.t2,dba.tp,_db_class");
}

TEST_CASE ("class listing fails: ER_FAILED, auth restored", "[contract]")
{
  reset ();
  g_list_fails = true;
  Out o = run_h (0, NULL, 0);

  CHECK (o.rc == ER_FAILED);
  CHECK (g_hist.empty ());
  CHECK_FALSE (au_ctx ()->disable_auth_check);
}

/* ---- LIST_MOPS release (the PR base never released it) ---- */

TEST_CASE ("LIST_MOPS is released once when the pass completes", "[contract]")
{
  reset ();
  Out o = run_h (0, NULL, 0);

  CHECK (o.rc == NO_ERROR);
  REQUIRE (g_listed.size () == 1);
  REQUIRE (g_freed.size () == 1);
  CHECK (g_freed[0] == g_listed[0]);
}

TEST_CASE ("a histogram error stops the pass with that error; LIST_MOPS released, auth restored, no summary",
	   "[contract]")
{
  reset ();
  g_fail_name = "dba.t2";
  g_fail_rc = ER_FAILED;
  Out o = run_h (0, NULL, 0);

  CHECK (o.rc == ER_FAILED);
  CHECK (hist_names () == "dba.t1,dba.t2");
  CHECK_FALSE (au_ctx ()->disable_auth_check);
  CHECK (o.out.empty ());
  REQUIRE (g_listed.size () == 1);
  REQUIRE (g_freed.size () == 1);
  CHECK (g_freed[0] == g_listed[0]);
}

TEST_CASE ("a listed class that cannot be found again: its error; LIST_MOPS released, auth restored", "[contract]")
{
  reset ();
  g_nofind_name = "dba.t2";
  Out o = run_h (0, NULL, 0);

  CHECK (o.rc == ER_LC_UNKNOWN_CLASSNAME);
  CHECK (hist_names () == "dba.t1");
  CHECK_FALSE (au_ctx ()->disable_auth_check);
  REQUIRE (g_listed.size () == 1);
  REQUIRE (g_freed.size () == 1);
  CHECK (g_freed[0] == g_listed[0]);
}

/* ---- the skip list (new code) ---- */

TEST_CASE ("skip [t2]: t2 gets no histogram, the summary says (1 excluded), no warning", "[contract][new]")
{
  NEW_CODE_ONLY ();
  reset ();
  MOP skip[1] = { mop_of ("dba.t2") };
  Out o = run_h (0, skip, 1);

  CHECK (o.rc == NO_ERROR);
  CHECK (hist_names () == "dba.t1,dba.tp,_db_class");
  CHECK (o.out == "Statistics updated successfully: 3 tables, 4 columns (1 excluded).\n");
  CHECK (o.err.empty ());
  CHECK (g_freed.size () == 1);
}

TEST_CASE ("skip by OID: a different MOP object carrying t2's OID still excludes t2", "[contract][new]")
{
  NEW_CODE_ONLY ();
  reset ();
  DB_OBJECT other;
  memset (&other, 0, sizeof (other));
  other.oid_info.oid = mop_of ("dba.t2")->oid_info.oid;
  MOP skip[1] = { &other };
  Out o = run_h (0, skip, 1);

  CHECK (o.rc == NO_ERROR);
  CHECK (hist_names () == "dba.t1,dba.tp,_db_class");
  CHECK (o.err.empty ());
}

TEST_CASE ("skip a NULL entry: ignored, nothing excluded", "[contract][new]")
{
  NEW_CODE_ONLY ();
  reset ();
  MOP skip[2] = { NULL, mop_of ("dba.t1") };
  Out o = run_h (0, skip, 2);

  CHECK (o.rc == NO_ERROR);
  CHECK (hist_names () == "dba.t2,dba.tp,_db_class");
}

TEST_CASE ("skip a partition: not a top-level class -> singular warning on stderr, every class built",
	   "[contract][new]")
{
  NEW_CODE_ONLY ();
  reset ();
  MOP skip[1] = { mop_of ("dba.tp__p__p0") };
  Out o = run_h (0, skip, 1);

  CHECK (o.rc == NO_ERROR);
  CHECK (hist_names () == "dba.t1,dba.t2,dba.tp,_db_class");
  CHECK (o.err == "Warning: 1 of the 1 classes to exclude is not a top-level class and was ignored.\n");
  CHECK (o.out == "Statistics updated successfully: 4 tables, 7 columns.\n");
}

TEST_CASE ("skip two non-top-level entries and one class: plural warning, (1 excluded)", "[contract][new]")
{
  NEW_CODE_ONLY ();
  reset ();
  MOP skip[3] = { mop_of ("dba.tp__p__p0"), mop_of ("dba.t1"), mop_of ("dba.v1") };
  Out o = run_h (0, skip, 3);

  CHECK (o.rc == NO_ERROR);
  CHECK (hist_names () == "dba.t2,dba.tp,_db_class");
  CHECK (o.err == "Warning: 2 of the 3 classes to exclude are not top-level classes and were ignored.\n");
  CHECK (o.out == "Statistics updated successfully: 3 tables, 5 columns (1 excluded).\n");
}

TEST_CASE ("skip every top-level class: no histogram built, the summary still printed with (4 excluded)",
	   "[contract][new]")
{
  NEW_CODE_ONLY ();
  reset ();
  MOP skip[4] = { mop_of ("_db_class"), mop_of ("dba.tp"), mop_of ("dba.t2"), mop_of ("dba.t1") };
  Out o = run_h (0, skip, 4);

  CHECK (o.rc == NO_ERROR);
  CHECK (g_hist.empty ());
  CHECK (o.out == "Statistics updated successfully: 0 tables, 0 columns (4 excluded).\n");
  CHECK (o.err.empty ());
  CHECK (g_freed.size () == 1);
}

TEST_CASE ("PR8166 F1 (latent): the same class twice in skip_mops is reported as 'not a top-level class' "
	   "(PASS = defect; optimizedb de-duplicates before calling, so not reachable today)", "[defect]")
{
  NEW_CODE_ONLY ();
  reset ();
  MOP skip[2] = { mop_of ("dba.t1"), mop_of ("dba.t1") };
  Out o = run_h (0, skip, 2);

  CHECK (o.rc == NO_ERROR);
  CHECK (hist_names () == "dba.t2,dba.tp,_db_class");
  /* t1 is a top-level class and was excluded; the warning counts entries, not distinct classes */
  CHECK (o.err == "Warning: 1 of the 2 classes to exclude is not a top-level class and was ignored.\n");
}
