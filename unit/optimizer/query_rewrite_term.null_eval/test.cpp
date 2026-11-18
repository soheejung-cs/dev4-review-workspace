/*
 * PR#8609 (CBRD-28029) -- qo_null_eval_expr () of src/optimizer/rewriter/query_rewrite_term.c (CS_MODE): the NULL
 * rejection prover reads which values (TRUE / FALSE / UNKNOWN) an expression can take while the target columns are
 * NULL.  Its default branch takes an operation it does not list as "NULL when its first operand is NULL" (fail-open
 * towards rejection is the user's decision, 2026-11-18 Q7: keep the default, guard it with this suite).
 *
 *   N1 [contract] every PT_OP_TYPE enumerator (parse_tree.h, PT_AND .. PT_LAST_OPCODE) is in the reviewed table below --
 *                 a new operator fails this case with its name: whoever adds it decides its NULL behavior and puts it
 *                 in a class (or in REVIEWED_DEFAULT when "NULL iff the first operand is NULL" is right)
 *   N2 [contract] each table class behaves as the switch does (a probe per operator that tells the class from default)
 *   N3 [contract] representative predicates on a NULL column x
 *   N4 [contract] the default branch: x in the first operand -> UNKNOWN, x only in a later operand -> no claim (ANY)
 *
 * No seam: qo_null_eval* is the real code.  Nodes are built by hand (calloc'd PT_NODE, no parser), the target is a
 * spec id with no name (all PT_NORMAL columns of the spec).  system_parameter.c is linked for prm_get_bool_value
 * (PRM_ID_ORACLE_STYLE_EMPTY_STRING at its default, false).
 */
#include "catch.hpp"
#include "config.h"
#include "dbtype.h"
#include "parser.h"
#include "parse_tree.h"
#include "string_regex.hpp"
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

/* fake: system_parameter.c's static initialization names the regex engine of its default */
namespace cubregex
{
  const char *get_engine_name (const engine_type & type)
  {
    return type == engine_type::LIB_RE2 ? "re2" : "cppstd";
  }
}

/* hooks.cpp (appended to the TU) */
int unit_null_eval (PARSER_CONTEXT * parser, PT_NODE * node, UINTPTR spec_id);
int unit_null_eval_expr (PARSER_CONTEXT * parser, PT_NODE * expr, UINTPTR spec_id);
const char *unit_null_eval_tu_path (void);
extern int unit_null_eval_true, unit_null_eval_false, unit_null_eval_unknown;

namespace
{
  /* ------------------------------------------------------------ the reviewed table (head 3a69da257) */
  enum op_class
  {
    C_LOGIC,			/* AND OR XOR: three-valued combine of both operands */
    C_NOT,			/* NOT */
    C_IS_NULL,			/* IS NULL, IS NOT NULL */
    C_IS,			/* IS [NOT] TRUE/FALSE/UNKNOWN */
    C_BETWEEN,			/* [NOT] BETWEEN over a PT_BETWEEN_AND */
    C_RANGE,			/* PT_RANGE over or_next alternatives */
    C_QUANT,			/* IN, NOT IN, op SOME, op ALL */
    C_EXISTS,			/* EXISTS */
    C_CONCAT,			/* STRCAT CONCAT PLUS: strict unless oracle_style_empty_string */
    C_STRICT2,			/* NULL when either operand is NULL */
    C_STRICTN,			/* NULL when any of arg1..arg3 is NULL */
    C_ANY,			/* no claim: can be other than NULL on NULL, or reads another row */
    C_DEFAULT			/* reviewed: NULL when the first operand is NULL (the switch's default) */
  };

  const char *class_name (op_class c)
  {
    static const char *n[] = { "LOGIC", "NOT", "IS_NULL", "IS", "BETWEEN", "RANGE", "QUANT", "EXISTS", "CONCAT",
      "STRICT2", "STRICTN", "ANY", "DEFAULT"
    };
    return n[c];
  }

  struct op_entry
  {
    const char *name;
    PT_OP_TYPE op;
  };

#define OP(x) { #x, x }

  const op_entry T_LOGIC[] = { OP (PT_AND), OP (PT_OR), OP (PT_XOR) };
  const op_entry T_NOT[] = { OP (PT_NOT) };
  const op_entry T_IS_NULL[] = { OP (PT_IS_NULL), OP (PT_IS_NOT_NULL) };
  const op_entry T_IS[] = { OP (PT_IS), OP (PT_IS_NOT) };
  const op_entry T_BETWEEN[] = { OP (PT_BETWEEN), OP (PT_NOT_BETWEEN) };
  const op_entry T_RANGE[] = { OP (PT_RANGE) };
  const op_entry T_QUANT[] = {
    OP (PT_IS_IN), OP (PT_IS_NOT_IN), OP (PT_EQ_SOME), OP (PT_NE_SOME), OP (PT_GE_SOME), OP (PT_GT_SOME),
    OP (PT_LT_SOME), OP (PT_LE_SOME), OP (PT_EQ_ALL), OP (PT_NE_ALL), OP (PT_GE_ALL), OP (PT_GT_ALL), OP (PT_LT_ALL),
    OP (PT_LE_ALL)
  };
  const op_entry T_EXISTS[] = { OP (PT_EXISTS) };
  const op_entry T_CONCAT[] = { OP (PT_STRCAT), OP (PT_CONCAT), OP (PT_PLUS) };
  const op_entry T_STRICT2[] = {
    OP (PT_EQ), OP (PT_NE), OP (PT_GE), OP (PT_GT), OP (PT_LT), OP (PT_LE), OP (PT_LIKE), OP (PT_NOT_LIKE),
    OP (PT_RLIKE), OP (PT_NOT_RLIKE), OP (PT_RLIKE_BINARY), OP (PT_NOT_RLIKE_BINARY), OP (PT_MINUS), OP (PT_TIMES),
    OP (PT_DIVIDE), OP (PT_MOD), OP (PT_MODULUS), OP (PT_DIV), OP (PT_BIT_AND), OP (PT_BIT_OR), OP (PT_BIT_XOR),
    OP (PT_BITSHIFT_LEFT), OP (PT_BITSHIFT_RIGHT)
  };
  const op_entry T_STRICTN[] = {
    OP (PT_DATEDIFF), OP (PT_TIMEDIFF), OP (PT_STRCMP), OP (PT_ATAN2), OP (PT_POWER), OP (PT_POSITION),
    OP (PT_FINDINSET), OP (PT_LOCATE)
  };
  const op_entry T_ANY[] = {
    OP (PT_CASE), OP (PT_DECODE), OP (PT_IF), OP (PT_COALESCE), OP (PT_NVL), OP (PT_NVL2), OP (PT_IFNULL),
    OP (PT_ISNULL), OP (PT_CONCAT_WS), OP (PT_NULLIF), OP (PT_FIELD), OP (PT_NULLSAFE_EQ), OP (PT_TYPEOF),
    OP (PT_COERCIBILITY), OP (PT_RAND), OP (PT_DRAND), OP (PT_RANDOM), OP (PT_DRANDOM), OP (PT_DEFAULTF),
    OP (PT_INCR), OP (PT_DECR), OP (PT_PRIOR), OP (PT_QPRIOR), OP (PT_CONNECT_BY_ROOT), OP (PT_SYS_CONNECT_BY_PATH),
    OP (PT_PATH_EXPR_SET), OP (PT_OID_OF_DUPLICATE_KEY)
  };
  /* reviewed for the default branch: "NULL when the first operand is NULL" holds, or a later operand is an option a
   * NULL leaves at its default; an operator here whose NULL behavior differs must move to a class above */
  const op_entry T_DEFAULT[] = {
    OP (PT_GT_INF), OP (PT_LT_INF), OP (PT_SETEQ), OP (PT_SETNEQ), OP (PT_SUPERSETEQ),
    OP (PT_SUPERSET), OP (PT_SUBSET), OP (PT_SUBSETEQ), OP (PT_UNARY_MINUS), OP (PT_BIT_NOT),
    OP (PT_BIT_COUNT), OP (PT_ASSIGN), OP (PT_BETWEEN_AND), OP (PT_BETWEEN_GE_LE), OP (PT_BETWEEN_GE_LT),
    OP (PT_BETWEEN_GT_LE), OP (PT_BETWEEN_GT_LT), OP (PT_BETWEEN_EQ_NA), OP (PT_BETWEEN_INF_LE), OP (PT_BETWEEN_INF_LT),
    OP (PT_BETWEEN_GE_INF), OP (PT_BETWEEN_GT_INF), OP (PT_SUBSTRING), OP (PT_OCTET_LENGTH), OP (PT_BIT_LENGTH),
    OP (PT_SUBSTRING_INDEX), OP (PT_MD5), OP (PT_SPACE), OP (PT_CHAR_LENGTH), OP (PT_LOWER),
    OP (PT_UPPER), OP (PT_TRIM), OP (PT_LTRIM), OP (PT_RTRIM), OP (PT_LPAD),
    OP (PT_RPAD), OP (PT_REPLACE), OP (PT_TRANSLATE), OP (PT_REPEAT), OP (PT_ADD_MONTHS),
    OP (PT_LAST_DAY), OP (PT_MONTHS_BETWEEN), OP (PT_SYS_DATE), OP (PT_TO_CHAR), OP (PT_TO_DATE),
    OP (PT_TO_NUMBER), OP (PT_SYS_TIME), OP (PT_SYS_TIMESTAMP), OP (PT_CURRENT_TIMESTAMP), OP (PT_SYS_DATETIME),
    OP (PT_CURRENT_DATETIME), OP (PT_CURRENT_TIME), OP (PT_CURRENT_DATE), OP (PT_UTC_TIME), OP (PT_UTC_DATE),
    OP (PT_TO_TIME), OP (PT_TO_TIMESTAMP), OP (PT_TO_DATETIME), OP (PT_CURRENT_VALUE), OP (PT_NEXT_VALUE),
    OP (PT_INST_NUM), OP (PT_ROWNUM), OP (PT_ORDERBY_NUM), OP (PT_CONNECT_BY_ISCYCLE), OP (PT_CONNECT_BY_ISLEAF),
    OP (PT_LEVEL), OP (PT_EXTRACT), OP (PT_LIKE_ESCAPE), OP (PT_CAST), OP (PT_CURRENT_USER),
    OP (PT_LOCAL_TRANSACTION_ID), OP (PT_FLOOR), OP (PT_CEIL), OP (PT_SIGN), OP (PT_ROUND),
    OP (PT_ABS), OP (PT_TRUNC), OP (PT_CHR), OP (PT_INSTR), OP (PT_LEAST),
    OP (PT_GREATEST), OP (PT_ENCRYPT), OP (PT_DECRYPT), OP (PT_LOG), OP (PT_EXP),
    OP (PT_SQRT), OP (PT_LEFT), OP (PT_RIGHT), OP (PT_MID), OP (PT_REVERSE),
    OP (PT_ACOS), OP (PT_ASIN), OP (PT_ATAN), OP (PT_COS), OP (PT_SIN),
    OP (PT_COT), OP (PT_TAN), OP (PT_DEGREES), OP (PT_RADIANS), OP (PT_PI),
    OP (PT_FORMAT), OP (PT_DISK_SIZE), OP (PT_LN), OP (PT_LOG2), OP (PT_LOG10),
    OP (PT_TIME_FORMAT), OP (PT_TIMESTAMP), OP (PT_UNIX_TIMESTAMP), OP (PT_FROM_UNIXTIME), OP (PT_SCHEMA),
    OP (PT_DATABASE), OP (PT_VERSION), OP (PT_ADDDATE), OP (PT_DATE_ADD), OP (PT_SUBDATE),
    OP (PT_DATE_SUB), OP (PT_DATE_FORMAT), OP (PT_DATEF), OP (PT_TIMEF), OP (PT_YEARF),
    OP (PT_MONTHF), OP (PT_DAYF), OP (PT_HOURF), OP (PT_MINUTEF), OP (PT_SECONDF),
    OP (PT_DAYOFMONTH), OP (PT_WEEKDAY), OP (PT_DAYOFWEEK), OP (PT_DAYOFYEAR), OP (PT_QUARTERF),
    OP (PT_TODAYS), OP (PT_FROMDAYS), OP (PT_TIMETOSEC), OP (PT_SECTOTIME), OP (PT_MAKEDATE),
    OP (PT_MAKETIME), OP (PT_WEEKF), OP (PT_USER), OP (PT_ROW_COUNT), OP (PT_LAST_INSERT_ID),
    OP (PT_STR_TO_DATE), OP (PT_LIST_DBS), OP (PT_LIKE_LOWER_BOUND), OP (PT_LIKE_UPPER_BOUND), OP (PT_BIT_TO_BLOB),
    OP (PT_BLOB_FROM_FILE), OP (PT_BLOB_LENGTH), OP (PT_BLOB_TO_BIT), OP (PT_CHAR_TO_BLOB), OP (PT_CHAR_TO_CLOB),
    OP (PT_CLOB_FROM_FILE), OP (PT_CLOB_LENGTH), OP (PT_CLOB_TO_CHAR), OP (PT_FUNCTION_HOLDER), OP (PT_INDEX_CARDINALITY),
    OP (PT_ESTIMATED_TABLE_ROWS), OP (PT_ESTIMATED_AVG_ROW_LENGTH), OP (PT_ESTIMATED_DATA_LENGTH), OP (PT_ESTIMATED_DATA_FREE), OP (PT_DEFINE_VARIABLE),
    OP (PT_EVALUATE_VARIABLE), OP (PT_EXEC_STATS), OP (PT_ADDTIME), OP (PT_BIN), OP (PT_HEX),
    OP (PT_ASCII), OP (PT_CONV), OP (PT_TO_ENUMERATION_VALUE), OP (PT_INET_ATON), OP (PT_INET_NTOA),
    OP (PT_CHARSET), OP (PT_COLLATION), OP (PT_WIDTH_BUCKET), OP (PT_TRACE_STATS), OP (PT_INDEX_PREFIX),
    OP (PT_AES_ENCRYPT), OP (PT_AES_DECRYPT), OP (PT_SHA_ONE), OP (PT_SHA_TWO), OP (PT_TO_BASE64),
    OP (PT_FROM_BASE64), OP (PT_SLEEP), OP (PT_SYS_GUID), OP (PT_UUID), OP (PT_UUID_FORMAT),
    OP (PT_DBTIMEZONE), OP (PT_SESSIONTIMEZONE), OP (PT_TZ_OFFSET), OP (PT_NEW_TIME), OP (PT_FROM_TZ),
    OP (PT_TO_DATETIME_TZ), OP (PT_TO_TIMESTAMP_TZ), OP (PT_UTC_TIMESTAMP), OP (PT_CRC32), OP (PT_SCHEMA_DEF),
    OP (PT_CONV_TZ), OP (PT_COLLECTION_TO_STRING)
  };

#undef OP

  struct classified
  {
    const char *name;
    PT_OP_TYPE op;
    op_class cls;
  };

  std::vector<classified> table ()
  {
    std::vector<classified> v;
    auto add =[&](const op_entry * b, size_t n, op_class c)
    {
      for (size_t i = 0; i < n; i++)
	{
	  v.push_back ({b[i].name, b[i].op, c});
	}
    };
#define ADD(arr, c) add (arr, sizeof (arr) / sizeof (arr[0]), c)
    ADD (T_LOGIC, C_LOGIC);
    ADD (T_NOT, C_NOT);
    ADD (T_IS_NULL, C_IS_NULL);
    ADD (T_IS, C_IS);
    ADD (T_BETWEEN, C_BETWEEN);
    ADD (T_RANGE, C_RANGE);
    ADD (T_QUANT, C_QUANT);
    ADD (T_EXISTS, C_EXISTS);
    ADD (T_CONCAT, C_CONCAT);
    ADD (T_STRICT2, C_STRICT2);
    ADD (T_STRICTN, C_STRICTN);
    ADD (T_ANY, C_ANY);
    ADD (T_DEFAULT, C_DEFAULT);
#undef ADD
    return v;
  }

  /* ------------------------------------------------------------ parse_tree.h of the revision under test */
  std::string parse_tree_h_path ()
  {
    std::string tu = unit_null_eval_tu_path ();	/* <wt>/src/optimizer/rewriter/<copy> */
    std::string d = tu.substr (0, tu.rfind ('/'));
    d = d.substr (0, d.rfind ('/'));
    d = d.substr (0, d.rfind ('/'));
    return d + "/parser/parse_tree.h";
  }

  /* the enumerator names from PT_AND up to (not including) PT_LAST_OPCODE; empty when not found */
  std::vector<std::string> header_opcodes (std::string &why)
  {
    std::ifstream f (parse_tree_h_path ());
    if (!f)
      {
	why = "cannot open " + parse_tree_h_path ();
	return {};
      }
    std::stringstream ss;
    ss << f.rdbuf ();
    std::string s = ss.str ();
    size_t b = s.find ("PT_AND = ");
    size_t e = s.find ("PT_LAST_OPCODE", b == std::string::npos ? 0 : b);
    if (b == std::string::npos || e == std::string::npos)
      {
	why = "PT_AND = .. PT_LAST_OPCODE not found in " + parse_tree_h_path ();
	return {};
      }
    std::string body = s.substr (b, e - b);
    body = std::regex_replace (body, std::regex ("/\\*[\\s\\S]*?\\*/"), " ");
    body = std::regex_replace (body, std::regex ("//[^\\n]*"), " ");
    std::vector<std::string> names;
    std::regex id ("\\b(PT_[A-Z0-9_]+)\\b\\s*(=[^,]*)?,");
    for (std::sregex_iterator it (body.begin (), body.end (), id), end; it != end; ++it)
      {
	names.push_back ((*it)[1].str ());
      }
    return names;
  }

  /* a list joined for the failure message: the harness report shows the expanded expression, so the names go there */
  std::string joined (const std::vector<std::string> &v)
  {
    std::string s;
    for (const std::string & e:v)
      {
	s += (s.empty ()? "" : ", ") + e;
      }
    return s;
  }

  /* ------------------------------------------------------------ nodes */
  const UINTPTR SPEC_T = 1;	/* the spec whose columns are NULL */
  const UINTPTR SPEC_O = 2;	/* another spec */

  struct arena
  {
    std::vector<PT_NODE *> nodes;
    ~arena ()
    {
      for (PT_NODE * n:nodes)
	{
	  free (n);
	}
    }
    PT_NODE *node (PT_NODE_TYPE t)
    {
      PT_NODE *n = (PT_NODE *) calloc (1, sizeof (PT_NODE));
      n->node_type = t;
      nodes.push_back (n);
      return n;
    }
    PT_NODE *col (UINTPTR spec = SPEC_T)
    {
      PT_NODE *n = node (PT_NAME);
      n->info.name.original = "x";
      n->info.name.spec_id = spec;
      n->info.name.meta_class = PT_NORMAL;
      n->type_enum = PT_TYPE_INTEGER;
      return n;
    }
    PT_NODE *ival (int v)
    {
      PT_NODE *n = node (PT_VALUE);
      n->type_enum = PT_TYPE_INTEGER;
      n->info.value.data_value.i = v;
      return n;
    }
    PT_NODE *sval (const char *)
    {
      PT_NODE *n = node (PT_VALUE);
      n->type_enum = PT_TYPE_CHAR;
      return n;
    }
    PT_NODE *nullv ()
    {
      PT_NODE *n = node (PT_VALUE);
      n->type_enum = PT_TYPE_NULL;
      return n;
    }
    PT_NODE *expr (PT_OP_TYPE op, PT_NODE * a1, PT_NODE * a2 = NULL, PT_NODE * a3 = NULL,
		   PT_TYPE_ENUM type = PT_TYPE_INTEGER)
    {
      PT_NODE *n = node (PT_EXPR);
      n->info.expr.op = op;
      n->info.expr.arg1 = a1;
      n->info.expr.arg2 = a2;
      n->info.expr.arg3 = a3;
      n->type_enum = type;
      return n;
    }
    PT_NODE *seq (PT_NODE * first)
    {
      PT_NODE *n = node (PT_FUNCTION);
      n->info.function.function_type = F_SEQUENCE;
      n->info.function.arg_list = first;
      return n;
    }
  };

  int T ()
  {
    return unit_null_eval_true;
  }
  int F ()
  {
    return unit_null_eval_false;
  }
  int U ()
  {
    return unit_null_eval_unknown;
  }
  int ANY ()
  {
    return T () | F () | U ();
  }

  std::string vals (int v)
  {
    std::string s;
    if (v & T ())
      s += "T";
    if (v & F ())
      s += "F";
    if (v & U ())
      s += "U";
    return s.empty ()? "-" : s;
  }

  int ev (PT_NODE * n)
  {
    return unit_null_eval (NULL, n, SPEC_T);
  }

  /* one probe: the expression and the values the class says it takes */
  struct probe
  {
    std::string what;
    int got, want;
  };

  /* probes that tell each class from the default branch ("UNKNOWN iff arg1 can only be UNKNOWN") */
  std::vector<probe> probes_for (arena & a, PT_OP_TYPE op, op_class c)
  {
    std::vector<probe> p;
    auto x =[&]()
    {
      return a.col ();
    };
    auto k =[&]()
    {
      return a.ival (1);
    };
    switch (c)
      {
      case C_LOGIC:
	/* (x = 1) op (1 = 1): AND {F,U}, OR {T,U}, XOR {U}; the default would be {U} */
	p.push_back ({"(x=1) op (1=1)", ev (a.expr (op, a.expr (PT_EQ, x (), k ()), a.expr (PT_EQ, k (), k ()))),
		     op == PT_AND ? (F () | U ()) : op == PT_OR ? (T () | U ()) : U ()});
	p.push_back ({"(x=1) op (x=1)", ev (a.expr (op, a.expr (PT_EQ, x (), k ()), a.expr (PT_EQ, x (), k ()))), U ()});
	break;
      case C_NOT:
	p.push_back ({"NOT (x=1)", ev (a.expr (op, a.expr (PT_EQ, x (), k ()))), U ()});
	p.push_back ({"NOT (x IS NULL)", ev (a.expr (op, a.expr (PT_IS_NULL, x ()))), F ()});
	break;
      case C_IS_NULL:
	p.push_back ({"x op", ev (a.expr (op, x ())), op == PT_IS_NULL ? T () : F ()});
	break;
      case C_IS:
	/* (x = 1) IS [NOT] UNKNOWN */
	p.push_back ({"(x=1) op UNKNOWN", ev (a.expr (op, a.expr (PT_EQ, x (), k ()), a.nullv ())),
		     op == PT_IS ? T () : F ()});
	break;
      case C_BETWEEN:
	p.push_back ({"x op 1 AND 2", ev (a.expr (op, x (), a.expr (PT_BETWEEN_AND, k (), a.ival (2)))), U ()});
	p.push_back ({"x op <not BETWEEN_AND>", ev (a.expr (op, x (), k ())), ANY ()});
	break;
      case C_RANGE:
	p.push_back ({"x RANGE (= 1)", ev (a.expr (op, x (), a.expr (PT_BETWEEN_EQ_NA, k ()))), U ()});
	p.push_back ({"x RANGE (<value>)", ev (a.expr (op, x (), k ())), ANY ()});
	break;
      case C_QUANT:
	p.push_back ({"x op {1, 2}", ev (a.expr (op, x (), a.seq (([&]()
								    {
								    PT_NODE * e = k (); e->next = a.ival (2); return e;}
								  ) ()))), U ()});
	p.push_back ({"x op <no set>", ev (a.expr (op, x (), NULL)), ANY ()});
	break;
      case C_EXISTS:
	{
	  PT_NODE *fw = a.ival (0);
	  fw->info.value.is_false_where = true;
	  p.push_back ({"EXISTS <false where>", ev (a.expr (op, fw)), F ()});
	  p.push_back ({"EXISTS x", ev (a.expr (op, x ())), ANY ()});
	}
	break;
      case C_CONCAT:
      case C_STRICT2:
	p.push_back ({"op (x, 1)", ev (a.expr (op, x (), k ())), U ()});
	p.push_back ({"op (1, x)", ev (a.expr (op, k (), x ())), U ()});
	p.push_back ({"op (1, 1)", ev (a.expr (op, k (), k ())), ANY ()});
	break;
      case C_STRICTN:
	p.push_back ({"op (x, 1, 1)", ev (a.expr (op, x (), k (), k ())), U ()});
	p.push_back ({"op (1, x, 1)", ev (a.expr (op, k (), x (), k ())), U ()});
	p.push_back ({"op (1, 1, x)", ev (a.expr (op, k (), k (), x ())), U ()});
	p.push_back ({"op (1, x)", ev (a.expr (op, k (), x ())), U ()});
	p.push_back ({"op (1, 1, 1)", ev (a.expr (op, k (), k (), k ())), ANY ()});
	break;
      case C_ANY:
	p.push_back ({"op (x, x, x)", ev (a.expr (op, x (), x (), x ())), ANY ()});
	break;
      case C_DEFAULT:
	p.push_back ({"op (x, 1, 1)", ev (a.expr (op, x (), k (), k ())), U ()});
	p.push_back ({"op (1, x, x)", ev (a.expr (op, k (), x (), x ())), ANY ()});
	p.push_back ({"op (x of another spec)", ev (a.expr (op, a.col (SPEC_O))), ANY ()});
	break;
      }
    return p;
  }
}

TEST_CASE ("PR8609 N1: every PT_OP_TYPE enumerator is in the reviewed NULL table (a new operator fails here by name)",
	   "[contract]")
{
  std::vector<classified> tab = table ();
  std::map<int, const char *> by_value;
  std::set<std::string> by_name;
  std::vector<std::string> dup;

  for (const classified & c:tab)
    {
      if (!by_value.emplace ((int) c.op, c.name).second || !by_name.insert (c.name).second)
	{
	  dup.push_back (c.name);
	}
    }
  INFO ("table operators: " << tab.size () << ", PT_LAST_OPCODE - PT_AND = " << (int) PT_LAST_OPCODE - (int) PT_AND);
  INFO ("listed twice: " << Catch::Detail::stringify (dup));
  CHECK (joined (dup) == "");

  /* by value: every opcode from PT_AND up to PT_LAST_OPCODE has a row */
  std::vector<int> missing_values;
  for (int v = (int) PT_AND; v < (int) PT_LAST_OPCODE; v++)
    {
      if (by_value.find (v) == by_value.end ())
	{
	  missing_values.push_back (v);
	}
    }

  /* by name: the header of this revision, for the message */
  std::string why;
  std::vector<std::string> hdr = header_opcodes (why);
  std::vector<std::string> missing_names;
  for (const std::string & n:hdr)
    {
      if (by_name.find (n) == by_name.end ())
	{
	  missing_names.push_back (n);
	}
    }

  INFO ("parse_tree.h: " << parse_tree_h_path () << " " << why);
  INFO ("operators not in the reviewed table -- decide their NULL behavior in qo_null_eval_expr () and add them to a "
	"class of unit/optimizer/query_rewrite_term.null_eval/test.cpp: " << Catch::Detail::stringify (missing_names));
  INFO ("opcode values not in the table: " << Catch::Detail::stringify (missing_values));
  CHECK (why == "");
  CHECK (!hdr.empty ());
  CHECK ((int) hdr.size () == (int) PT_LAST_OPCODE - (int) PT_AND);
  /* names of the operators to classify (a failure lists them) */
  CHECK (joined (missing_names) == "");
  CHECK (missing_values.size () == 0);
  CHECK ((int) tab.size () == (int) PT_LAST_OPCODE - (int) PT_AND);
}

TEST_CASE ("PR8609 N2: each operator behaves as its class in the table (table = qo_null_eval_expr switch)", "[contract]")
{
  std::vector<std::string> bad;
  int n = 0;

  for (const classified & c:table ())
    {
      arena a;
      for (const probe & p:probes_for (a, c.op, c.cls))
	{
	  n++;
	  if (p.got != p.want)
	    {
	      bad.push_back (std::string (c.name) + " [" + class_name (c.cls) + "] " + p.what + ": got " + vals (p.got)
			     + ", class says " + vals (p.want));
	    }
	}
    }
  INFO ("probes: " << n);
  INFO ("operators that do not behave as their class -- the switch moved them; move them in the table, or fix the "
	"switch: " << Catch::Detail::stringify (bad));
  CHECK (joined (bad) == "");
}

TEST_CASE ("PR8609 N3: representative predicates on a NULL column x", "[contract]")
{
  arena a;

  SECTION ("DATEDIFF (SYSDATE, x) < 30 is UNKNOWN only")
  {
    PT_NODE *dd = a.expr (PT_DATEDIFF, a.expr (PT_SYS_DATE, NULL), a.col ());
    CHECK (vals (ev (a.expr (PT_LT, dd, a.ival (30)))) == "U");
  }
  SECTION ("LOCATE ('b', x, 2) is UNKNOWN (x as the string)")
  {
    CHECK (vals (ev (a.expr (PT_LOCATE, a.sval ("b"), a.col (), a.ival (2)))) == "U");
  }
  SECTION ("LOCATE ('b', 'abc', x) is UNKNOWN (x as the third operand, the position)")
  {
    CHECK (vals (ev (a.expr (PT_LOCATE, a.sval ("b"), a.sval ("abc"), a.col ()))) == "U");
  }
  SECTION ("LOCATE ('b', x, 2) > 0 is UNKNOWN only")
  {
    CHECK (vals (ev (a.expr (PT_GT, a.expr (PT_LOCATE, a.sval ("b"), a.col (), a.ival (2)), a.ival (0)))) == "U");
  }
  SECTION ("COALESCE (x, 1) = 1 is any value")
  {
    CHECK (vals (ev (a.expr (PT_EQ, a.expr (PT_COALESCE, a.col (), a.ival (1)), a.ival (1)))) == "TFU");
  }
  SECTION ("x IS NULL is TRUE")
  {
    CHECK (vals (ev (a.expr (PT_IS_NULL, a.col ()))) == "T");
  }
  SECTION ("NOT (x = 1) is UNKNOWN")
  {
    CHECK (vals (ev (a.expr (PT_NOT, a.expr (PT_EQ, a.col (), a.ival (1))))) == "U");
  }
  SECTION ("x = 1 OR x IS NULL can be TRUE (no rejection)")
  {
    CHECK ((ev (a.expr (PT_OR, a.expr (PT_EQ, a.col (), a.ival (1)), a.expr (PT_IS_NULL, a.col ()))) & T ()) != 0);
  }
  SECTION ("a column of another spec is any value")
  {
    CHECK (vals (ev (a.expr (PT_EQ, a.col (SPEC_O), a.ival (1)))) == "TFU");
  }
}

TEST_CASE ("PR8609 N4: the default branch (fail-open) -- x in the first operand only proves UNKNOWN", "[contract]")
{
  arena a;

  /* PT_UPPER and PT_SUBSTRING are not listed: NULL when the first operand is NULL */
  CHECK (vals (ev (a.expr (PT_EQ, a.expr (PT_UPPER, a.col ()), a.sval ("A")))) == "U");
  CHECK (vals (ev (a.expr (PT_EQ, a.expr (PT_SUBSTRING, a.col (), a.ival (1), a.ival (2)), a.sval ("A")))) == "U");
  /* x only as a later operand of an unlisted operation: no claim, the prover does not reject */
  CHECK (vals (ev (a.expr (PT_EQ, a.expr (PT_SUBSTRING, a.sval ("abc"), a.col (), a.ival (2)), a.sval ("A"))))
	 == "TFU");
  /* a NULL literal first operand counts as NULL as well */
  CHECK (vals (ev (a.expr (PT_EQ, a.expr (PT_UPPER, a.nullv ()), a.sval ("A")))) == "U");
}
