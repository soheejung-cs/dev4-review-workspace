/* appended to histogram_cl.cpp: reach the TU-static CHAR re-padding helpers (CBRD-27251, PR#7977)
 * and the constant probe-key extractor from the test.
 *
 * The suite also runs at revisions before CBRD-27251 (base comparison), where hist_repad_ctx,
 * histogram_repad_ctx () and histogram_repad_char_value () do not exist and histogram_extract_key ()
 * takes no column type. Every reference below is written ::name: qualified lookup consults the
 * using-directive for unitseam_absent only when the global namespace itself declares no such name,
 * so at a revision that has the real function the real one is called, and at one that does not the
 * fallback (returning absent_t) is found and the hook reports "absent" instead of failing to compile. */
#include <type_traits>
#include <utility>

namespace unitseam_absent
{
  struct absent_t
  {
  };
  struct hist_repad_ctx
  {
    DB_TYPE type;
    int precision;
    INTL_CODESET codeset;
  };
  template <typename... A> absent_t histogram_repad_char_value (A &&...);
  template <typename... A> absent_t histogram_repad_ctx (A &&...);
}
using namespace unitseam_absent;

template <typename Ctx>
static std::string_view
unitseam_repad_t (std::string_view value, const Ctx &ctx, std::string &buf, bool *present)
{
  using R = decltype (::histogram_repad_char_value (value, ctx, buf));
  if constexpr (std::is_same<R, unitseam_absent::absent_t>::value)
    {
      *present = false;
      return value;
    }
  else
    {
      *present = true;
      return ::histogram_repad_char_value (value, ctx, buf);
    }
}

/* histogram_repad_char_value (value, {type, precision, codeset}, buf) */
std::string_view
unitseam_repad (std::string_view value, int type, int precision, int codeset, std::string &buf, bool *present)
{
  const ::hist_repad_ctx ctx { (DB_TYPE) type, precision, (INTL_CODESET) codeset };

  return unitseam_repad_t (value, ctx, buf, present);
}

template <typename Reader>
static bool
unitseam_ctx_t (const Reader &reader, int collation_id, int *type, int *precision, int *codeset)
{
  using R = decltype (::histogram_repad_ctx (reader, collation_id));
  if constexpr (std::is_same<R, unitseam_absent::absent_t>::value)
    {
      return false;
    }
  else
    {
      const auto ctx = ::histogram_repad_ctx (reader, collation_id);
      *type = (int) ctx.type;
      *precision = ctx.precision;
      *codeset = (int) ctx.codeset;
      return true;
    }
}

/* histogram_repad_ctx (reader, collation_id), unpacked; false when the revision has none */
bool
unitseam_repad_ctx (const hist::HistogramReader &reader, int collation_id, int *type, int *precision, int *codeset)
{
  return unitseam_ctx_t (reader, collation_id, type, precision, codeset);
}

template <typename Reader>
static std::string_view
unitseam_via_reader_t (const Reader &reader, int collation_id, std::string_view value, std::string &buf,
		       bool *present)
{
  using R = decltype (::histogram_repad_ctx (reader, collation_id));
  if constexpr (std::is_same<R, unitseam_absent::absent_t>::value)
    {
      *present = false;
      return value;
    }
  else
    {
      /* what like_match_value / rlike_match_string do: one ctx per predicate, the real struct
       * handed to the real re-pad */
      const auto ctx = ::histogram_repad_ctx (reader, collation_id);
      *present = true;
      return ::histogram_repad_char_value (value, ctx, buf);
    }
}

std::string_view
unitseam_repad_via_reader (const hist::HistogramReader &reader, int collation_id, std::string_view value,
			   std::string &buf, bool *present)
{
  return unitseam_via_reader_t (reader, collation_id, value, buf, present);
}

template <typename F>
static bool
unitseam_extract_t (F f, const DB_VALUE *v, DB_TYPE column_type, hist::histogram_key &k, bool *by_column)
{
  if constexpr (std::is_invocable<F, const DB_VALUE *, DB_TYPE, hist::histogram_key &>::value)
    {
      *by_column = true;
      return f (v, column_type, k);
    }
  else
    {
      /* before CBRD-27251: (db_val, key) -- normalized by the constant's own type */
      *by_column = false;
      return f (v, k);
    }
}

/* the string probe key the planner builds for constant v against a column of column_type
 * (histogram_extract_key, called as histogram_get_equal/comp_selectivity call it) */
bool
unitseam_extract_str_key (const DB_VALUE *v, int column_type, std::string *out, bool *by_column)
{
  hist::histogram_key k;

  if (!unitseam_extract_t (&histogram_extract_key, v, (DB_TYPE) column_type, k, by_column)
      || k.kind != hist::histogram_key_kind::str)
    {
      return false;
    }
  *out = k.str;
  return true;
}
