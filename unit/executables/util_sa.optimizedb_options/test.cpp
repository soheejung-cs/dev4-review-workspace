/*
 * PR#8166 (CBRD-27249) -- optimizedb --no-histogram / --no-histogram-classes=NAME[,NAME...]
 *
 * Unit under test: src/executables/util_sa.c (SA_MODE) -- optimizedb () run end to end, and through its -n
 * path the static optimizedb_update_class_statistics (). Everything optimizedb () calls lives in another
 * TU, so every callee below is a fake written after the real function's branch that this path takes (see
 * the comment on each). The database is never started: db_restart / db_shutdown only count.
 *
 * What is checked: option parsing (-n, --no-histogram, --no-histogram-classes and their combinations, the
 * list syntax: commas, empty entries, blanks, case, owner prefix, dba system classes, name length limits,
 * repeated entries counted once), what reaches the statistics calls (sm_update_all_statistics arguments,
 * the single-class histogram/statistics order), db_shutdown exactly once after a successful db_restart on
 * every exit, and the two buffers optimizedb () allocates (names_buf, class_mops) -- malloc/free are
 * interposed for the duration of the optimizedb () call, so any allocation it leaves behind is a leak,
 * including when an allocation is made to fail.
 *
 * Cases for the options this PR adds are compiled only where utility.h defines OPTIMIZE_NO_HISTOGRAM_S
 * (absent on the PR base: new code, no base comparison).
 */
#include "catch.hpp"
#include "config.h"

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <unistd.h>

#include "error_code.h"
#include "utility.h"
#include "db.h"
#include "authenticate.h"
#include "schema_manager.h"
#include "schema_system_catalog.hpp"
#include "execute_schema.h"
#include "work_space.h"
#include "statistics.h"
#include "message_catalog.h"
#include "util_func.h"

/* ------------------------------------------------------------------------------------------------ */
/* allocation tracking: malloc/free interposed (glibc forwards strdup's malloc through the same symbol) */

extern "C" void *__libc_malloc (size_t);
extern "C" void __libc_free (void *);

static bool g_track;		/* true only while optimizedb () runs (fakes switch it off, see Untracked) */
static int g_allocs;		/* allocations made while tracked */
static int g_fail_at;		/* 1-based: the tracked allocation that returns NULL (0 = none) */
static void *g_live[512];	/* tracked allocations not yet freed */
static int g_live_n;

extern "C" void *
malloc (size_t n) __THROW
{
  if (!g_track)
    {
      return __libc_malloc (n);
    }
  g_allocs++;
  if (g_fail_at == g_allocs)
    {
      return NULL;
    }
  void *p = __libc_malloc (n);
  if (p != NULL && g_live_n < 512)
    {
      g_live[g_live_n++] = p;
    }
  return p;
}

extern "C" void
free (void *p) __THROW
{
  if (p != NULL)
    {
      for (int i = 0; i < g_live_n; i++)
	{
	  if (g_live[i] == p)
	    {
	      g_live[i] = g_live[--g_live_n];
	      break;
	    }
	}
    }
  __libc_free (p);
}

struct Untracked		/* fakes record into std containers; that is the test's memory, not optimizedb's */
{
  bool saved;
  Untracked () : saved (g_track) { g_track = false; }
  ~Untracked () { g_track = saved; }
};

/* ------------------------------------------------------------------------------------------------ */
/* the fake catalog */

struct FakeClass
{
  const char *name;		/* as the catalog stores it: owner-qualified lower case, system classes bare */
  int is_class;			/* db_is_class (): 1 class (SM_CLASS_CT, partitions too), 0 view */
  DB_OBJECT mop;
};

static FakeClass g_cat[] = {
  {"dba.t1", 1, {}},
  {"dba.t2", 1, {}},
  {"public.t3", 1, {}},
  {"dba.v1", 0, {}},
  {"dba.a,b", 1, {}},		/* create table [a,b] */
  {"_db_class", 1, {}},		/* dba system class (bare name) */
  {"db_class", 0, {}},		/* dba system view */
  {"dba.tp", 1, {}},		/* partitioned class */
  {"dba.tp__p__p0", 1, {}},	/* its partition: a class, not a top-level one */
  {NULL, 0, {}},		/* [9]  dba.<222 x>            filled in by catalog_init () */
  {NULL, 0, {}},		/* [10] <31 o>.t               filled in by catalog_init () */
};
static const int g_ncat = sizeof (g_cat) / sizeof (g_cat[0]);
static std::string g_long_cls_name;	/* "dba." + 222 'x' : class part at the limit (< 223) */
static std::string g_long_own_name;	/* 31 'o' + ".t"     : owner at the limit (< 32) */

static struct
{
  const char *syn, *target;
} g_syn[] = { {"dba.syn_t1", "dba.t1"} };

static const char *g_sys_names[] = { "_db_class", "db_class", "_db_attribute", "db_attribute", "db_serial" };

static void
catalog_init ()
{
  static bool done;
  if (done)
    {
      return;
    }
  done = true;
  g_long_cls_name = "dba." + std::string (222, 'x');
  g_long_own_name = std::string (31, 'o') + ".t";
  g_cat[9].name = g_long_cls_name.c_str ();
  g_cat[9].is_class = 1;
  g_cat[10].name = g_long_own_name.c_str ();
  g_cat[10].is_class = 1;
  for (int i = 0; i < g_ncat; i++)
    {
      memset (&g_cat[i].mop, 0, sizeof (DB_OBJECT));
      g_cat[i].mop.oid_info.oid.volid = 0;
      g_cat[i].mop.oid_info.oid.pageid = 100;
      g_cat[i].mop.oid_info.oid.slotid = (short) (i + 1);
    }
}

static DB_OBJECT *
cat_mop (const char *name)
{
  for (int i = 0; i < g_ncat; i++)
    {
      if (g_cat[i].name != NULL && strcmp (g_cat[i].name, name) == 0)
	{
	  return &g_cat[i].mop;
	}
    }
  return NULL;
}

static const FakeClass *
cat_of (DB_OBJECT * mop)
{
  for (int i = 0; i < g_ncat; i++)
    {
      if (&g_cat[i].mop == mop)
	{
	  return &g_cat[i];
	}
    }
  return NULL;
}

/* ------------------------------------------------------------------------------------------------ */
/* recorders */

struct AllCall
{
  bool with_fullscan;
  int random_seed;
  int no_histogram;
  int arity;			/* 3 = PR base signature, 5 = with the skip list */
  std::vector<DB_OBJECT *> skip;
  int n_skip;
};

struct HistCall
{
  DB_OBJECT *obj;
  int bucket_count;
  bool with_fullscan;
  int random_seed;
  void *target_columns;
  DO_HISTOGRAM what;
  bool quiet;
  bool auth_disabled;		/* Au_disable while the helper ran */
};

static std::vector<AllCall> g_all;
static std::vector<DB_OBJECT *> g_upd;	/* sm_update_statistics */
static std::vector<HistCall> g_hist;
static std::vector<std::string> g_find;	/* db_find_class arguments */
static std::vector<std::string> g_order;	/* "hist:<name>" / "upd:<name>" in call order */
static int g_restart, g_shutdown, g_commit, g_login;
static int g_restart_rc, g_all_rc, g_upd_rc, g_hist_rc;
static int g_last_er;

alignas (authenticate_context) static unsigned char g_au_storage[sizeof (authenticate_context)];

static void
reset_recorders ()
{
  g_all.clear ();
  g_upd.clear ();
  g_hist.clear ();
  g_find.clear ();
  g_order.clear ();
  g_restart = g_shutdown = g_commit = g_login = 0;
  g_restart_rc = g_all_rc = g_upd_rc = g_hist_rc = NO_ERROR;
  g_last_er = NO_ERROR;
  memset (g_au_storage, 0, sizeof (g_au_storage));
}

/* ------------------------------------------------------------------------------------------------ */
/* fakes: util_common.c -- bodies copied (message printing reduced to one stderr line) */

static int
fake_option_index (UTIL_ARG_MAP * arg_map, int arg_ch)
{
  for (int i = 0; arg_map[i].arg_ch; i++)
    {
      if (arg_map[i].arg_ch == arg_ch)
	{
	  return i;
	}
    }
  return -1;
}

bool
utility_get_option_bool_value (UTIL_ARG_MAP * arg_map, int arg_ch)
{
  int index = fake_option_index (arg_map, arg_ch);
  return index != -1 && arg_map[index].arg_value.i == 1;
}

char *
utility_get_option_string_value (UTIL_ARG_MAP * arg_map, int arg_ch, int index)
{
  int arg_index = fake_option_index (arg_map, arg_ch);
  if (arg_index != -1)
    {
      if (arg_ch == OPTION_STRING_TABLE)
	{
	  if (index < arg_map[arg_index].value_info.num_strings)
	    {
	      return (((char **) arg_map[arg_index].arg_value.p)[index]);
	    }
	}
      else
	{
	  return ((char *) arg_map[arg_index].arg_value.p);
	}
    }
  return NULL;
}

int
utility_get_option_string_table_size (UTIL_ARG_MAP * arg_map)
{
  int arg_index = fake_option_index (arg_map, OPTION_STRING_TABLE);
  return (arg_index != -1) ? arg_map[arg_index].value_info.num_strings : 0;
}

/* check_database_name_local (name, EXISTING_DATABASE) */
int
check_database_name (const char *name)
{
  if (name[0] == '#')
    {
      return ER_GENERIC_ERROR;
    }
  for (int i = 0; name[i] != 0; i++)
    {
      if (isspace (name[i]) || name[i] == '/' || name[i] == '\\' || !isprint (name[i]))
	{
	  return ER_GENERIC_ERROR;
	}
    }
  return NO_ERROR;
}

/* utility_check_class_name: empty, total >= 255, no dot, empty owner, owner >= 32, empty class, class >= 223 */
int
utility_check_class_name (const char *class_name)
{
  Untracked u;
  const char *why = NULL;
  const char *dot;

  if (class_name == NULL || class_name[0] == '\0')
    {
      why = "invalid argument";
    }
  else if ((int) strlen (class_name) >= SM_MAX_IDENTIFIER_LENGTH)
    {
      why = "name too long";
    }
  else if ((dot = strchr (class_name, '.')) == NULL || dot - class_name < 1)
    {
      why = "invalid format";
    }
  else if (dot - class_name >= SM_MAX_USER_LENGTH)
    {
      why = "owner too long";
    }
  else if (strlen (dot + 1) < 1)
    {
      why = "invalid format";
    }
  else if ((int) strlen (dot + 1) >= SM_MAX_IDENTIFIER_LENGTH - SM_MAX_USER_LENGTH)
    {
      why = "class name too long";
    }
  if (why != NULL)
    {
      fprintf (stderr, "check_class_name: %s\n", why);
      return ER_FAILED;
    }
  return NO_ERROR;
}

/* ------------------------------------------------------------------------------------------------ */
/* fakes: schema_manager.c sm_qualifier_name / sm_remove_qualifier_name -- bodies copied (asserts dropped, NDEBUG) */

char *
sm_qualifier_name (const char *name, char *buf, int buf_size)
{
  const char *dot;
  int len;

  if (name == NULL || name[0] == '\0')
    {
      buf[0] = '\0';
      return NULL;
    }
  dot = strchr (name, '.');
  if (dot == NULL)
    {
      buf[0] = '\0';
      return NULL;
    }
  len = (int) (dot - name);
  if (len >= SM_MAX_USER_LENGTH)
    {
      buf[0] = '\0';
      return NULL;
    }
  memcpy (buf, name, len);
  buf[len] = '\0';
  return buf;
}

const char *
sm_remove_qualifier_name (const char *name)
{
  if (name == NULL || name[0] == '\0')
    {
      return NULL;
    }
  const char *dot = strchr (name, '.');
  return dot ? (dot + 1) : name;
}

/* schema_system_catalog.cpp: identifier check (names starting with "_db" skip it) && case-insensitive set lookup */
bool
sm_check_system_class_by_name (const std::string_view name)
{
  if (strncasecmp (name.data (), "_db", 3) != 0)
    {
      for (size_t i = 0; i < name.size (); i++)
	{
	  if ((i == 0 && !isalpha ((unsigned char) name[0]))
	      || (i >= 1 && !isalnum ((unsigned char) name[i]) && name[i] != '_' && name[i] != '.'))
	    {
	      return false;
	    }
	}
    }
  for (const char *s : g_sys_names)
    {
      if (strlen (s) == name.size () && strncasecmp (s, name.data (), name.size ()) == 0)
	{
	  return true;
	}
    }
  return false;
}

/* ------------------------------------------------------------------------------------------------ */
/* fakes: db_info.c / schema_manager.c class lookup.
 * db_find_class -> sm_find_class_with_purpose: sm_user_specified_name (owner-qualified -> lower case as is;
 * >= 223 bytes or a system class name -> lower case bare; otherwise "<current schema>." + name, the
 * current schema being DBA after db_login ("DBA")), locator lookup, then a synonym of that name. */

DB_OBJECT *
db_find_class (const char *name)
{
  Untracked u;
  std::string real;

  g_find.push_back (name ? name : "(null)");
  if (name == NULL || name[0] == '\0')
    {
      g_last_er = ER_OBJ_INVALID_ARGUMENTS;
      return NULL;
    }
  if (strchr (name, '.') != NULL || strlen (name) >= SM_MAX_IDENTIFIER_LENGTH - SM_MAX_USER_LENGTH
      || sm_check_system_class_by_name (name))
    {
      real = name;
    }
  else
    {
      real = std::string ("dba.") + name;
    }
  for (char &c : real)
    {
      c = (char) tolower ((unsigned char) c);
    }
  DB_OBJECT *mop = cat_mop (real.c_str ());
  if (mop != NULL)
    {
      return mop;
    }
  for (auto &s : g_syn)
    {
      if (real == s.syn)
	{
	  return cat_mop (s.target);
	}
    }
  g_last_er = ER_LC_UNKNOWN_CLASSNAME;
  return NULL;
}

/* db_is_class: NULL -> 0; a class (SM_CLASS_CT) -> 1; a view -> 0 */
int
db_is_class (DB_OBJECT * obj)
{
  const FakeClass *c = cat_of (obj);
  return (c != NULL) ? c->is_class : 0;
}

/* ------------------------------------------------------------------------------------------------ */
/* fakes: session, errors, messages */

authenticate_context *
au_ctx (void)
{
  return reinterpret_cast < authenticate_context * >(g_au_storage);
}

void
authenticate_context::disable_passwords (void)
{
}

int
er_init (const char *msglog_filename, int exit_ask)
{
  return NO_ERROR;
}

void
er_set (int severity, const char *file_name, const int line_no, int err_id, int num_args, ...)
{
  g_last_er = err_id;
}

int
er_errid (void)
{
  return g_last_er;
}

void
db_set_client_type (int client_type)
{
}

int
db_login (const char *name, const char *password)
{
  g_login++;
  return NO_ERROR;
}

int
db_restart (const char *program, int print_version, const char *volume)
{
  g_restart++;
  return g_restart_rc;
}

int
db_shutdown (void)
{
  g_shutdown++;
  return NO_ERROR;
}

int
db_commit_transaction (void)
{
  g_commit++;
  return NO_ERROR;
}

const char *
db_error_string (int level)
{
  return "fake error";
}

int
util_log_write_errid (int message_id, ...)
{
  return 0;
}

int
util_log_write_errstr (const char *format, ...)
{
  return 0;
}

char *
msgcat_message (int cat_id, int set_id, int msg_id)
{
  static char usage[] = "usage: %s optimizedb [OPTION] database-name\n";
  static char other[] = "message\n";
  return (set_id == MSGCAT_UTIL_SET_OPTIMIZEDB && msg_id == OPTIMIZEDB_MSG_USAGE) ? usage : other;
}

/* ------------------------------------------------------------------------------------------------ */
/* fakes: the statistics calls (schema_manager.c, execute_schema.c) -- record and return the set code */

int
sm_update_all_statistics (bool with_fullscan, int random_seed, int no_histogram, MOP * skip, int n_skip)
{
  Untracked u;
  AllCall c{with_fullscan, random_seed, no_histogram, 5, {}, n_skip};
  for (int i = 0; skip != NULL && i < n_skip; i++)
    {
      c.skip.push_back (skip[i]);
    }
  g_all.push_back (c);
  return g_all_rc;
}

int
sm_update_all_statistics (bool with_fullscan, int random_seed, int no_histogram)	/* PR base signature */
{
  Untracked u;
  g_all.push_back (AllCall{with_fullscan, random_seed, no_histogram, 3, {}, 0});
  return g_all_rc;
}

int
sm_update_statistics (MOP classop, bool with_fullscan, struct class_attr_ndv *provided_ndv)
{
  Untracked u;
  g_upd.push_back (classop);
  const FakeClass *c = cat_of (classop);
  g_order.push_back (std::string ("upd:") + (c ? c->name : "?"));
  return g_upd_rc;
}

int
update_or_drop_histogram_helper (PARSER_CONTEXT * parser, DB_OBJECT * const obj, bool quiet,
				 PT_HISTOGRAM_INFO * const info, DO_HISTOGRAM do_histogram, int *out_skipped)
{
  Untracked u;
  g_hist.push_back (HistCall{obj, info->bucket_count, info->with_fullscan, info->random_seed,
			     (void *) info->target_columns, do_histogram, quiet, au_ctx ()->disable_auth_check});
  const FakeClass *c = cat_of (obj);
  g_order.push_back (std::string ("hist:") + (c ? c->name : "?"));
  if (out_skipped != NULL)
    {
      *out_skipped = 0;
    }
  return g_hist_rc;
}

/* ------------------------------------------------------------------------------------------------ */
/* driver */

struct Opt
{
  const char *n = NULL;		/* -n value (NULL = not given) */
  bool no_histogram = false;	/* --no-histogram */
  const char *list = NULL;	/* --no-histogram-classes value (NULL = not given) */
  int n_db = 1;			/* positional arguments */
  const char *db = "testdb";
  /* injected results */
  int restart_rc = NO_ERROR;	/* db_restart */
  int all_rc = NO_ERROR;	/* sm_update_all_statistics */
  int upd_rc = NO_ERROR;	/* sm_update_statistics */
  int hist_rc = NO_ERROR;	/* update_or_drop_histogram_helper */
  int fail_at = 0;		/* 1-based tracked allocation to fail */
};

struct Run
{
  int rc;
  int allocs;			/* allocations optimizedb () made */
  int leaks;			/* of those, not freed when it returned */
  std::string err;		/* what it wrote to stderr */
};

static Run
run_opt (const Opt & o)
{
  catalog_init ();
  reset_recorders ();
  g_restart_rc = o.restart_rc;
  g_all_rc = o.all_rc;
  g_upd_rc = o.upd_rc;
  g_hist_rc = o.hist_rc;
  g_fail_at = o.fail_at;

  static char db[64], db2[64], argv0[64];
  static char *dbs[2] = { db, db2 };
  static char *argv[2] = { argv0, NULL };
  snprintf (db, sizeof (db), "%s", o.db);
  snprintf (db2, sizeof (db2), "otherdb");
  snprintf (argv0, sizeof (argv0), "/opt/cubrid/bin/cub_admin");

  UTIL_ARG_MAP map[] = {
    {OPTION_STRING_TABLE, {0}, {0}},
    {OPTIMIZE_CLASS_NAME_S, {ARG_STRING}, {0}},
#if defined (OPTIMIZE_NO_HISTOGRAM_S)
    {OPTIMIZE_NO_HISTOGRAM_S, {ARG_BOOLEAN}, {0}},
    {OPTIMIZE_NO_HISTOGRAM_CLASSES_S, {ARG_STRING}, {0}},
#endif
    {0, {0}, {0}}
  };
  map[0].value_info.num_strings = o.n_db;
  map[0].arg_value.p = dbs;
  map[1].arg_value.p = (void *) o.n;
#if defined (OPTIMIZE_NO_HISTOGRAM_S)
  map[2].arg_value.i = o.no_histogram ? 1 : 0;
  map[3].arg_value.p = (void *) o.list;
#else
  REQUIRE_FALSE (o.no_histogram);
  REQUIRE (o.list == NULL);
#endif

  UTIL_FUNCTION_ARG arg;
  arg.arg_map = map;
  arg.command_name = "optimizedb";
  arg.argv0 = argv0;
  arg.argv = argv;
  arg.valid_arg = true;

  /* stderr -> a temp file (set up before tracking starts) */
  fflush (stderr);
  int saved = dup (2);
  FILE *tf = tmpfile ();
  dup2 (fileno (tf), 2);

  g_allocs = 0;
  g_live_n = 0;
  g_track = true;
  int rc = optimizedb (&arg);
  g_track = false;

  fflush (stderr);
  dup2 (saved, 2);
  close (saved);
  std::string err;
  rewind (tf);
  char buf[512];
  size_t k;
  while ((k = fread (buf, 1, sizeof (buf), tf)) > 0)
    {
      err.append (buf, k);
    }
  fclose (tf);

  Run r{rc, g_allocs, g_live_n, err};
  g_fail_at = 0;
  return r;
}

static bool
contains (const std::string & s, const char *what)
{
  return s.find (what) != std::string::npos;
}

static bool
is_usage (const Run & r)
{
  return contains (r.err, "usage: cub_admin optimizedb");
}

/* after a successful db_restart every exit shuts the database down exactly once; before it, never */
#define CHECK_SESSION_CLOSED() \
  do { CHECK (g_shutdown == g_restart); CHECK (g_restart <= 1); } while (0)

/* ================================================================================================ */
/* present on the PR base and on head                                                               */
/* ================================================================================================ */

TEST_CASE ("no option: all classes, histograms included, nothing allocated", "[contract]")
{
  Run r = run_opt (Opt ());

  CHECK (r.rc == EXIT_SUCCESS);
  REQUIRE (g_all.size () == 1);
  CHECK (g_all[0].with_fullscan == STATS_WITH_SAMPLING);
  CHECK (g_all[0].random_seed == 0);
  CHECK (g_all[0].no_histogram == 0);
  CHECK (g_all[0].skip.empty ());
  CHECK (g_all[0].n_skip == 0);
  CHECK (g_upd.empty ());
  CHECK (g_hist.empty ());
  CHECK (g_commit == 1);
  CHECK_SESSION_CLOSED ();
  CHECK (r.allocs == 0);
  CHECK (r.leaks == 0);
}

TEST_CASE ("-n dba.t1: only that class is updated, the all-classes call is not made", "[contract]")
{
  Run r = run_opt (Opt { "dba.t1" });

  CHECK (r.rc == EXIT_SUCCESS);
  CHECK (g_all.empty ());
  /* base: sm_update_statistics; head: the all-columns histogram build (which refreshes the statistics too) */
  REQUIRE (g_order.size () == 1);
  CHECK ((g_order[0] == "upd:dba.t1" || g_order[0] == "hist:dba.t1"));
  CHECK (g_commit == 1);
  CHECK_SESSION_CLOSED ();
  CHECK (r.leaks == 0);
}

TEST_CASE ("-n 'dba.a,b': a comma inside the -n value is part of one identifier, not a list", "[contract]")
{
  Run r = run_opt (Opt { "dba.a,b" });

  CHECK (r.rc == EXIT_SUCCESS);
  CHECK_FALSE (is_usage (r));
  REQUIRE (g_find.size () == 1);
  CHECK (g_find[0] == "dba.a,b");
  REQUIRE (g_order.size () == 1);
  CHECK ((g_order[0] == "upd:dba.a,b" || g_order[0] == "hist:dba.a,b"));
  CHECK_SESSION_CLOSED ();
  CHECK (r.leaks == 0);
}

TEST_CASE ("-n DBA._DB_CLASS: dba's system class is looked up by its bare name, case-insensitively", "[contract]")
{
  Run r = run_opt (Opt { "DBA._DB_CLASS" });

  CHECK (r.rc == EXIT_SUCCESS);
  REQUIRE (g_find.size () == 1);
  CHECK (g_find[0] == "_DB_CLASS");
  REQUIRE (g_order.size () == 1);
  CHECK ((g_order[0] == "upd:_db_class" || g_order[0] == "hist:_db_class"));
  CHECK_SESSION_CLOSED ();
}

TEST_CASE ("-n PUBLIC.T3: an upper-case non-dba owner is passed through and found", "[contract]")
{
  Run r = run_opt (Opt { "PUBLIC.T3" });

  CHECK (r.rc == EXIT_SUCCESS);
  REQUIRE (g_find.size () == 1);
  CHECK (g_find[0] == "PUBLIC.T3");
  REQUIRE (g_order.size () == 1);
  CHECK ((g_order[0] == "upd:public.t3" || g_order[0] == "hist:public.t3"));
}

TEST_CASE ("-n unknown class: EXIT_FAILURE, nothing updated, session closed, no leak", "[contract]")
{
  Run r = run_opt (Opt { "dba.nosuch" });

  CHECK (r.rc == EXIT_FAILURE);
  CHECK (g_all.empty ());
  CHECK (g_order.empty ());
  CHECK (g_commit == 0);
  CHECK_SESSION_CLOSED ();
  CHECK (r.leaks == 0);
}

TEST_CASE ("-n on a view: nothing to update, EXIT_SUCCESS (the old lenient behaviour)", "[contract]")
{
  Run r = run_opt (Opt { "dba.v1" });

  CHECK (r.rc == EXIT_SUCCESS);
  CHECK (g_all.empty ());
  CHECK (g_order.empty ());
  CHECK_SESSION_CLOSED ();
  CHECK (r.leaks == 0);
}

TEST_CASE ("-n '': an empty -n means all classes", "[contract]")
{
  Run r = run_opt (Opt { "" });

  CHECK (r.rc == EXIT_SUCCESS);
  REQUIRE (g_all.size () == 1);
  CHECK (g_all[0].no_histogram == 0);
  CHECK (g_order.empty ());
  CHECK (r.leaks == 0);
}

TEST_CASE ("-n name length limits: owner 31 and class part 222 bytes pass; 32 / 223 are rejected", "[contract]")
{
  catalog_init ();
  SECTION ("class part 222 bytes")
  {
    Run r = run_opt (Opt { g_long_cls_name.c_str () });
    CHECK (r.rc == EXIT_SUCCESS);
    CHECK (g_order.size () == 1);
  }
  SECTION ("owner 31 bytes")
  {
    Run r = run_opt (Opt { g_long_own_name.c_str () });
    CHECK (r.rc == EXIT_SUCCESS);
    CHECK (g_order.size () == 1);
  }
  SECTION ("class part 223 bytes")
  {
    std::string n = "dba." + std::string (223, 'x');
    Run r = run_opt (Opt { n.c_str () });
    CHECK (r.rc == EXIT_FAILURE);
    CHECK (contains (r.err, "class name too long"));
    CHECK (g_order.empty ());
    CHECK_SESSION_CLOSED ();
    CHECK (r.leaks == 0);
  }
  SECTION ("owner 32 bytes")
  {
    std::string n = std::string (32, 'o') + ".t";
    Run r = run_opt (Opt { n.c_str () });
    CHECK (r.rc == EXIT_FAILURE);
    CHECK (contains (r.err, "owner too long"));
    CHECK (g_order.empty ());
    CHECK_SESSION_CLOSED ();
    CHECK (r.leaks == 0);
  }
  SECTION ("no owner")
  {
    Run r = run_opt (Opt { "t1" });
    CHECK (r.rc == EXIT_FAILURE);
    CHECK (contains (r.err, "invalid format"));
    CHECK (g_order.empty ());
    CHECK_SESSION_CLOSED ();
  }
}

TEST_CASE ("two database names: usage, the database is never started", "[contract]")
{
  Opt o;
  o.n_db = 2;
  Run r = run_opt (o);

  CHECK (r.rc == EXIT_FAILURE);
  CHECK (is_usage (r));
  CHECK (g_restart == 0);
  CHECK (g_shutdown == 0);
  CHECK (r.leaks == 0);
}

TEST_CASE ("db_restart fails: EXIT_FAILURE, no shutdown, nothing updated", "[contract]")
{
  Opt o;
  o.restart_rc = ER_FAILED;
  Run r = run_opt (o);

  CHECK (r.rc == EXIT_FAILURE);
  CHECK (g_restart == 1);
  CHECK (g_shutdown == 0);
  CHECK (g_all.empty ());
  CHECK (r.leaks == 0);
}

TEST_CASE ("bad database name: EXIT_FAILURE before the database is started", "[contract]")
{
  Opt o;
  o.db = "#bad";
  Run r = run_opt (o);

  CHECK (r.rc == EXIT_FAILURE);
  CHECK (g_restart == 0);
  CHECK (g_all.empty ());
}

TEST_CASE ("statistics call fails: EXIT_FAILURE, session closed, nothing committed", "[contract]")
{
  SECTION ("all classes")
  {
    Opt o;
    o.all_rc = ER_FAILED;
    Run r = run_opt (o);
    CHECK (r.rc == EXIT_FAILURE);
    CHECK (g_all.size () == 1);
    CHECK (g_commit == 0);
    CHECK_SESSION_CLOSED ();
    CHECK (r.leaks == 0);
  }
  SECTION ("-n, both the histogram build and the plain update fail")
  {
    Opt o;
    o.n = "dba.t1";
    o.upd_rc = ER_FAILED;
    o.hist_rc = ER_FAILED;
    Run r = run_opt (o);
    CHECK (r.rc == EXIT_FAILURE);
    CHECK (g_commit == 0);
    CHECK_SESSION_CLOSED ();
    CHECK (r.leaks == 0);
  }
}

/* ================================================================================================ */
/* the options this PR adds (new code: not on the PR base)                                          */
/* ================================================================================================ */
#if defined (OPTIMIZE_NO_HISTOGRAM_S)

TEST_CASE ("--no-histogram: all classes with no_histogram set, no skip list", "[contract]")
{
  Opt o;
  o.no_histogram = true;
  Run r = run_opt (o);

  CHECK (r.rc == EXIT_SUCCESS);
  REQUIRE (g_all.size () == 1);
  CHECK (g_all[0].arity == 5);
  CHECK (g_all[0].no_histogram != 0);
  CHECK (g_all[0].n_skip == 0);
  CHECK (g_all[0].skip.empty ());
  CHECK (r.allocs == 0);
  CHECK_SESSION_CLOSED ();
}

TEST_CASE ("--no-histogram-classes=dba.t1,dba.t2: both MOPs reach sm_update_all_statistics in order, histograms on",
	   "[contract]")
{
  Opt o;
  o.list = "dba.t1,dba.t2";
  Run r = run_opt (o);

  CHECK (r.rc == EXIT_SUCCESS);
  REQUIRE (g_all.size () == 1);
  CHECK (g_all[0].no_histogram == 0);
  REQUIRE (g_all[0].n_skip == 2);
  REQUIRE (g_all[0].skip.size () == 2);
  CHECK (g_all[0].skip[0] == cat_mop ("dba.t1"));
  CHECK (g_all[0].skip[1] == cat_mop ("dba.t2"));
  CHECK (g_order.empty ());
  CHECK (g_commit == 1);
  CHECK_SESSION_CLOSED ();
  CHECK (r.allocs == 2);	/* names_buf + class_mops */
  CHECK (r.leaks == 0);
}

TEST_CASE ("--no-histogram-classes: one entry", "[contract]")
{
  Opt o;
  o.list = "dba.t2";
  Run r = run_opt (o);

  CHECK (r.rc == EXIT_SUCCESS);
  REQUIRE (g_all.size () == 1);
  REQUIRE (g_all[0].n_skip == 1);
  CHECK (g_all[0].skip[0] == cat_mop ("dba.t2"));
  CHECK (r.leaks == 0);
}

TEST_CASE ("--no-histogram-classes: the same class named again (case, synonym) is passed once", "[contract]")
{
  Opt o;
  o.list = "dba.t1,DBA.T1,dba.t2,dba.syn_t1,dba.t1";
  Run r = run_opt (o);

  CHECK (r.rc == EXIT_SUCCESS);
  REQUIRE (g_all.size () == 1);
  REQUIRE (g_all[0].n_skip == 2);
  CHECK (g_all[0].skip[0] == cat_mop ("dba.t1"));
  CHECK (g_all[0].skip[1] == cat_mop ("dba.t2"));
  CHECK (r.leaks == 0);
}

TEST_CASE ("--no-histogram-classes: case and dba system classes follow the -n rules", "[contract]")
{
  Opt o;
  o.list = "DBA._DB_CLASS,PUBLIC.T3";
  Run r = run_opt (o);

  CHECK (r.rc == EXIT_SUCCESS);
  REQUIRE (g_find.size () == 2);
  CHECK (g_find[0] == "_DB_CLASS");	/* dba's system class by its bare name */
  CHECK (g_find[1] == "PUBLIC.T3");
  REQUIRE (g_all.size () == 1);
  REQUIRE (g_all[0].n_skip == 2);
  CHECK (g_all[0].skip[0] == cat_mop ("_db_class"));
  CHECK (g_all[0].skip[1] == cat_mop ("public.t3"));
}

TEST_CASE ("--no-histogram-classes: a partition is accepted here (the not-top-level warning is the next layer's)",
	   "[contract]")
{
  Opt o;
  o.list = "dba.tp__p__p0";
  Run r = run_opt (o);

  CHECK (r.rc == EXIT_SUCCESS);
  REQUIRE (g_all.size () == 1);
  REQUIRE (g_all[0].n_skip == 1);
  CHECK (g_all[0].skip[0] == cat_mop ("dba.tp__p__p0"));
}

TEST_CASE ("--no-histogram-classes: an empty entry (a,,b / trailing / leading / lone comma) is a usage error",
	   "[contract]")
{
  const char *lists[] = { "dba.t1,,dba.t2", "dba.t1,", ",dba.t1", "," };
  for (const char *l : lists)
    {
      Opt o;
      o.list = l;
      Run r = run_opt (o);
      INFO ("list = '" << l << "'");
      CHECK (r.rc == EXIT_FAILURE);
      CHECK (is_usage (r));
      CHECK (g_all.empty ());
      CHECK (g_commit == 0);
      CHECK_SESSION_CLOSED ();
      CHECK (r.leaks == 0);
    }
}

TEST_CASE ("--no-histogram-classes='': usage before the database is started", "[contract]")
{
  Opt o;
  o.list = "";
  Run r = run_opt (o);

  CHECK (r.rc == EXIT_FAILURE);
  CHECK (is_usage (r));
  CHECK (g_restart == 0);
  CHECK (r.allocs == 0);
}

TEST_CASE ("--no-histogram-classes with -n, or with --no-histogram: usage before the database is started",
	   "[contract]")
{
  SECTION ("-n + list")
  {
    Opt o;
    o.n = "dba.t1";
    o.list = "dba.t2";
    Run r = run_opt (o);
    CHECK (r.rc == EXIT_FAILURE);
    CHECK (is_usage (r));
    CHECK (g_restart == 0);
    CHECK (r.allocs == 0);
  }
  SECTION ("--no-histogram + list")
  {
    Opt o;
    o.no_histogram = true;
    o.list = "dba.t2";
    Run r = run_opt (o);
    CHECK (r.rc == EXIT_FAILURE);
    CHECK (is_usage (r));
    CHECK (g_restart == 0);
    CHECK (r.allocs == 0);
  }
  SECTION ("-n + --no-histogram is allowed")
  {
    Opt o;
    o.n = "dba.t1";
    o.no_histogram = true;
    Run r = run_opt (o);
    CHECK (r.rc == EXIT_SUCCESS);
    CHECK_FALSE (is_usage (r));
  }
  SECTION ("-n '' + list: the empty -n counts as not given, the list runs")
  {
    Opt o;
    o.n = "";
    o.list = "dba.t2";
    Run r = run_opt (o);
    CHECK (r.rc == EXIT_SUCCESS);
    REQUIRE (g_all.size () == 1);
    CHECK (g_all[0].n_skip == 1);
  }
}

TEST_CASE ("--no-histogram-classes: blanks are not trimmed -- 'dba.t1, dba.t2' fails on ' dba.t2', nothing updated",
	   "[contract]")
{
  Opt o;
  o.list = "dba.t1, dba.t2";
  Run r = run_opt (o);

  CHECK (r.rc == EXIT_FAILURE);
  REQUIRE (g_find.size () == 2);
  CHECK (g_find[1] == " dba.t2");
  CHECK (g_all.empty ());	/* not silently dropped: the whole run stops */
  CHECK (g_commit == 0);
  CHECK_SESSION_CLOSED ();
  CHECK (r.leaks == 0);
}

TEST_CASE ("--no-histogram-classes: an unknown entry after a good one fails the run, nothing updated, no leak",
	   "[contract]")
{
  Opt o;
  o.list = "dba.t1,dba.nosuch";
  Run r = run_opt (o);

  CHECK (r.rc == EXIT_FAILURE);
  CHECK (g_all.empty ());
  CHECK (g_commit == 0);
  CHECK_SESSION_CLOSED ();
  CHECK (r.allocs == 2);
  CHECK (r.leaks == 0);
}

TEST_CASE ("--no-histogram-classes: a view is rejected ('not a class'), nothing updated", "[contract]")
{
  Opt o;
  o.list = "dba.t1,dba.v1";
  Run r = run_opt (o);

  CHECK (r.rc == EXIT_FAILURE);
  CHECK (contains (r.err, "dba.v1: not a class"));
  CHECK (g_all.empty ());
  CHECK_SESSION_CLOSED ();
  CHECK (r.leaks == 0);
}

TEST_CASE ("--no-histogram-classes: a name without owner or an invalid name fails the run", "[contract]")
{
  const char *lists[] = { "dba.t1,t2", "dba.t1,.t2", "dba.t1,dba.", "dba.t1, " };
  for (const char *l : lists)
    {
      Opt o;
      o.list = l;
      Run r = run_opt (o);
      INFO ("list = '" << l << "'");
      CHECK (r.rc == EXIT_FAILURE);
      CHECK (g_all.empty ());
      CHECK_SESSION_CLOSED ();
      CHECK (r.leaks == 0);
    }
}

TEST_CASE ("--no-histogram-classes: the length limits apply per entry, not to the whole list", "[contract]")
{
  catalog_init ();
  SECTION ("two names at the limits, list longer than 255 bytes")
  {
    std::string l = g_long_cls_name + "," + g_long_own_name + ",dba.t1";
    REQUIRE (l.size () > 255);
    Opt o;
    o.list = l.c_str ();
    Run r = run_opt (o);
    CHECK (r.rc == EXIT_SUCCESS);
    REQUIRE (g_all.size () == 1);
    CHECK (g_all[0].n_skip == 3);
    CHECK (r.leaks == 0);
  }
  SECTION ("one entry over the class-part limit")
  {
    std::string l = "dba.t1,dba." + std::string (223, 'x');
    Opt o;
    o.list = l.c_str ();
    Run r = run_opt (o);
    CHECK (r.rc == EXIT_FAILURE);
    CHECK (contains (r.err, "class name too long"));
    CHECK (g_all.empty ());
    CHECK (r.leaks == 0);
  }
  SECTION ("one entry over the owner limit")
  {
    std::string l = "dba.t1," + std::string (32, 'o') + ".t";
    Opt o;
    o.list = l.c_str ();
    Run r = run_opt (o);
    CHECK (r.rc == EXIT_FAILURE);
    CHECK (contains (r.err, "owner too long"));
    CHECK (g_all.empty ());
    CHECK (r.leaks == 0);
  }
}

TEST_CASE ("allocation failure (strdup or the MOP array): EXIT_FAILURE, session closed, the other buffer freed",
	   "[contract]")
{
  for (int fail = 1; fail <= 2; fail++)
    {
      Opt o;
      o.list = "dba.t1,dba.t2";
      o.fail_at = fail;
      Run r = run_opt (o);
      INFO ("failing allocation #" << fail);
      CHECK (r.rc == EXIT_FAILURE);
      CHECK (r.allocs == 2);
      CHECK (r.leaks == 0);
      CHECK (g_all.empty ());
      CHECK_SESSION_CLOSED ();
    }
  /* same on the -n path */
  for (int fail = 1; fail <= 2; fail++)
    {
      Opt o;
      o.n = "dba.t1";
      o.fail_at = fail;
      Run r = run_opt (o);
      INFO ("-n, failing allocation #" << fail);
      CHECK (r.rc == EXIT_FAILURE);
      CHECK (r.leaks == 0);
      CHECK (g_order.empty ());
      CHECK_SESSION_CLOSED ();
    }
}

TEST_CASE ("statistics call fails with a list: EXIT_FAILURE, session closed, both buffers freed", "[contract]")
{
  Opt o;
  o.list = "dba.t1";
  o.all_rc = ER_FAILED;
  Run r = run_opt (o);

  CHECK (r.rc == EXIT_FAILURE);
  CHECK (g_all.size () == 1);
  CHECK (g_commit == 0);
  CHECK_SESSION_CLOSED ();
  CHECK (r.allocs == 2);
  CHECK (r.leaks == 0);
}

/* -n: UPDATE STATISTICS ON <class> order (do_update_stats single-class branch) */

TEST_CASE ("-n dba.t1 (histograms on): the all-columns histogram build only, auth disabled around it",
	   "[contract]")
{
  Run r = run_opt (Opt { "dba.t1" });

  CHECK (r.rc == EXIT_SUCCESS);
  REQUIRE (g_hist.size () == 1);
  CHECK (g_hist[0].obj == cat_mop ("dba.t1"));
  CHECK (g_hist[0].what == DO_HISTOGRAM_CREATE);
  CHECK (g_hist[0].bucket_count == -1);
  CHECK (g_hist[0].with_fullscan == false);
  CHECK (g_hist[0].random_seed == 0);
  CHECK (g_hist[0].target_columns == NULL);
  CHECK (g_hist[0].quiet);
  CHECK (g_hist[0].auth_disabled);
  CHECK_FALSE (au_ctx ()->disable_auth_check);	/* restored */
  CHECK (g_upd.empty ());	/* the histogram scan refreshed the statistics */
}

TEST_CASE ("-n dba.t1 --no-histogram: sm_update_statistics only, no histogram build", "[contract]")
{
  Opt o;
  o.n = "dba.t1";
  o.no_histogram = true;
  Run r = run_opt (o);

  CHECK (r.rc == EXIT_SUCCESS);
  CHECK (g_hist.empty ());
  REQUIRE (g_upd.size () == 1);
  CHECK (g_upd[0] == cat_mop ("dba.t1"));
  CHECK (g_all.empty ());
}

TEST_CASE ("-n: the histogram build answering ER_OBJ_INVALID_ARGUMENTS falls back to sm_update_statistics",
	   "[contract]")
{
  Opt o;
  o.n = "dba.t1";
  o.hist_rc = ER_OBJ_INVALID_ARGUMENTS;
  Run r = run_opt (o);

  CHECK (r.rc == EXIT_SUCCESS);
  REQUIRE (g_order.size () == 2);
  CHECK (g_order[0] == "hist:dba.t1");
  CHECK (g_order[1] == "upd:dba.t1");
  CHECK_FALSE (au_ctx ()->disable_auth_check);
}

TEST_CASE ("-n: any other histogram error is the run's error, no fallback", "[contract]")
{
  Opt o;
  o.n = "dba.t1";
  o.hist_rc = ER_FAILED;
  Run r = run_opt (o);

  CHECK (r.rc == EXIT_FAILURE);
  REQUIRE (g_order.size () == 1);
  CHECK (g_order[0] == "hist:dba.t1");
  CHECK (g_commit == 0);
  CHECK_SESSION_CLOSED ();
  CHECK_FALSE (au_ctx ()->disable_auth_check);
  CHECK (r.leaks == 0);
}

#endif /* OPTIMIZE_NO_HISTOGRAM_S */
