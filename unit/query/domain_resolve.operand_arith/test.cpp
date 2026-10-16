/*
 * PR#8022 (CBRD-27510) -- how resolve_domains types an operator's operands before the first row
 * (src/query/domain_resolve.c, server mode; the type rules it applies are src/query/domain_rules.c, compiled into the
 * test binary as real code). Both files are new in the PR: the arithmetic contract is the row-time typed operators the
 * rule says it keeps (qdata_{add,subtract,multiply,divide}_dbval, query_opfunc.c at a9940d67a).
 *
 *   A1  number with number, + - * /: the result type of the typed operator (7 x 7 x 4), no operand converted
 *   A2  the one asymmetric number pair: FLOAT + BIGINT is DOUBLE, BIGINT + FLOAT is FLOAT (- * / are symmetric)
 *   A3  a NULL operand gives no value and no error, for every operator and operand type
 *   A4  string, date and other pairs: operand targets, the operator kind, the asymmetries the typed operators had
 *       (INT * DATE no value, DATE * INT -454), MOD
 *   A5  parameters: oracle_compat_number_behavior (INT / INT -> NUMERIC), return_null_on_function_errors,
 *       plus_as_concat
 *   A6  bind (late-binding) operands through qexec_resolve_late_bind_node: one plan, the bind types rotated -
 *       each execution resolves its own types, nothing kept from the previous one
 *   A7  a bind pair the operator rejects: -454 raised before any row, the entry holds NULL
 *   A8  string binds added (plus as concatenation): the resolved collation is the one db_string_concatenate gives
 *       the row, and a pair it rejects (-1150) resolves to NULL (the row raises it)
 *   A9  UNKNOWN: an operand whose domain is VARIABLE and has no value type leaves the node to a late binding,
 *       untouched; a literal and a compiled producer give their own domains (qexec_resolve_operand)
 *   A10 SUM / AVG over a late-bound argument: function and accumulator domains (DOUBLE for a string, NUMERIC widened,
 *       FLOAT accumulated in DOUBLE), a date or time rejected (-454) unless return_null_on_function_errors
 *
 * hooks.cpp exposes qexec_resolve_late_bind_node and qexec_resolve_operand (static in the TU).
 */
#include "catch.hpp"
/* first: mprec.h (via the sources below) defines cmp, a member name of DOMAIN_COMPARE */
#include "domain_rules.h"
#include "domain_plan.h"
#include "domain_rules.c"
#undef new
#include "system_parameter.c"
#undef new
#include "language_support.c"
#undef new
#include "intl_support.c"
#undef new
#include "chartype.c"
#undef new
#include "mprec.c"
#undef new
#include "dtoa.c"
#undef new
#include "object_primitive.c"
#undef new
#include "object_domain.c"
#undef new
#include "object_domain_convert.cpp"
#undef new
#include "numeric_opfunc.c"
#undef new
#include "db_date.c"
#undef new
#include "db_macro.c"
#undef new
#include "string_opfunc.c"
#undef new
#undef cmp
#include "boot_sr.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

/* ---------------------------------------------------------------- fakes: what a booted server process has */

/* system_parameter.c's static initializers (prm_Def's parallelism defaults, the regex engine keyword table) */
namespace cubthread
{
  std::size_t system_core_count (void)
  {
    return 8;
  }
}
namespace cubregex
{
  const char *get_engine_name (const engine_type & type)
  {
    return type == engine_type::LIB_RE2 ? "re2" : "cppstd";
  }
}

/* boot_sr.h BO_IS_SERVER_RESTARTED () reads this. DOWN makes prm_get_value (system_parameter.c:10829) read a
 * session parameter's global value - its default here - instead of a session's copy, which needs a thread entry */
BOOT_SERVER_STATUS boot_Server_status = BOOT_SERVER_DOWN;

/* environment_variable.c: no CUBRID_* variable is set (lang_init_builtin reads CUBRID_MSG_LANG) */
const char *
envvar_get (const char *)
{
  return NULL;
}

/* environment_variable.c:494 body */
void
envvar_trim_char (char *env_val, const int c)
{
  if (env_val == NULL)
    {
      return;
    }
  size_t size = strlen (env_val);
  if (*env_val == c && size > 2)
    {
      memmove (env_val, env_val + 1, size - 2);
      env_val[size - 2] = '\0';
    }
}

/* tp_init () before 5de66400a (#8104) names the deduplicate-key attributes; nothing under test reads them */
void
dk_deduplicate_key_attribute_initialized ()
{
}

/* error_manager.c: the last error id er_set raised, as er_errid () reads it; er_clear () resets it */
static int fake_er_id = NO_ERROR;
void
er_set (int, const char *, const int, int err_id, int, ...)
{
  fake_er_id = err_id;
}

int
er_errid (void)
{
  return fake_er_id;
}

void
er_clear (void)
{
  fake_er_id = NO_ERROR;
}

/* memory_alloc.c: the private heap of a thread is malloc here */
void *
db_private_alloc_release (THREAD_ENTRY *, size_t size, bool)
{
  return malloc (size);
}

void
db_private_free_release (THREAD_ENTRY *, void *ptr, bool)
{
  free (ptr);
}

void *
db_private_realloc_release (THREAD_ENTRY *, void *ptr, size_t size, bool)
{
  return realloc (ptr, size);
}

/* area_alloc.c: tp_init's domain area hands out zeroed element-sized blocks */
struct fake_area
{
  size_t element_size;
};
AREA *
area_create (const char *, size_t element_size, size_t)
{
  return (AREA *) new fake_area { element_size };
}

void *
area_alloc (AREA * area)
{
  return calloc (1, ((fake_area *) area)->element_size);
}

int
area_free (AREA *, void *ptr)
{
  free (ptr);
  return NO_ERROR;
}

/* The boot order of a server (boot_restart_server): built-in collations, the type module, then the database's
 * charset and language - en_US.utf8 here - which also gives the string domains their codeset (tp_apply_sys_charset) */
static bool
boot_types ()
{
  static bool done = false;
  static bool ok = false;
  if (!done)
    {
      done = true;
      lang_init_builtin ();
      ok = tp_init () == NO_ERROR;
      lang_Initialized = true;
      ok = ok && lang_set_charset_lang ("en_US.utf8") == NO_ERROR;
    }
  return ok;
}

/* ---------------------------------------------------------------- hooks.cpp (in domain_resolve.c) */
int unitseam_resolve_late_bind_node (const DOMAIN_PLAN * plan, int index, RESOLVED_DOMAIN_TABLE * resolved);
bool unitseam_resolve_operand (const DOMAIN_PLAN * plan, const RESOLVED_DOMAIN_TABLE * resolved, int link,
			       int arg_index, DOMAIN_CTX context, int opcode, DOMAIN_OPERAND * operand);

/* ---------------------------------------------------------------- helpers */

static const char *
type_name (DB_TYPE type)
{
  return pr_type_name (type);
}

/* a system parameter for one case, as a session would set it (prm_Def is system_parameter.c's, defaults loaded) */
struct scoped_bool_param
{
  PARAM_ID id;
  bool saved;
  scoped_bool_param (PARAM_ID param, bool value) : id (param), saved (prm_Def[param].value.v.b)
  {
    prm_Def[param].value.v.b = value;
  }
  ~scoped_bool_param ()
  {
    prm_Def[id].value.v.b = saved;
  }
};

struct rule_answer
{
  int error;
  DOMAIN_ARITH_KIND kind;
  DB_TYPE type, left_target, right_target;
};

static rule_answer
arith_rule (int opcode, DB_TYPE left, DB_TYPE right)
{
  rule_answer a;
  DOMAIN_ARITH arith;
  a.error = domain_arith_rule (opcode, left, right, &a.left_target, &a.right_target, &arith);
  a.kind = arith.kind;
  a.type = arith.type;
  return a;
}

static std::string
show (const rule_answer & a)
{
  static const char *kinds[] = { "UNRESOLVED", "NO_VALUE", "NUMBER", "DATE", "CONCAT", "STRING", "COLLECTION",
    "REJECT", "REJECT_OR_NULL"
  };
  return std::string ("e=") + std::to_string (a.error) + " " + kinds[a.kind] + " " + type_name (a.type) + " targets "
    + type_name (a.left_target) + "," + type_name (a.right_target);
}

static DB_VALUE
string_value (const char *s, int codeset, int collation)
{
  DB_VALUE v;
  db_make_varchar (&v, DB_MAX_VARCHAR_PRECISION, s, (int) strlen (s), codeset, collation);
  return v;
}

static DB_VALUE
numeric_value (const char *text, int p, int s)
{
  DB_VALUE src, dst;
  db_make_string (&src, text);
  db_make_null (&dst);
  REQUIRE (tp_value_cast (&src, &dst, tp_domain_resolve (DB_TYPE_NUMERIC, NULL, p, s, NULL, 0), false)
	   == DOMAIN_COMPATIBLE);
  return dst;
}

static std::string
domain_text (const TP_DOMAIN * d)
{
  if (d == NULL)
    {
      return "no domain";
    }
  char buf[128];
  snprintf (buf, sizeof (buf), "%s(%d,%d)", type_name (TP_DOMAIN_TYPE (d)), d->precision, d->scale);
  std::string s = buf;
  if (TP_TYPE_HAS_COLLATION (TP_DOMAIN_TYPE (d)))
    {
      s += " coll=" + std::to_string (d->collation_id);
    }
  return s;
}

/*
 * The smallest plan qexec_resolve_late_bind_node reads (domain_plan.h): n bind operand items (val_pos 0..n-1, each a
 * late-binding position, DOMAIN_PLAN_LATE_BIND) and one late-binding node over them (resolved index 0), linked as the
 * load links it (DOMAIN_LATE_BIND_LINK); the execution's resolved domain table holds the binds (in) and one entry.
 */
struct one_node_plan
{
  DOMAIN_PLAN_ITEM items[4];
  DOMAIN_PLAN_ITEM_COLD cold[4];
  DOMAIN_PLAN_ITEM *nodes[1];
  DOMAIN_LATE_BIND_LINK links[1];
  const DOMAIN_PLAN_ITEM *operands[3];
  const DB_VALUE *literals[3];
  int resolved_node[1];
  DOMAIN_PLAN plan;
  RESOLVED_DOMAIN entries[1];
  RESOLVED_DOMAIN_TABLE table;
  DB_VALUE binds[3];

  one_node_plan (DOMAIN_CTX context, int opcode, int n_operands)
  {
    memset (items, 0, sizeof (items));
    memset (cold, 0, sizeof (cold));
    memset (links, 0, sizeof (links));
    memset (&plan, 0, sizeof (plan));
    memset (&table, 0, sizeof (table));
    for (int i = 0; i < 3; i++)
      {
	literals[i] = NULL;
	db_make_null (&binds[i]);
      }
    for (int i = 0; i < n_operands; i++)
      {
	items[i].resolved_index = -1;
	items[i].ref = i;
	items[i].flags = DOMAIN_PLAN_LATE_BIND;
	items[i].node_domain_index = -1;
	cold[i].val_pos = i;
	cold[i].constant_branch = -1;
	operands[i] = &items[i];
      }
    DOMAIN_PLAN_ITEM *node = &items[n_operands];
    node->resolved_index = 0;
    node->ref = -1;
    node->flags = DOMAIN_PLAN_LATE_BIND;
    node->node_domain_index = -1;
    cold[n_operands].val_pos = -1;
    cold[n_operands].ctx = (short) context;
    cold[n_operands].opcode = opcode;
    cold[n_operands].constant_branch = -1;
    nodes[0] = node;
    links[0].operands = operands;
    links[0].literal = literals;
    links[0].n_operands = n_operands;
    resolved_node[0] = 0;
    plan.n_items = n_operands + 1;
    plan.items = items;
    plan.items_cold = cold;
    plan.n_resolved = 1;
    plan.n_late_bind_nodes = 1;
    plan.late_bind_nodes = nodes;
    plan.late_bind_links = links;
    plan.resolved_late_bind_node = resolved_node;
    table.in = binds;
    table.domains = entries;
    table.n_resolved = 1;
    table.plan = &plan;
  }

  /* one execution: the client's values, then resolve_domains' late-binding node step */
  int execute (const DB_VALUE & a, const DB_VALUE & b)
  {
    binds[0] = a;
    binds[1] = b;
    entries[0] = RESOLVED_DOMAIN ();
    er_clear ();
    return unitseam_resolve_late_bind_node (&plan, 0, &table);
  }

  const RESOLVED_DOMAIN & entry () const
  {
    return entries[0];
  }
};

static const DB_TYPE NUMBERS[] = {
  DB_TYPE_SHORT, DB_TYPE_INTEGER, DB_TYPE_BIGINT, DB_TYPE_NUMERIC, DB_TYPE_FLOAT, DB_TYPE_DOUBLE, DB_TYPE_MONETARY
};

static const DB_TYPE ALL_TYPES[] = {
  DB_TYPE_NULL, DB_TYPE_SHORT, DB_TYPE_INTEGER, DB_TYPE_BIGINT, DB_TYPE_NUMERIC, DB_TYPE_FLOAT, DB_TYPE_DOUBLE,
  DB_TYPE_MONETARY, DB_TYPE_CHAR, DB_TYPE_VARCHAR, DB_TYPE_BIT, DB_TYPE_VARBIT, DB_TYPE_DATE, DB_TYPE_TIME,
  DB_TYPE_DATETIME, DB_TYPE_TIMESTAMP, DB_TYPE_DATETIMETZ, DB_TYPE_TIMESTAMPLTZ, DB_TYPE_ENUMERATION, DB_TYPE_SET,
  DB_TYPE_MULTISET, DB_TYPE_SEQUENCE, DB_TYPE_JSON, DB_TYPE_BLOB
};

static DB_TYPE
type_of_letter (char c)
{
  switch (c)
    {
    case 'S':
      return DB_TYPE_SHORT;
    case 'I':
      return DB_TYPE_INTEGER;
    case 'B':
      return DB_TYPE_BIGINT;
    case 'N':
      return DB_TYPE_NUMERIC;
    case 'F':
      return DB_TYPE_FLOAT;
    case 'D':
      return DB_TYPE_DOUBLE;
    default:
      return DB_TYPE_MONETARY;
    }
}

static const TP_DOMAIN *
type_domain_of (DB_TYPE type)
{
  return tp_domain_resolve_default (type);
}

/* ---------------------------------------------------------------- cases */

TEST_CASE ("A1 number with number: the typed operator's result type, no operand converted", "[contract]")
{
  REQUIRE (boot_types ());
  /* row = left, column = right, in NUMBERS order (S I B N F D M); a9940d67a query_opfunc.c: qdata_add_*_to_dbval
   * 1683-2120, qdata_subtract_*_to_dbval 3732-, qdata_multiply_*_to_dbval 5267-, qdata_divide_*_to_dbval 5877- */
  const char *const add[] = { "SIBNFDM", "IIBNFDM", "BBBNFDM", "NNNNDDM", "FFDDFDM", "DDDDDDM", "MMMMMMM" };
  const char *const other[] = { "SIBNFDM", "IIBNFDM", "BBBNFDM", "NNNNDDM", "FFFDFDM", "DDDDDDM", "MMMMMMM" };
  const int ops[] = { T_ADD, T_SUB, T_MUL, T_DIV };
  for (int op : ops)
    {
      const char *const *grid = op == T_ADD ? add : other;
      for (int l = 0; l < 7; l++)
	{
	  for (int r = 0; r < 7; r++)
	    {
	      const rule_answer a = arith_rule (op, NUMBERS[l], NUMBERS[r]);
	      INFO ("op " << op << ": " << type_name (NUMBERS[l]) << " with " << type_name (NUMBERS[r]) << " -> " << show (a));
	      CHECK (a.error == NO_ERROR);
	      CHECK (a.kind == DOMAIN_ARITH_NUMBER);
	      CHECK (a.type == type_of_letter (grid[l][r]));
	      CHECK (a.left_target == NUMBERS[l]);
	      CHECK (a.right_target == NUMBERS[r]);
	    }
	}
    }
}

TEST_CASE ("A2 the one asymmetric number pair: FLOAT + BIGINT is DOUBLE, BIGINT + FLOAT is FLOAT", "[contract]")
{
  REQUIRE (boot_types ());
  /* a9940d67a query_opfunc.c:1961 qdata_add_float_to_dbval adds a BIGINT in double (qdata_add_double), :1866
   * qdata_add_bigint_to_dbval adds a FLOAT in float (qdata_add_float ((float) bi, ...)) */
  std::vector<std::string> asymmetric;
  const int ops[] = { T_ADD, T_SUB, T_MUL, T_DIV };
  for (int op : ops)
    {
      for (DB_TYPE l : NUMBERS)
	{
	  for (DB_TYPE r : NUMBERS)
	    {
	      if (l < r && arith_rule (op, l, r).type != arith_rule (op, r, l).type)
		{
		  asymmetric.push_back (std::to_string (op) + " " + type_name (l) + " " + type_name (r));
		}
	    }
	}
    }
  CHECK (asymmetric == std::vector<std::string> { std::to_string (T_ADD) + " float bigint" });
  CHECK (arith_rule (T_ADD, DB_TYPE_FLOAT, DB_TYPE_BIGINT).type == DB_TYPE_DOUBLE);
  CHECK (arith_rule (T_ADD, DB_TYPE_BIGINT, DB_TYPE_FLOAT).type == DB_TYPE_FLOAT);
}

TEST_CASE ("A3 a NULL operand gives no value and no error, every operator and operand type", "[contract]")
{
  REQUIRE (boot_types ());
  const int ops[] = { T_ADD, T_SUB, T_MUL, T_DIV };
  for (int op : ops)
    {
      for (DB_TYPE t : ALL_TYPES)
	{
	  const rule_answer left = arith_rule (op, DB_TYPE_NULL, t), right = arith_rule (op, t, DB_TYPE_NULL);
	  INFO ("op " << op << " with " << type_name (t) << ": NULL first " << show (left) << ", NULL second "
		<< show (right));
	  CHECK (left.error == NO_ERROR);
	  CHECK (left.kind == DOMAIN_ARITH_NO_VALUE);
	  CHECK (right.error == NO_ERROR);
	  CHECK (right.kind == DOMAIN_ARITH_NO_VALUE);
	}
    }
}

TEST_CASE ("A4 string, date and other pairs: operand targets, operator kind, the typed operators' asymmetries",
	   "[contract]")
{
  REQUIRE (boot_types ());
  struct
  {
    int op;
    DB_TYPE left, right;
    const char *expected;
  } cases[] = {
    {T_ADD, DB_TYPE_VARCHAR, DB_TYPE_INTEGER, "e=0 NUMBER double targets double,integer"},
    {T_ADD, DB_TYPE_INTEGER, DB_TYPE_VARCHAR, "e=0 NUMBER double targets integer,double"},
    {T_ADD, DB_TYPE_VARCHAR, DB_TYPE_VARCHAR, "e=0 CONCAT character varying targets character varying,character varying"},
    {T_ADD, DB_TYPE_CHAR, DB_TYPE_VARCHAR, "e=0 CONCAT character varying targets character,character varying"},
    {T_ADD, DB_TYPE_VARCHAR, DB_TYPE_BIT, "e=-622 CONCAT *NULL* targets character varying,bit"},
    {T_ADD, DB_TYPE_BIT, DB_TYPE_VARBIT, "e=0 CONCAT bit varying targets bit,bit varying"},
    {T_ADD, DB_TYPE_DATE, DB_TYPE_INTEGER, "e=0 DATE date targets date,integer"},
    {T_ADD, DB_TYPE_INTEGER, DB_TYPE_DATE, "e=0 DATE date targets integer,date"},
    {T_ADD, DB_TYPE_DATE, DB_TYPE_DOUBLE, "e=0 DATE date targets date,bigint"},
    {T_ADD, DB_TYPE_DATE, DB_TYPE_VARCHAR, "e=0 DATE date targets date,bigint"},
    {T_ADD, DB_TYPE_VARCHAR, DB_TYPE_DATE, "e=0 DATE date targets bigint,date"},
    {T_ADD, DB_TYPE_DATE, DB_TYPE_TIME, "e=-454 REJECT_OR_NULL *NULL* targets date,time"},
    {T_ADD, DB_TYPE_TIME, DB_TYPE_DATE, "e=0 NO_VALUE *NULL* targets time,date"},
    {T_ADD, DB_TYPE_DATETIME, DB_TYPE_BIGINT, "e=0 DATE datetime targets datetime,bigint"},
    {T_ADD, DB_TYPE_TIMESTAMP, DB_TYPE_INTEGER, "e=0 DATE timestamp targets timestamp,integer"},
    {T_ADD, DB_TYPE_ENUMERATION, DB_TYPE_VARCHAR, "e=0 CONCAT character varying targets character varying,character varying"},
    {T_ADD, DB_TYPE_ENUMERATION, DB_TYPE_INTEGER, "e=0 NUMBER integer targets smallint,integer"},
    {T_ADD, DB_TYPE_INTEGER, DB_TYPE_ENUMERATION, "e=0 NUMBER integer targets integer,smallint"},
    {T_ADD, DB_TYPE_SET, DB_TYPE_SET, "e=0 COLLECTION set targets set,set"},
    {T_ADD, DB_TYPE_SET, DB_TYPE_MULTISET, "e=0 COLLECTION multiset targets set,multiset"},
    {T_ADD, DB_TYPE_SET, DB_TYPE_INTEGER, "e=-454 REJECT *NULL* targets set,integer"},
    {T_ADD, DB_TYPE_INTEGER, DB_TYPE_SET, "e=0 NO_VALUE *NULL* targets integer,set"},
    {T_ADD, DB_TYPE_INTEGER, DB_TYPE_BIT, "e=0 NO_VALUE *NULL* targets integer,bit"},
    {T_ADD, DB_TYPE_BIT, DB_TYPE_INTEGER, "e=-621 STRING *NULL* targets bit,integer"},
    {T_ADD, DB_TYPE_JSON, DB_TYPE_INTEGER, "e=-454 REJECT *NULL* targets json,integer"},
    {T_SUB, DB_TYPE_DATE, DB_TYPE_DATE, "e=0 DATE integer targets date,date"},
    {T_SUB, DB_TYPE_DATETIME, DB_TYPE_DATE, "e=0 DATE bigint targets datetime,date"},
    {T_SUB, DB_TYPE_TIMESTAMP, DB_TYPE_TIMESTAMP, "e=0 DATE integer targets timestamp,timestamp"},
    {T_SUB, DB_TYPE_DATE, DB_TYPE_INTEGER, "e=0 DATE date targets date,integer"},
    {T_SUB, DB_TYPE_INTEGER, DB_TYPE_DATE, "e=0 DATE date targets integer,date"},
    {T_SUB, DB_TYPE_SHORT, DB_TYPE_DATE, "e=0 DATE time targets smallint,date"},
    {T_SUB, DB_TYPE_DOUBLE, DB_TYPE_DATE, "e=0 DATE date targets bigint,date"},
    {T_SUB, DB_TYPE_VARCHAR, DB_TYPE_DATE, "e=0 DATE bigint targets datetime,datetime"},
    {T_SUB, DB_TYPE_DATE, DB_TYPE_VARCHAR, "e=0 DATE bigint targets datetime,datetime"},
    {T_SUB, DB_TYPE_TIME, DB_TYPE_VARCHAR, "e=0 DATE integer targets time,time"},
    {T_SUB, DB_TYPE_VARCHAR, DB_TYPE_INTEGER, "e=0 NUMBER double targets double,integer"},
    {T_SUB, DB_TYPE_DATETIME, DB_TYPE_TIME, "e=0 NO_VALUE *NULL* targets datetime,time"},
    {T_SUB, DB_TYPE_ENUMERATION, DB_TYPE_INTEGER, "e=0 NUMBER integer targets smallint,integer"},
    {T_SUB, DB_TYPE_SET, DB_TYPE_SET, "e=0 COLLECTION set targets set,set"},
    {T_SUB, DB_TYPE_MULTISET, DB_TYPE_SET, "e=0 COLLECTION multiset targets multiset,set"},
    {T_MUL, DB_TYPE_INTEGER, DB_TYPE_DATE, "e=0 NO_VALUE *NULL* targets integer,date"},
    {T_MUL, DB_TYPE_DATE, DB_TYPE_INTEGER, "e=-454 REJECT_OR_NULL *NULL* targets date,integer"},
    {T_MUL, DB_TYPE_VARCHAR, DB_TYPE_VARCHAR, "e=0 NUMBER double targets double,double"},
    {T_MUL, DB_TYPE_VARCHAR, DB_TYPE_INTEGER, "e=0 NUMBER double targets double,integer"},
    {T_MUL, DB_TYPE_ENUMERATION, DB_TYPE_INTEGER, "e=-454 REJECT_OR_NULL *NULL* targets enum,integer"},
    {T_MUL, DB_TYPE_SET, DB_TYPE_SET, "e=0 COLLECTION set targets set,set"},
    {T_DIV, DB_TYPE_INTEGER, DB_TYPE_INTEGER, "e=0 NUMBER integer targets integer,integer"},
    {T_DIV, DB_TYPE_DOUBLE, DB_TYPE_VARCHAR, "e=0 NUMBER double targets double,double"},
    {T_DIV, DB_TYPE_SET, DB_TYPE_SET, "e=-454 REJECT_OR_NULL *NULL* targets set,set"},
    {T_DIV, DB_TYPE_VARCHAR, DB_TYPE_VARCHAR, "e=0 NUMBER double targets double,double"},
  };
  const bool capture = getenv ("UNITSEAM_CAPTURE") != NULL;
  for (auto & c : cases)
    {
      const std::string got = show (arith_rule (c.op, c.left, c.right));
      if (capture)
	{
	  printf ("A4 {%d, (DB_TYPE) %d, (DB_TYPE) %d, \"%s\"},\n", c.op, (int) c.left, (int) c.right, got.c_str ());
	  continue;
	}
      INFO ("op " << c.op << ": " << type_name (c.left) << " with " << type_name (c.right));
      CHECK (got == c.expected);
    }
  /* MOD (db_mod_dbval): a string is DOUBLE, a date is rejected */
  DOMAIN_OPERAND int_string[2] = { {type_domain_of (DB_TYPE_INTEGER), DB_TYPE_INTEGER, -1, false},
  {type_domain_of (DB_TYPE_VARCHAR), DB_TYPE_VARCHAR, -1, false}
  };
  RESOLVED_DOMAIN r;
  bool late = true;
  CHECK (domain_resolve (DOMAIN_CTX_ARITH, T_MOD, int_string, 2, NULL, &r, &late) == NO_ERROR);
  CHECK (domain_text (r.domain) == "double(15,0)");
  CHECK (domain_text (r.operand_domain[1]) == "double(15,0)");
  DOMAIN_OPERAND date_int[2] = { {type_domain_of (DB_TYPE_DATE), DB_TYPE_DATE, -1, false},
  {type_domain_of (DB_TYPE_INTEGER), DB_TYPE_INTEGER, -1, false}
  };
  CHECK (domain_resolve (DOMAIN_CTX_ARITH, T_MOD, date_int, 2, NULL, &r, &late) == ER_QPROC_INVALID_DATATYPE);
}

TEST_CASE ("A5 parameters: oracle_compat_number_behavior, return_null_on_function_errors, plus_as_concat",
	   "[contract]")
{
  REQUIRE (boot_types ());
  /* defaults (system_parameter.c prm_Def): integer division stays integer, a rejected pair is an error, + of two
   * strings concatenates */
  CHECK (show (arith_rule (T_DIV, DB_TYPE_INTEGER, DB_TYPE_INTEGER)) == "e=0 NUMBER integer targets integer,integer");
  CHECK (show (arith_rule (T_MUL, DB_TYPE_DATE, DB_TYPE_INTEGER)) == "e=-454 REJECT_OR_NULL *NULL* targets date,integer");
  CHECK (show (arith_rule (T_ADD, DB_TYPE_VARCHAR, DB_TYPE_VARCHAR))
	 == "e=0 CONCAT character varying targets character varying,character varying");
  {
    scoped_bool_param p (PRM_ID_ORACLE_COMPAT_NUMBER_BEHAVIOR, true);
    CHECK (show (arith_rule (T_DIV, DB_TYPE_INTEGER, DB_TYPE_INTEGER)) == "e=0 NUMBER numeric targets numeric,numeric");
    CHECK (show (arith_rule (T_DIV, DB_TYPE_SHORT, DB_TYPE_BIGINT)) == "e=0 NUMBER numeric targets numeric,numeric");
    CHECK (show (arith_rule (T_DIV, DB_TYPE_INTEGER, DB_TYPE_DOUBLE)) == "e=0 NUMBER double targets integer,double");
  }
  {
    scoped_bool_param p (PRM_ID_RETURN_NULL_ON_FUNCTION_ERRORS, true);
    CHECK (show (arith_rule (T_MUL, DB_TYPE_DATE, DB_TYPE_INTEGER)) == "e=0 REJECT_OR_NULL *NULL* targets date,integer");
    /* addition rejects always (the date addition's own case) */
    CHECK (show (arith_rule (T_ADD, DB_TYPE_SET, DB_TYPE_INTEGER)) == "e=-454 REJECT *NULL* targets set,integer");
  }
  {
    scoped_bool_param p (PRM_ID_PLUS_AS_CONCAT, false);
    CHECK (show (arith_rule (T_ADD, DB_TYPE_VARCHAR, DB_TYPE_VARCHAR)) == "e=0 NUMBER double targets double,double");
  }
}

TEST_CASE ("A6 bind operands: one plan, the bind types rotated - each execution resolves its own types", "[contract]")
{
  REQUIRE (boot_types ());
  one_node_plan p (DOMAIN_CTX_ARITH, T_ADD, 2);
  DB_VALUE i1, i2, big, half, nul, day;
  db_make_int (&i1, 1);
  db_make_int (&i2, 2);
  db_make_bigint (&big, 5);
  half = string_value ("1.5", INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY);
  db_make_null (&nul);
  db_make_date (&day, 1, 2, 2020);
  const TP_VALUE_CONVERTER varchar_to_double =
    tp_value_find_converter (DB_TYPE_VARCHAR, tp_domain_resolve_default (DB_TYPE_DOUBLE), DOMAIN_CONVERT_ASSIGN);

  REQUIRE (p.execute (i1, i2) == NO_ERROR);
  CHECK (domain_text (p.entry ().domain) == "integer(10,0)");
  CHECK (p.entry ().arith.kind == DOMAIN_ARITH_NUMBER);
  CHECK (p.entry ().conv[0] == NULL);

  /* the string bind: plus with a number swaps and casts the string to DOUBLE, ASSIGN (tp_value_auto_cast) */
  REQUIRE (p.execute (half, i2) == NO_ERROR);
  CHECK (domain_text (p.entry ().domain) == "double(15,0)");
  CHECK (domain_text (p.entry ().operand_domain[0]) == "double(15,0)");
  CHECK (p.entry ().conv[0] == varchar_to_double);
  CHECK (p.entry ().conv[1] == NULL);

  /* a NULL bind: no value (the operator answers NULL before it looks at types) */
  REQUIRE (p.execute (nul, i2) == NO_ERROR);
  CHECK (p.entry ().domain == &tp_Null_domain);
  CHECK (p.entry ().operand_domain[0] == &tp_Null_domain);
  CHECK (p.entry ().arith.kind == DOMAIN_ARITH_NO_VALUE);

  REQUIRE (p.execute (day, i2) == NO_ERROR);
  CHECK (domain_text (p.entry ().domain) == "date(10,0)");
  CHECK (p.entry ().arith.kind == DOMAIN_ARITH_DATE);

  REQUIRE (p.execute (big, i2) == NO_ERROR);
  CHECK (domain_text (p.entry ().domain) == "bigint(19,0)");

  /* back to the first types: the first answer again */
  REQUIRE (p.execute (i1, i2) == NO_ERROR);
  CHECK (domain_text (p.entry ().domain) == "integer(10,0)");
  CHECK (p.entry ().conv[0] == NULL);

  /* NUMERIC binds: the default NUMERIC domain, the value operation fixes precision and scale */
  REQUIRE (p.execute (numeric_value ("12.34", 4, 2), i1) == NO_ERROR);
  CHECK (domain_text (p.entry ().domain) == "numeric(40,0)");
  CHECK (domain_text (p.entry ().operand_domain[0]) == "numeric(4,2)");
}

TEST_CASE ("A7 a bind pair the operator rejects: -454 before any row, the entry holds NULL", "[contract]")
{
  REQUIRE (boot_types ());
  one_node_plan p (DOMAIN_CTX_ARITH, T_MUL, 2);
  DB_VALUE day, i1;
  db_make_date (&day, 1, 2, 2020);
  db_make_int (&i1, 1);
  CHECK (p.execute (day, i1) == ER_QPROC_INVALID_DATATYPE);
  CHECK (er_errid () == ER_QPROC_INVALID_DATATYPE);
  CHECK (p.entry ().domain == &tp_Null_domain);
  /* the other order is the typed operator's silent no value (INT * DATE) */
  CHECK (p.execute (i1, day) == NO_ERROR);
  CHECK (p.entry ().domain == &tp_Null_domain);
  CHECK (p.entry ().arith.kind == DOMAIN_ARITH_NO_VALUE);
  /* a session asking for NULLs on function errors gets no error */
  scoped_bool_param q (PRM_ID_RETURN_NULL_ON_FUNCTION_ERRORS, true);
  CHECK (p.execute (day, i1) == NO_ERROR);
  CHECK (p.entry ().domain == &tp_Null_domain);
}

TEST_CASE ("A8 string binds added: the collation db_string_concatenate gives the row; a pair it rejects is NULL",
	   "[contract]")
{
  REQUIRE (boot_types ());
  one_node_plan p (DOMAIN_CTX_ARITH, T_ADD, 2);
  const DB_VALUE u = string_value ("u", INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY);
  const DB_VALUE i = string_value ("i", INTL_CODESET_ISO88591, LANG_COLL_ISO_BINARY);
  const DB_VALUE ci = string_value ("c", INTL_CODESET_UTF8, LANG_COLL_UTF8_EN_CI);
  const DB_VALUE cs = string_value ("s", INTL_CODESET_UTF8, LANG_COLL_UTF8_EN_CS);
  const std::pair < DB_VALUE, DB_VALUE > pairs[] = { {u, i}, {i, u}, {u, ci}, {ci, u}, {ci, cs}, {u, u} };
  for (const auto & pr : pairs)
    {
      DB_VALUE row;
      DB_DATA_STATUS status;
      db_make_null (&row);
      er_clear ();
      const int row_error = db_string_concatenate (&pr.first, &pr.second, &row, &status);
      REQUIRE (p.execute (pr.first, pr.second) == NO_ERROR);
      INFO ("collations " << db_get_string_collation (&pr.first) << " + " << db_get_string_collation (&pr.second)
	    << ": row error " << row_error << ", resolved " << domain_text (p.entry ().domain));
      CHECK (p.entry ().arith.kind == DOMAIN_ARITH_CONCAT);
      if (row_error != NO_ERROR)
	{
	  CHECK (row_error == ER_QSTR_INCOMPATIBLE_COLLATIONS);
	  CHECK (p.entry ().domain == &tp_Null_domain);
	}
      else
	{
	  REQUIRE (p.entry ().domain != NULL);
	  CHECK (p.entry ().domain->collation_id == db_get_string_collation (&row));
	  CHECK (p.entry ().domain->codeset == db_get_string_codeset (&row));
	  CHECK (TP_DOMAIN_TYPE (p.entry ().domain) == DB_VALUE_DOMAIN_TYPE (&row));
	}
      pr_clear_value (&row);
    }
}

TEST_CASE ("A9 UNKNOWN: a VARIABLE operand without a value type waits for a late binding; a literal and a compiled "
	   "producer give their own domains", "[contract]")
{
  REQUIRE (boot_types ());
  DOMAIN_OPERAND ops[2] = { {&tp_Variable_domain, DB_TYPE_NULL, -1, true},
  {tp_domain_resolve_default (DB_TYPE_INTEGER), DB_TYPE_NULL, -1, false}
  };
  RESOLVED_DOMAIN r;
  r.domain = &tp_Integer_domain;
  bool late = false;
  CHECK (domain_resolve (DOMAIN_CTX_ARITH, T_ADD, ops, 2, NULL, &r, &late) == NO_ERROR);
  CHECK (late);
  CHECK (r.domain == &tp_Integer_domain);
  /* a fixed domain with no value type is typed by its domain */
  DOMAIN_OPERAND typed[2] = { {tp_domain_resolve_default (DB_TYPE_BIGINT), DB_TYPE_NULL, -1, false},
  {tp_domain_resolve_default (DB_TYPE_INTEGER), DB_TYPE_NULL, -1, false}
  };
  CHECK (domain_resolve (DOMAIN_CTX_ARITH, T_ADD, typed, 2, NULL, &r, &late) == NO_ERROR);
  CHECK (!late);
  CHECK (domain_text (r.domain) == "bigint(19,0)");

  /* qexec_resolve_operand: operand 0 a literal (no value position), operand 1 a compiled producer (no value, no
   * resolution: its fixed domain) */
  one_node_plan p (DOMAIN_CTX_ARITH, T_ADD, 2);
  DB_VALUE literal;
  db_make_double (&literal, 2.5);
  p.cold[0].val_pos = -1;
  p.literals[0] = &literal;
  p.cold[1].val_pos = -1;
  p.items[1].fixed.domain = tp_domain_resolve (DB_TYPE_NUMERIC, NULL, 7, 3, NULL, 0);
  DOMAIN_OPERAND operand;
  REQUIRE (unitseam_resolve_operand (&p.plan, &p.table, 0, 0, DOMAIN_CTX_ARITH, T_ADD, &operand));
  CHECK (domain_text (operand.domain) == "double(15,0)");
  CHECK (operand.val_type == DB_TYPE_DOUBLE);
  REQUIRE (unitseam_resolve_operand (&p.plan, &p.table, 0, 1, DOMAIN_CTX_ARITH, T_ADD, &operand));
  CHECK (domain_text (operand.domain) == "numeric(7,3)");
  CHECK (operand.val_type == DB_TYPE_NUMERIC);
  CHECK (!operand.is_variable_pos);
  /* a NULL literal: no value in arithmetic */
  DB_VALUE null_literal;
  db_make_null (&null_literal);
  p.literals[0] = &null_literal;
  REQUIRE (unitseam_resolve_operand (&p.plan, &p.table, 0, 0, DOMAIN_CTX_ARITH, T_ADD, &operand));
  CHECK (operand.domain == &tp_Null_domain);
  CHECK (operand.val_type == DB_TYPE_NULL);
}

TEST_CASE ("A10 SUM / AVG over a late-bound argument: function and accumulator domains; a date is rejected",
	   "[contract]")
{
  REQUIRE (boot_types ());
  struct
  {
    int function;
    const TP_DOMAIN *argument;
    const char *expected;	/* "error" or "function | accumulator" */
  } cases[] = {
    {PT_SUM, tp_domain_resolve_default (DB_TYPE_INTEGER), "integer(10,0) | integer(10,0)"},
    {PT_AVG, tp_domain_resolve_default (DB_TYPE_INTEGER), "integer(10,0) | integer(10,0)"},
    {PT_SUM, tp_domain_resolve_default (DB_TYPE_BIGINT), "bigint(19,0) | bigint(19,0)"},
    {PT_SUM, tp_domain_resolve_default (DB_TYPE_SHORT), "smallint(5,0) | smallint(5,0)"},
    {PT_SUM, tp_domain_resolve (DB_TYPE_NUMERIC, NULL, 5, 2, NULL, 0), "numeric(5,2) | numeric(40,0)"},
    {PT_AVG, tp_domain_resolve (DB_TYPE_NUMERIC, NULL, 5, 2, NULL, 0), "numeric(5,2) | numeric(40,0)"},
    {PT_SUM, tp_domain_resolve_default (DB_TYPE_FLOAT), "float(7,0) | double(15,0)"},
    {PT_AVG, tp_domain_resolve_default (DB_TYPE_FLOAT), "float(7,0) | double(15,0)"},
    {PT_SUM, tp_domain_resolve_default (DB_TYPE_DOUBLE), "double(15,0) | double(15,0)"},
    {PT_SUM, tp_domain_resolve_default (DB_TYPE_MONETARY), "monetary(15,0) | monetary(15,0)"},
    {PT_SUM, tp_domain_resolve (DB_TYPE_VARCHAR, NULL, 10, 0, NULL, LANG_COLL_UTF8_BINARY), "double(15,0) | double(15,0)"},
    {PT_AVG, tp_domain_resolve (DB_TYPE_VARCHAR, NULL, 10, 0, NULL, LANG_COLL_UTF8_BINARY), "double(15,0) | double(15,0)"},
    {PT_SUM, tp_domain_resolve_default (DB_TYPE_DATE), "error -454"},
    {PT_AVG, tp_domain_resolve_default (DB_TYPE_DATETIME), "error -454"},
    {PT_MAX, tp_domain_resolve (DB_TYPE_VARCHAR, NULL, 10, 0, NULL, LANG_COLL_UTF8_BINARY), "character varying(10,0) coll=1 | character varying(10,0) coll=1"},
    {PT_MIN, tp_domain_resolve_default (DB_TYPE_DATE), "date(10,0) | date(10,0)"},
    {PT_GROUP_CONCAT, tp_domain_resolve_default (DB_TYPE_INTEGER), "character varying(1073741823,0) coll=1 | character varying(1073741823,0) coll=1"},
    {PT_COUNT, tp_domain_resolve (DB_TYPE_VARCHAR, NULL, 10, 0, NULL, LANG_COLL_UTF8_BINARY), "bigint(19,0) | character varying(10,0) coll=1"},
    {PT_STDDEV, tp_domain_resolve_default (DB_TYPE_INTEGER), "integer(10,0) | double(15,0)"},
    {PT_MEDIAN, tp_domain_resolve_default (DB_TYPE_INTEGER), "double(15,0) | double(15,0)"},
    {PT_MEDIAN, tp_domain_resolve_default (DB_TYPE_DATE), "date(10,0) | date(10,0)"},
  };
  const bool capture = getenv ("UNITSEAM_CAPTURE") != NULL;
  for (auto & c : cases)
    {
      DOMAIN_OPERAND arg = { c.argument, TP_DOMAIN_TYPE (c.argument), -1, true };
      RESOLVED_DOMAIN r;
      bool late = true;
      const int e = domain_resolve (DOMAIN_CTX_AGG, c.function, &arg, 1, NULL, &r, &late);
      const std::string got = e != NO_ERROR ? "error " + std::to_string (e)
	: domain_text (r.domain) + " | " + domain_text (r.operand_domain[0]);
      if (capture)
	{
	  printf ("A10 %d %s -> \"%s\"\n", c.function, domain_text (c.argument).c_str (), got.c_str ());
	  continue;
	}
      INFO ("function " << c.function << " over " << domain_text (c.argument));
      CHECK (got == c.expected);
    }
  /* the NUMERIC accumulator is the floating NUMERIC (DB_DEFAULT_NUMERIC_PRECISION == DB_MAX_NUMERIC_PRECISION, 40):
   * an added value keeps its scale on the way in (numeric_opfunc.c numeric_coerce_num_to_num: a destination at the
   * default precision takes the source as it is) */
  {
    DOMAIN_OPERAND n52 = { tp_domain_resolve (DB_TYPE_NUMERIC, NULL, 5, 2, NULL, 0), DB_TYPE_NUMERIC, -1, true };
    RESOLVED_DOMAIN acc;
    bool late = true;
    REQUIRE (domain_resolve (DOMAIN_CTX_AGG, PT_SUM, &n52, 1, NULL, &acc, &late) == NO_ERROR);
    DB_VALUE in = numeric_value ("1.25", 5, 2), out;
    db_make_null (&out);
    if (acc.conv[0] != NULL)
      {
	REQUIRE (tp_value_convert (acc.conv[0], acc.operand_domain[0], &in, &out) == DOMAIN_COMPATIBLE);
      }
    else
      {
	out = in;
      }
    char digits[NUMERIC_MAX_STRING_SIZE];
    numeric_db_value_print (&out, digits);
    CHECK (std::string (digits) == "1.25");
  }
  scoped_bool_param p (PRM_ID_RETURN_NULL_ON_FUNCTION_ERRORS, true);
  DOMAIN_OPERAND day = { tp_domain_resolve_default (DB_TYPE_DATE), DB_TYPE_DATE, -1, true };
  RESOLVED_DOMAIN r;
  bool late = true;
  CHECK (domain_resolve (DOMAIN_CTX_AGG, PT_SUM, &day, 1, NULL, &r, &late) == NO_ERROR);
}
