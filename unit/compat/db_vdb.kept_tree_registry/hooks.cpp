/* appended to db_vdb.c (same TU): reach the static registry of kept SQL-level prepared trees (db_Kept_trees) and
 * its static functions from the test.
 *
 * The suite runs at two shapes of the registry:
 *   PR#8314 head (71916dad2~): db_prepared_tree_take (unlink; the caller owns it) + db_prepared_tree_put_back,
 *                              db_Kept_trees CUB_THREAD_LOCAL
 *   before it (cbe8d181b):     db_prepared_tree_lookup (unlink, re-link at the front -- stays on the list),
 *                              db_Kept_trees a plain static
 * Every reference to a function only one shape has is written ::name inside a template: qualified lookup consults the
 * using-directive for unitseam_absent only when the global namespace declares no such name, so the real function is
 * called where it exists and the fallback (returning absent_t) is found where it does not -- the hook then reports
 * "absent at this revision" instead of failing to compile (same device as optimizer/histogram_cl.char_keys).
 *
 * unitseam_exec_* model the EXECUTE choreography around the kept tree in db_execute_and_keep_statement_local, line by
 * line from each shape (head db_vdb.c:3330-3378, cbe8d181b db_vdb.c:3302-3340):
 *   checkout     head: kept = db_prepared_tree_take (name, text)       base: kept = db_prepared_tree_lookup (name, text)
 *   return_ok    head: db_prepared_tree_put_back (kept)                base: (nothing -- it never left the list)
 *                (err >= 0, and the "genuine execution error" return: both put back at head, both nothing at base)
 *   stale        head: signature_miss ? put_back (kept) : close (kept)  base: !signature_miss ? remove (name) : nothing
 * The shape is told by which of take / lookup exists. */
#include <type_traits>
#include <utility>

namespace unitseam_absent
{
  struct absent_t
  {
  };
  template <typename... A> absent_t db_prepared_tree_take (A &&...);
  template <typename... A> absent_t db_prepared_tree_put_back (A &&...);
  template <typename... A> absent_t db_prepared_tree_lookup (A &&...);
}
using namespace unitseam_absent;

template <typename N>
static DB_SESSION *
unitseam_take_t (N name, N text, bool *present)
{
  using R = decltype (::db_prepared_tree_take (name, text));
  if constexpr (std::is_same<R, unitseam_absent::absent_t>::value)
    {
      *present = false;
      return NULL;
    }
  else
    {
      *present = true;
      return ::db_prepared_tree_take (name, text);
    }
}

template <typename N>
static DB_SESSION *
unitseam_lookup_t (N name, N text, bool *present)
{
  using R = decltype (::db_prepared_tree_lookup (name, text));
  if constexpr (std::is_same<R, unitseam_absent::absent_t>::value)
    {
      *present = false;
      return NULL;
    }
  else
    {
      *present = true;
      return ::db_prepared_tree_lookup (name, text);
    }
}

template <typename S>
static void
unitseam_put_back_t (S kept, bool *present)
{
  using R = decltype (::db_prepared_tree_put_back (kept));
  if constexpr (std::is_same<R, unitseam_absent::absent_t>::value)
    {
      *present = false;
    }
  else
    {
      *present = true;
      ::db_prepared_tree_put_back (kept);
    }
}

DB_SESSION *
unitseam_take (const char *name, const char *text, bool *present)
{
  return unitseam_take_t<const char *> (name, text, present);
}

DB_SESSION *
unitseam_lookup (const char *name, const char *text, bool *present)
{
  return unitseam_lookup_t<const char *> (name, text, present);
}

void
unitseam_put_back (DB_SESSION * kept, bool *present)
{
  unitseam_put_back_t<DB_SESSION *> (kept, present);
}

/* 1 = head shape (take / put_back), 0 = base shape (lookup) -- decided at compile time, nothing is called */
template <typename N>
static constexpr bool
unitseam_has_take_t (void)
{
  return !std::is_same<decltype (::db_prepared_tree_take (std::declval<N> (), std::declval<N> ())),
		       unitseam_absent::absent_t>::value;
}

int
unitseam_shape (void)
{
  return unitseam_has_take_t<const char *> () ? 1 : 0;
}

/* the EXECUTE choreography (see the head comment) */
DB_SESSION *
unitseam_exec_checkout (const char *name, const char *text)
{
  bool p = false;
  if (unitseam_shape () == 1)
    {
      return unitseam_take_t<const char *> (name, text, &p);
    }
  return unitseam_lookup_t<const char *> (name, text, &p);
}

void
unitseam_exec_return_ok (DB_SESSION * kept)
{
  bool p = false;
  if (unitseam_shape () == 1)
    {
      unitseam_put_back_t<DB_SESSION *> (kept, &p);
    }
}

void
unitseam_exec_stale (DB_SESSION * kept, const char *name, bool signature_miss)
{
  bool p = false;
  if (unitseam_shape () == 1)
    {
      if (signature_miss)
	{
	  unitseam_put_back_t<DB_SESSION *> (kept, &p);
	}
      else
	{
	  db_prepared_tree_close (kept);
	}
    }
  else if (!signature_miss)
    {
      db_prepared_tree_remove (name);
    }
}

/* functions both shapes have */
DB_SESSION *
unitseam_unlink (const char *name)
{
  return db_prepared_tree_unlink (name);
}

void
unitseam_close (DB_SESSION * kept)
{
  db_prepared_tree_close (kept);
}

void
unitseam_remove (const char *name)
{
  db_prepared_tree_remove (name);
}

bool
unitseam_register (DB_SESSION * owner, const char *name, DB_SESSION * subsession)
{
  return db_prepared_tree_register (owner, name, subsession);
}

/* the calling thread's list (head: CUB_THREAD_LOCAL; base: the one process-wide list) */
DB_SESSION *
unitseam_list_head (void)
{
  return db_Kept_trees;
}

/* forget the list without closing (test teardown; the sessions belong to the test's pool) */
void
unitseam_list_forget (void)
{
  db_Kept_trees = NULL;
}
