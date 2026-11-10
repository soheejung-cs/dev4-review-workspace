/*
 * unitseam appendix for src/query/arithmetic.c (PR#8474, CBRD-27843) -- compiled at the end of the TU.
 *
 * round_bigint () is static and exists only from dd9d27e7f on; this file is appended at every revision. The probe
 * finds it by ordinary lookup at this point of the TU (the PR declares it at the top of the file). Where it does not
 * exist the call below has dependent arguments only (DB_BIGINT is a fundamental type, so no argument dependent
 * lookup either), its substitution fails and the probe answers false -- the test then does not register the
 * round_bigint cases (a base run lists only what develop has).
 */
#include <type_traits>
#include <utility>

namespace unit_integer_round
{
  template <typename T, typename = void>
  struct has_round_bigint : std::false_type
  {
  };

  template <typename T>
  struct has_round_bigint <T, decltype ((void) round_bigint (std::declval<T> (), std::declval<T> (),
					 std::declval<T *> ()))> : std::true_type
  {
  };

  template <typename T>
  static bool call_round_bigint (T num, T place, T * result, std::true_type)
  {
    return round_bigint (num, place, result);
  }

  template <typename T>
  static bool call_round_bigint (T, T, T * result, std::false_type)
  {
    *result = 0;
    return false;
  }
}

/* constant-initialized: the test reads it from its static initializers to decide what to register */
extern const bool unit_round_bigint_present = unit_integer_round::has_round_bigint<DB_BIGINT>::value;

bool
unit_round_bigint (DB_BIGINT num, DB_BIGINT place, DB_BIGINT * result)
{
  return unit_integer_round::call_round_bigint<DB_BIGINT> (num, place, result,
							    unit_integer_round::has_round_bigint<DB_BIGINT> ());
}
