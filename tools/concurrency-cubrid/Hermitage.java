import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.function.Function;

/**
 * Hermitage for CUBRID (dev4 tools/concurrency-cubrid) — fixed interleavings of two or three sessions, each provoking
 * one isolation anomaly in Adya's terms (G0, G1a, G1b, G1c, OTV, PMP, P4, G-single, G2-item, G2), after
 * github.com/ept/hermitage, plus two unique-key insert races. Every scenario runs at READ COMMITTED (4),
 * REPEATABLE READ (5) and SERIALIZABLE (6) over three access paths, so that MVCC visibility is checked through the
 * heap and through an index alike:
 *   pk    the table has its primary key only (the optimizer chooses)
 *   full  every statement with a WHERE reads the heap (USING INDEX NONE)
 *   vidx  a secondary index on v, forced on the predicates over v (USING INDEX i_hm_v(+))
 * A step still running after BLOCK_MS is "blocked" and the scenario goes on with the other sessions; the next step of
 * that session first waits for it (up to WAIT_MS, past the sessions' lock timeout — longer is a hang). The judge of a
 * scenario reads the observations and says ANOMALY or prevented. The expectation is the CUBRID manual
 * (en/sql/transaction.rst): RC takes a snapshot per statement and re-evaluates a row it waited for, RR is snapshot
 * isolation with write skew allowed, SERIALIZABLE is RR.
 *
 * usage: java -cp cubrid_jdbc.jar Hermitage.java <jdbc url> [scenario name prefix]
 * output: one line per run, the transcript of every run that is not as expected, a matrix per access path.
 * exit: 0 all as the manual says (allowed anomalies included), 1 a FAIL or HANG, 2 setup error.
 */
public class Hermitage {
    static final long BLOCK_MS = 1000;      // lock waits start at once; a statement running this long is blocked
    static final long WAIT_MS = 15000;      // past the session lock timeout: a blocked step must have ended by then
    static final int LOCK_TIMEOUT_S = 10;   // no statement waits forever on a lock (a lost wake-up shows as a timeout)
    static final int[] LEVELS = {4, 5, 6};
    static final String[] VARIANTS = {"pk", "full", "vidx"};

    record Step(String sess, String label, String sql) {}

    /** expect: per level RC, RR, SER — 'A' the manual allows the anomaly, 'P' it must be prevented. */
    record Scenario(String name, String what, List<Step> steps, String expect, Function<Map<String, String>, String> judge) {}

    static Step s(String sess, String label, String sql) {
        return new Step(sess, label, sql);
    }

    static String v(Map<String, String> o, String label) {
        String x = o.get(label);
        return x == null ? "" : x;
    }

    static boolean err(Map<String, String> o, String label) {
        return v(o, label).contains("err:");
    }

    static boolean done(Map<String, String> o, String... labels) {   // every step ran without an error or a hang
        for (String l : labels) {
            String x = v(o, l);
            if (x.isEmpty() || x.contains("err:") || x.contains("hang")) return false;
        }
        return true;
    }

    static List<Scenario> scenarios() {
        List<Scenario> l = new ArrayList<>();
        l.add(new Scenario("G0", "dirty write: two writers interleave on two rows", List.of(
                s("T1", "w1a", "update hm set v = 11 where k = 1"),
                s("T2", "w2a", "update hm set v = 12 where k = 1"),
                s("T1", "w1b", "update hm set v = 21 where k = 2"),
                s("T1", "c1", "commit"),
                s("T2", "w2b", "update hm set v = 22 where k = 2"),
                s("T2", "c2", "commit")), "PPP",
                o -> v(o, "final").equals("rows[1=11,2=21]") || v(o, "final").equals("rows[1=12,2=22]")
                        ? "prevented" : "ANOMALY final " + v(o, "final") + " mixes the two writers"));
        l.add(new Scenario("G1a", "aborted read", List.of(
                s("T1", "w1", "update hm set v = 101 where k = 1"),
                s("T2", "r1", "select k, v from hm where v > 0"),
                s("T1", "a1", "rollback"),
                s("T2", "r2", "select k, v from hm where v > 0"),
                s("T2", "c2", "commit")), "PPP",
                o -> v(o, "r1").contains("=101") || v(o, "r2").contains("=101") ? "ANOMALY T2 read the aborted 101" : "prevented"));
        l.add(new Scenario("G1b", "intermediate read", List.of(
                s("T1", "w1", "update hm set v = 101 where k = 1"),
                s("T2", "r1", "select k, v from hm where v > 0"),
                s("T1", "w2", "update hm set v = 11 where k = 1"),
                s("T1", "c1", "commit"),
                s("T2", "r2", "select k, v from hm where v > 0"),
                s("T2", "c2", "commit")), "PPP",
                o -> v(o, "r1").contains("=101") || v(o, "r2").contains("=101") ? "ANOMALY T2 read the intermediate 101" : "prevented"));
        l.add(new Scenario("G1c", "circular information flow", List.of(
                s("T1", "w1", "update hm set v = 11 where k = 1"),
                s("T2", "w2", "update hm set v = 22 where k = 2"),
                s("T1", "r1", "select k, v from hm where k = 2"),
                s("T2", "r2", "select k, v from hm where k = 1"),
                s("T1", "c1", "commit"),
                s("T2", "c2", "commit")), "PPP",
                o -> v(o, "r1").contains("2=22") || v(o, "r2").contains("1=11") ? "ANOMALY each read the other's uncommitted write" : "prevented"));
        l.add(new Scenario("OTV", "observed transaction vanishes", List.of(
                s("T1", "w1a", "update hm set v = 11 where k = 1"),
                s("T1", "w1b", "update hm set v = 19 where k = 2"),
                s("T2", "w2a", "update hm set v = 12 where k = 1"),
                s("T1", "c1", "commit"),
                s("T3", "r3a", "select k, v from hm where k = 1"),
                s("T2", "w2b", "update hm set v = 18 where k = 2"),
                s("T3", "r3b", "select k, v from hm where k = 2"),
                s("T2", "c2", "commit"),
                s("T3", "r3c", "select k, v from hm where k = 2"),
                s("T3", "r3d", "select k, v from hm where k = 1"),
                s("T3", "c3", "commit")), "PPP",
                o -> v(o, "r3b").contains("2=18") ? "ANOMALY T3 read T2's uncommitted 18"
                        : v(o, "r3c").contains("2=18") && v(o, "r3d").contains("1=11") ? "ANOMALY T3 saw T2 (2=18), then T1's 1=11" : "prevented"));
        l.add(new Scenario("PMP", "predicate-many-preceders: a phantom on a repeated predicate read", List.of(
                s("T1", "r1", "select k, v from hm where v = 30"),
                s("T2", "i2", "insert into hm values (3, 30)"),
                s("T2", "c2", "commit"),
                s("T1", "r2", "select k, v from hm where mod(v, 3) = 0"),
                s("T1", "c1", "commit")), "APP",
                o -> v(o, "r2").contains("3=30") ? "ANOMALY phantom 3=30" : "prevented"));
        l.add(new Scenario("PMP-write", "predicate-many-preceders with a write predicate", List.of(
                s("T1", "u1", "update hm set v = v + 10 where k > 0"),
                s("T2", "r2", "select k, v from hm where v = 20"),
                s("T2", "d2", "delete from hm where v = 20"),
                s("T1", "c1", "commit"),
                s("T2", "r3", "select k, v from hm where k > 0"),
                s("T2", "c2", "commit")), "APP",
                o -> err(o, "d2") || err(o, "c2") ? "prevented (T2 aborted)"
                        : v(o, "final").equals("rows[2=30]") || v(o, "final").equals("rows[1=20]") ? "prevented"
                        : "ANOMALY delete matched neither state, final " + v(o, "final")));
        l.add(new Scenario("P4", "lost update", List.of(
                s("T1", "r1", "select k, v from hm where k = 1"),
                s("T2", "r2", "select k, v from hm where k = 1"),
                s("T1", "u1", "update hm set v = 11 where k = 1"),
                s("T2", "u2", "update hm set v = 11 where k = 1"),
                s("T1", "c1", "commit"),
                s("T2", "c2", "commit")), "APP",
                o -> done(o, "u1", "c1", "u2", "c2") ? "ANOMALY both read 10 and both updated" : "prevented"));
        l.add(new Scenario("G-single", "read skew", List.of(
                s("T1", "r1", "select k, v from hm where k = 1"),
                s("T2", "r2a", "select k, v from hm where k = 1"),
                s("T2", "r2b", "select k, v from hm where k = 2"),
                s("T2", "u2a", "update hm set v = 12 where k = 1"),
                s("T2", "u2b", "update hm set v = 18 where k = 2"),
                s("T2", "c2", "commit"),
                s("T1", "r1b", "select k, v from hm where k = 2"),
                s("T1", "c1", "commit")), "APP",
                o -> v(o, "r1b").contains("2=18") ? "ANOMALY T1 read 1=10 before T2 and 2=18 after" : "prevented"));
        l.add(new Scenario("G-single-write", "read skew through a write predicate", List.of(
                s("T1", "r1", "select k, v from hm where k = 1"),
                s("T2", "u2a", "update hm set v = 12 where k = 1"),
                s("T2", "u2b", "update hm set v = 18 where k = 2"),
                s("T2", "c2", "commit"),
                s("T1", "d1", "delete from hm where v = 20"),
                s("T1", "r1b", "select k, v from hm where k > 0"),
                s("T1", "c1", "commit")), "APP",
                o -> err(o, "d1") || err(o, "c1") ? "prevented (T1 aborted)"
                        : v(o, "d1").endsWith("ok:1") ? "ANOMALY T1 deleted the row T2 had updated"
                        : "ANOMALY T1's delete saw T2's commit after T1 read 1=10"));
        l.add(new Scenario("G2-item", "write skew", List.of(
                s("T1", "r1", "select k, v from hm where k in (1, 2)"),
                s("T2", "r2", "select k, v from hm where k in (1, 2)"),
                s("T1", "u1", "update hm set v = 11 where k = 1"),
                s("T2", "u2", "update hm set v = 21 where k = 2"),
                s("T1", "c1", "commit"),
                s("T2", "c2", "commit")), "AAA",
                o -> done(o, "u1", "u2", "c1", "c2") ? "ANOMALY write skew" : "prevented"));
        l.add(new Scenario("G2", "anti-dependency cycle on a predicate", List.of(
                s("T1", "r1", "select k, v from hm where mod(v, 3) = 0"),
                s("T2", "r2", "select k, v from hm where mod(v, 3) = 0"),
                s("T1", "i1", "insert into hm values (3, 30)"),
                s("T2", "i2", "insert into hm values (4, 42)"),
                s("T1", "c1", "commit"),
                s("T2", "c2", "commit")), "AAA",
                o -> done(o, "i1", "i2", "c1", "c2") ? "ANOMALY both inserted a row the other's predicate covers" : "prevented"));
        l.add(new Scenario("U-commit", "same key inserted by two sessions, the first commits", List.of(
                s("T1", "i1", "insert into hm values (3, 30)"),
                s("T2", "i2", "insert into hm values (3, 31)"),
                s("T1", "c1", "commit"),
                s("T2", "c2", "commit")), "PPP",
                o -> err(o, "i2") ? "prevented" : "ANOMALY the second insert of key 3 succeeded, final " + v(o, "final")));
        l.add(new Scenario("U-rollback", "same key inserted by two sessions, the first rolls back", List.of(
                s("T1", "i1", "insert into hm values (3, 30)"),
                s("T2", "i2", "insert into hm values (3, 31)"),
                s("T1", "a1", "rollback"),
                s("T2", "c2", "commit")), "PPP",
                o -> err(o, "i2") ? "SPURIOUS key 3 was free after T1's rollback: " + v(o, "i2")
                        : v(o, "final").contains("3=31") ? "prevented" : "ANOMALY T2's insert is lost, final " + v(o, "final")));
        return l;
    }

    /** USING INDEX for the access-path variant; statements without a WHERE run as they are. */
    static String variant(String sql, String var) {
        String low = sql.toLowerCase();
        if (!low.contains(" where ") || low.startsWith("insert")) return sql;
        if (var.equals("full")) return sql + " using index none";
        if (var.equals("vidx") && low.matches(".* where v (=|>).*")) return sql + " using index i_hm_v(+)";
        return sql;
    }

    static String exec(Connection c, String sql) {
        String low = sql.trim().toLowerCase();
        try {
            if (low.equals("commit")) { c.commit(); return "ok"; }
            if (low.equals("rollback")) { c.rollback(); return "ok"; }
            try (Statement st = c.createStatement()) {
                if (st.execute(sql)) {
                    List<String> rows = new ArrayList<>();
                    try (ResultSet r = st.getResultSet()) {
                        while (r.next()) rows.add(r.getInt(1) + "=" + r.getInt(2));
                    }
                    Collections.sort(rows);
                    return "rows[" + String.join(",", rows) + "]";
                }
                return "ok:" + st.getUpdateCount();
            }
        } catch (SQLException e) {
            String m = String.valueOf(e.getMessage()).split("\n")[0];
            return "err:" + e.getErrorCode() + " " + (m.length() > 90 ? m.substring(0, 90) : m);
        }
    }

    static final class Sess {
        final Connection c;
        final ExecutorService ex;
        Future<String> pending;
        String pendingLabel;

        Sess(String name, Connection c) {
            this.c = c;
            this.ex = Executors.newSingleThreadExecutor(r -> {
                Thread t = new Thread(r, name);
                t.setDaemon(true);
                return t;
            });
        }
    }

    static Connection open(String url, int level) throws SQLException {
        Connection c = DriverManager.getConnection(url);
        c.setAutoCommit(false);
        try (Statement st = c.createStatement()) {
            st.execute("set transaction lock timeout " + LOCK_TIMEOUT_S);
        }
        c.commit();
        c.setTransactionIsolation(level == 4 ? Connection.TRANSACTION_READ_COMMITTED
                : level == 5 ? Connection.TRANSACTION_REPEATABLE_READ : Connection.TRANSACTION_SERIALIZABLE);
        return c;
    }

    static void reset(String url, String var) throws SQLException {
        try (Connection a = DriverManager.getConnection(url); Statement st = a.createStatement()) {
            a.setAutoCommit(true);
            st.execute("set transaction lock timeout " + LOCK_TIMEOUT_S);
            st.execute("drop table if exists hm");
            st.execute("create table hm (k int primary key, v int)");
            if (var.equals("vidx")) st.execute("create index i_hm_v on hm (v)");
            st.execute("insert into hm values (1, 10), (2, 20)");
        }
    }

    /** One scenario at one level and access path: the observations by step label, plus "final" and "hang". */
    static Map<String, String> run(String url, Scenario sc, int level, String var) throws Exception {
        reset(url, var);
        Map<String, Sess> ss = new LinkedHashMap<>();
        Map<String, String> o = new LinkedHashMap<>();
        try {
            for (Step st : sc.steps) {
                if (!ss.containsKey(st.sess)) ss.put(st.sess, new Sess(st.sess, open(url, level)));
            }
            for (Step st : sc.steps) {
                Sess x = ss.get(st.sess);
                if (x.pending != null) {
                    if (!settle(x, o)) { o.put("hang", st.sess + " stuck at " + x.pendingLabel); break; }
                }
                String sql = variant(st.sql, var);
                Future<String> f = x.ex.submit(() -> exec(x.c, sql));
                try {
                    o.put(st.label, f.get(BLOCK_MS, TimeUnit.MILLISECONDS));
                } catch (TimeoutException te) {
                    x.pending = f;
                    x.pendingLabel = st.label;
                    o.put(st.label, "blocked");
                }
            }
            for (Sess x : ss.values()) {
                if (x.pending != null && !settle(x, o)) o.put("hang", "stuck at " + x.pendingLabel);
            }
        } finally {
            for (Sess x : ss.values()) {
                Future<?> f = x.ex.submit(() -> { try { x.c.rollback(); x.c.close(); } catch (SQLException ignored) { } });
                try { f.get(WAIT_MS, TimeUnit.MILLISECONDS); } catch (TimeoutException te) { o.putIfAbsent("hang", "rollback stuck"); }
                x.ex.shutdownNow();
            }
        }
        try (Connection a = DriverManager.getConnection(url)) {
            o.put("final", exec(a, "select k, v from hm where k > 0"));
        }
        return o;
    }

    static boolean settle(Sess x, Map<String, String> o) throws Exception {
        try {
            o.put(x.pendingLabel, "blocked->" + x.pending.get(WAIT_MS, TimeUnit.MILLISECONDS));
            x.pending = null;
            return true;
        } catch (TimeoutException te) {
            o.put(x.pendingLabel, "hang");
            return false;
        }
    }

    public static void main(String[] args) throws Exception {
        if (args.length < 1) {
            System.err.println("usage: java -cp cubrid_jdbc.jar Hermitage.java <jdbc url> [scenario name prefix]");
            System.exit(2);
        }
        String url = args[0], only = args.length > 1 ? args[1] : "";
        Class.forName("cubrid.jdbc.driver.CUBRIDDriver");
        List<Scenario> scs = new ArrayList<>();
        for (Scenario sc : scenarios()) if (sc.name.startsWith(only)) scs.add(sc);
        Map<String, String> cell = new LinkedHashMap<>();
        int bad = 0;
        StringBuilder detail = new StringBuilder();
        for (String var : VARIANTS) {
            for (Scenario sc : scs) {
                for (int li = 0; li < LEVELS.length; li++) {
                    Map<String, String> o;
                    try {
                        o = run(url, sc, LEVELS[li], var);
                    } catch (SQLException e) {
                        System.out.println("SETUP ERROR " + sc.name + " " + var + " " + LEVELS[li] + ": " + e.getMessage());
                        System.exit(2);
                        return;
                    }
                    String verdict = o.containsKey("hang") ? "HANG " + o.get("hang") : sc.judge.apply(o);
                    boolean anomaly = verdict.startsWith("ANOMALY");
                    char exp = sc.expect.charAt(li);
                    String res = verdict.startsWith("HANG") ? "HANG"
                            : verdict.startsWith("SPURIOUS") ? "FAIL"
                            : anomaly ? (exp == 'A' ? "allowed" : "FAIL")
                            : (exp == 'P' ? "ok" : "stricter");
                    if (res.equals("FAIL") || res.equals("HANG")) bad++;
                    cell.put(var + "|" + sc.name + "|" + li, res);
                    System.out.printf("%-5s %-15s L%d  %-8s %s%n", var, sc.name, LEVELS[li], res, verdict);
                    {   // every transcript, so that a reader can check the judge; '!!' marks what is not as documented
                        String mark = res.equals("ok") || res.equals("allowed") ? "---" : "!!!";
                        detail.append(String.format("%n%s %s %s level %d: %s — %s%n", mark, var, sc.name, LEVELS[li], res, verdict));
                        for (Step st : sc.steps) {
                            detail.append(String.format("  %s %-4s %-62s -> %s%n", st.sess, st.label, variant(st.sql, var), v(o, st.label)));
                        }
                        detail.append("  final ").append(v(o, "final")).append('\n');
                    }
                }
            }
        }
        System.out.println(detail);
        for (String var : VARIANTS) {
            System.out.printf("%n[%s]  %-15s %-9s %-9s %-9s%n", var, "scenario", "RC(4)", "RR(5)", "SER(6)");
            for (Scenario sc : scs) {
                System.out.printf("       %-15s %-9s %-9s %-9s   expect %s  — %s%n", sc.name, cell.get(var + "|" + sc.name + "|0"),
                        cell.get(var + "|" + sc.name + "|1"), cell.get(var + "|" + sc.name + "|2"), sc.expect, sc.what);
            }
        }
        System.out.println(bad == 0 ? "\nALL AS DOCUMENTED" : "\n" + bad + " FAIL/HANG");
        System.exit(bad == 0 ? 0 : 1);
    }
}
