/*
 * unitseam appendix for src/transaction/locator_sr.c (compiled at the end of that TU, SERVER_MODE) - for
 * unit/transaction/locator_sr.check_foreign_key.
 *
 * locator_check_foreign_key () is static: the test reaches it through this wrapper. Nothing else of the TU is
 * touched - every function it calls lives in another TU (heap_file.c, partition.c, btree.c, object_primitive.c,
 * log_tran_table.c, error_manager.c, memory_alloc.c) and is a fake in test.cpp.
 */
int
unitseam_locator_check_foreign_key (THREAD_ENTRY * thread_p, HFID * hfid, OID * class_oid, OID * inst_oid,
				    RECDES * recdes, RECDES * new_recdes, bool * is_cached, LC_COPYAREA ** copyarea)
{
  return locator_check_foreign_key (thread_p, hfid, class_oid, inst_oid, recdes, new_recdes, is_cached, copyarea);
}
