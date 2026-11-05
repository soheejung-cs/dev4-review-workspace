#!/bin/bash
# run_matrix.sh [time-limit-s] — the standard matrix (20 runs, about an hour at 90 s): workloads x isolation x faults.
# Expected models: READ COMMITTED -> read-committed; REPEATABLE READ and SERIALIZABLE -> snapshot-isolation (the manual
# says CUBRID's SERIALIZABLE is REPEATABLE READ and allows write skew). b-rc-rmw is a control: it must FAIL
# (wrong-total), RC does not prevent read-modify-write lost updates.
T=${1:-90}; cd "$(dirname "$0")"
C="--time-limit $T --concurrency 10 --rate 50 --nemesis-interval 15"
r() { ./run_one.sh "$@"; }
r a-rc-none    --workload append --isolation read-committed  --expect read-committed     $C
r a-rc-kill    --workload append --isolation read-committed  --expect read-committed     $C --faults kill
r a-rc-pause   --workload append --isolation read-committed  --expect read-committed     $C --faults pause
r a-rr-none    --workload append --isolation repeatable-read --expect snapshot-isolation $C
r a-rr-kill    --workload append --isolation repeatable-read --expect snapshot-isolation $C --faults kill
r a-rr-pause   --workload append --isolation repeatable-read --expect snapshot-isolation $C --faults pause
r a-rr-cas     --workload append --isolation repeatable-read --expect snapshot-isolation $C --faults kill-cas
r a-ser-none   --workload append --isolation serializable    --expect serializable       $C
r a-ser-kill   --workload append --isolation serializable    --expect snapshot-isolation $C --faults kill
r w-rc-none    --workload wr     --isolation read-committed  --expect read-committed     $C
r w-rr-none    --workload wr     --isolation repeatable-read --expect snapshot-isolation $C
r w-rr-kill    --workload wr     --isolation repeatable-read --expect snapshot-isolation $C --faults kill
r b-rr-none    --workload bank   --isolation repeatable-read --bank-mode rmw    $C
r b-rr-kill    --workload bank   --isolation repeatable-read --bank-mode rmw    $C --faults kill
r b-rr-pause   --workload bank   --isolation repeatable-read --bank-mode rmw    $C --faults pause
r b-rc-at-kill --workload bank   --isolation read-committed  --bank-mode atomic $C --faults kill
r b-rc-rmw     --workload bank   --isolation read-committed  --bank-mode rmw    $C
r s-rc-kill    --workload set    --isolation read-committed  $C --faults kill
r s-rr-kp      --workload set    --isolation repeatable-read $C --faults kill,pause
r s-rc-cas     --workload set    --isolation read-committed  $C --faults kill-cas
. ./env.sh; echo "matrix done $(date +%T)" >> "$JEPSEN_CUBRID_OUT/summary.txt"
