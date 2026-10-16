/*
 * unitseam prelude — CUBRID globals a unit binary reads before any test runs, given the values a
 * booted process would have for an isolated test. Weak, so a test can override any of them. Linked
 * before the auto-stubs are generated, so these names never become zero-filled stubs.
 */

/* memory_monitor_sr.hpp: the server malloc wrapper routes through the memory monitor unless this is
 * set; the monitor object itself is not built into a unit binary */
__attribute__ ((weak)) bool mmon_disabled = true;
