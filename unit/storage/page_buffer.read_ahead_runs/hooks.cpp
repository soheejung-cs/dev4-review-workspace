/* ---- unitseam appendix (PR#8490 pgbuf_read_ahead suite): the page buffer's hash table, from inside page_buffer.c ----
 * pgbuf_Pool is static here.  The test gives the pool a zeroed hash table of PGBUF_HASH_SIZE anchors and puts BCBs of
 * the pages "in the buffer" at the head of their chains, the way pgbuf_insert_into_hash_chain () links them.  No
 * mutex is taken: pgbuf_read_ahead () walks the chains without one. */

/* pgbuf_is_temporary_volume () reads LOG_ISRESTARTED () (log_Gl.rcv_phase): log_Gl is the unit binary's 4 KiB zeroed
 * stub, so the field must lie inside it (LOG_RESTARTED == 0: restarted, xdisk_get_purpose () decides) */
static_assert (offsetof (LOG_GLOBAL, rcv_phase) + sizeof (LOG_RECVPHASE) <= 4096, "log_Gl stub too small for rcv_phase");

#define UNIT_MAX_BCBS 256
static PGBUF_BUFFER_HASH *unit_hash_table;
static PGBUF_BCB *unit_bcbs[UNIT_MAX_BCBS];
static unsigned int unit_touched[UNIT_MAX_BCBS];
static int unit_n_bcbs;

void
unit_pgbuf_reset (void)
{
  int i;

  if (unit_hash_table == NULL)
    {
      unit_hash_table = (PGBUF_BUFFER_HASH *) calloc (PGBUF_HASH_SIZE, sizeof (PGBUF_BUFFER_HASH));
    }
  for (i = 0; i < unit_n_bcbs; i++)
    {
      unit_hash_table[unit_touched[i]].hash_next = NULL;
      free (unit_bcbs[i]);
    }
  unit_n_bcbs = 0;
  pgbuf_Pool.buf_hash_table = unit_hash_table;
}

/* a BCB holding this page, at the head of its hash chain */
void
unit_pgbuf_put (short volid, int pageid)
{
  PGBUF_BCB *bcb = (PGBUF_BCB *) calloc (1, sizeof (PGBUF_BCB));
  unsigned int h;

  if (unit_n_bcbs >= UNIT_MAX_BCBS)
    {
      abort ();
    }
  bcb->vpid.volid = volid;
  bcb->vpid.pageid = pageid;
  h = PGBUF_HASH_VALUE (&bcb->vpid);
  bcb->hash_next = unit_hash_table[h].hash_next;
  unit_hash_table[h].hash_next = bcb;
  unit_bcbs[unit_n_bcbs] = bcb;
  unit_touched[unit_n_bcbs] = h;
  unit_n_bcbs++;
}

/* the hash chain value of a page (pages 2^20 apart on one volume share a chain) */
unsigned int
unit_pgbuf_hash (short volid, int pageid)
{
  VPID vpid;

  vpid.volid = volid;
  vpid.pageid = pageid;
  return PGBUF_HASH_VALUE (&vpid);
}

int
unit_pgbuf_chain_length (short volid, int pageid)
{
  int n = 0;
  PGBUF_BCB *b;

  for (b = unit_hash_table[unit_pgbuf_hash (volid, pageid)].hash_next; b != NULL; b = b->hash_next)
    {
      n++;
    }
  return n;
}
