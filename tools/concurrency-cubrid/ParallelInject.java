import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.ResultSetMetaData;
import java.sql.SQLException;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ThreadLocalRandom;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Parallel query error and cancel injection for CUBRID (dev4 tools/concurrency-cubrid).
 *
 * Errors raised inside parallel workers, and cancels arriving while workers run, travel from the worker threads back to
 * the main thread of the query (CBRD-28412 was such a path: a probe error of a parallel hash join in a parallel
 * subquery worker). The oracle needs no expected answer:
 *   error   a query that raises an error at one row (division by zero, a failed cast) or at many rows at once
 *           (smallint overflow) runs with the parallel operators off (hints NO_PARALLEL_SCAN NO_PARALLEL_HASH_JOIN
 *           NO_PARALLEL_SUBQUERY) and as planned; both must end in an error with the same code.
 *   cancel  a long parallel query is cancelled (Statement.cancel) or times out (setQueryTimeout) at a random moment;
 *           the same connection must then return the reference answer of a check query.
 *   check   parallel check queries must keep returning the answer they gave with the parallel operators off.
 * A writer thread updates, deletes and inserts rows of the scanned table in transactions that always roll back, so the
 * committed state — and every reference answer — stays fixed while scans meet uncommitted versions and vacuum.
 * The run script watches the server pid and the core directory (a debug server also asserts).
 *
 * usage: java -cp cubrid_jdbc.jar ParallelInject.java <jdbc url> <seconds> [client threads]
 * exit: 0 no finding, 1 a finding, 2 setup error.
 */
public class ParallelInject {
    static final int N_PT = 600_000;   // ~90 bytes a row: about 3,300 16K pages, well over parallel_scan_page_threshold 256
    static final int N_PU = 300_000;   // over the hash join threshold (256 pages) too
    static final String OFF = "NO_PARALLEL_SCAN NO_PARALLEL_HASH_JOIN NO_PARALLEL_SUBQUERY";
    static final int ERR_INTERRUPTED = -4;

    interface Q {
        String sql(boolean serial, long x);
    }

    record T(String name, Q q) {}

    static String hint(boolean serial, String more) {
        String h = (more + " " + (serial ? OFF : "")).trim();
        return h.isEmpty() ? "" : "/*+ " + h + " */ ";
    }

    static final List<T> CHECKS = List.of(
            new T("scan-agg", (s, x) -> "select " + hint(s, "") + "count(*), sum(a), min(s), max(g) from pt where mod(a, 7) <> 3"),
            new T("group", (s, x) -> "select " + hint(s, "") + "g, count(*), sum(a) from pt group by g order by g"),
            new T("hash-join", (s, x) -> "select " + hint(s, "USE_HASH") + "count(*), sum(pt.a + pu.c) from pt, pu where pt.id = pu.id and pu.c >= 0"),
            new T("subquery", (s, x) -> "select " + hint(s, "") + "count(*), sum(a) from pt where pt.g in (select pu.g from pu where pu.c > 50)"));

    // x: the id of the row that fails (1..N_PU, so that it is in both tables); 0 makes the query run clean (trace check)
    static final List<T> ERRORS = List.of(
            new T("div-scan", (s, x) -> "select " + hint(s, "") + "count(*) from pt where a / (id - " + x + ") >= 0"),
            new T("cast-scan", (s, x) -> "select " + hint(s, "") + "count(*) from pt where cast(case when id = " + x + " then 'bad' else '1' end as int) > 0"),
            new T("div-group", (s, x) -> "select " + hint(s, "") + "g, sum(a / (id - " + x + ")) from pt group by g"),
            new T("div-hash-join", (s, x) -> "select " + hint(s, "USE_HASH") + "count(*) from pt, pu where pt.id = pu.id and pt.a / (pu.id - " + x + ") >= 0"),
            new T("div-subquery", (s, x) -> "select " + hint(s, "") + "count(*) from pt where pt.g in (select pu.g from pu where pu.c / (pu.id - " + x + ") >= 0)"),
            // every row past 32767 overflows: the workers fail at once, each with its own error
            new T("overflow-many", (s, x) -> "select " + hint(s, "") + "count(*) from pt where cast("
                    + (x == 0 ? "mod(id, 30000)" : "id") + " as smallint) <> -1"));

    static String heavy(int limit) {
        return "select /*+ USE_HASH */ count(*), sum(a.a) from pt a, pu b where a.g = b.g and b.id < " + limit;
    }

    static final AtomicBoolean stop = new AtomicBoolean();
    static final Map<String, AtomicLong> count = new ConcurrentHashMap<>();
    static final List<String> findings = Collections.synchronizedList(new ArrayList<>());
    static final Map<String, String> reference = new ConcurrentHashMap<>();
    static String url;
    static int heavyLimit;

    static void inc(String k) {
        count.computeIfAbsent(k, z -> new AtomicLong()).incrementAndGet();
    }

    static void finding(String s) {
        inc("FINDING");
        if (findings.size() < 40) findings.add(s);
        System.out.println("FINDING " + s);
    }

    /** "rows:<all values>" or "err:<code> <message>". */
    static String outcome(Connection c, String sql, int timeoutS) {
        try (Statement st = c.createStatement()) {
            if (timeoutS > 0) st.setQueryTimeout(timeoutS);
            return result(st, sql);
        } catch (SQLException e) {
            return "err:" + e.getErrorCode() + " " + String.valueOf(e.getMessage()).split("\n")[0];
        }
    }

    static String result(Statement st, String sql) throws SQLException {
        StringBuilder b = new StringBuilder("rows:");
        try (ResultSet r = st.executeQuery(sql)) {
            ResultSetMetaData m = r.getMetaData();
            while (r.next()) {
                for (int i = 1; i <= m.getColumnCount(); i++) b.append(r.getString(i)).append(i < m.getColumnCount() ? "," : ";");
            }
        }
        return b.toString();
    }

    static int code(String outcome) {
        if (!outcome.startsWith("err:")) return 0;
        String n = outcome.substring(4).split(" ")[0];
        try { return Integer.parseInt(n); } catch (NumberFormatException e) { return 1; }
    }

    static Connection open() throws SQLException {
        Connection c = DriverManager.getConnection(url);
        c.setAutoCommit(true);
        c.setTransactionIsolation(Connection.TRANSACTION_READ_COMMITTED);
        return c;
    }

    static void load(Connection c) throws SQLException {
        try (Statement st = c.createStatement()) {
            try (ResultSet r = st.executeQuery("select count(*) from db_class where class_name in ('pt', 'pu')")) {
                r.next();
                if (r.getInt(1) == 2) {
                    try (ResultSet r2 = st.executeQuery("select count(*) from pt")) {
                        r2.next();
                        if (r2.getInt(1) == N_PT) return;
                    }
                }
            }
            st.execute("drop table if exists pt, pu");
            st.execute("create table pt (id int primary key, g int, a int, s varchar(64))");
            st.execute("create table pu (id int primary key, g int, c int)");
        }
        c.setAutoCommit(false);
        try (PreparedStatement p = c.prepareStatement("insert into pt values (?, ?, ?, ?)")) {
            for (int i = 1; i <= N_PT; i++) {
                p.setInt(1, i);
                p.setInt(2, i % 100);
                p.setInt(3, (int) ((i * 2654435761L) % 1000));
                p.setString(4, "r" + i + "-" + "x".repeat(i % 40));
                p.addBatch();
                if (i % 2000 == 0) { p.executeBatch(); c.commit(); }
            }
        }
        try (PreparedStatement p = c.prepareStatement("insert into pu values (?, ?, ?)")) {
            for (int i = 1; i <= N_PU; i++) {
                p.setInt(1, i);
                p.setInt(2, (i * 7) % 100);
                p.setInt(3, (int) ((i * 40503L) % 200) - 20);
                p.addBatch();
                if (i % 2000 == 0) { p.executeBatch(); c.commit(); }
            }
        }
        c.commit();
        c.setAutoCommit(true);
        try (Statement st = c.createStatement()) {
            st.execute("update statistics on pt with fullscan");
            st.execute("update statistics on pu with fullscan");
        }
    }

    /** Whether the query runs with parallel workers, from SHOW TRACE ("parallel workers: N", N > 0). */
    static String parallelOf(Connection c, String sql) {
        try (Statement st = c.createStatement()) {
            st.execute("set trace on");
            try (ResultSet r = st.executeQuery(sql)) { while (r.next()) { } }
            String t = "";
            try (ResultSet r = st.executeQuery("show trace")) { if (r.next()) t = r.getString(1); }
            st.execute("set trace off");
            java.util.regex.Matcher m = java.util.regex.Pattern.compile("parallel workers: ([1-9][0-9]*)").matcher(t);
            List<String> w = new ArrayList<>();
            while (m.find()) w.add(m.group(1));
            return w.isEmpty() ? "serial" : "parallel " + String.join("/", w);
        } catch (SQLException e) {
            return "trace error " + e.getErrorCode() + " " + e.getMessage();
        }
    }

    static void errorAction(Connection c) {
        T t = ERRORS.get(ThreadLocalRandom.current().nextInt(ERRORS.size()));
        long x = 1 + ThreadLocalRandom.current().nextInt(N_PU);
        String ser = outcome(c, t.q.sql(true, x), 0), par = outcome(c, t.q.sql(false, x), 0);
        int cs = code(ser), cp = code(par);
        if (cs == 0 && cp == 0) {
            inc("error:" + t.name + ":no-error-either");   // the template did not reach the failing row: not an engine finding
        } else if (cs == 0 || cp == 0 || cs != cp) {
            inc("error:" + t.name + ":MISMATCH");
            finding("error-path " + t.name + " x=" + x + ": serial " + abbrev(ser) + " / parallel " + abbrev(par) + "\n    " + t.q.sql(false, x));
        } else {
            inc("error:" + t.name + ":same" + cs);
        }
    }

    static void cancelAction(Connection c, ScheduledExecutorService sch) throws SQLException {
        boolean byTimeout = ThreadLocalRandom.current().nextInt(3) == 0;
        String o;
        AtomicBoolean running = new AtomicBoolean(true);
        java.util.concurrent.ScheduledFuture<?> pending = null;
        try (Statement st = c.createStatement()) {
            if (byTimeout) {
                st.setQueryTimeout(1);
            } else {
                long delay = 5 + ThreadLocalRandom.current().nextInt(1200);
                // a cancel that fires after the query ended must not reach the connection's next statement
                pending = sch.schedule(() -> {
                    if (running.get()) { try { st.cancel(); } catch (SQLException ignored) { } }
                }, delay, TimeUnit.MILLISECONDS);
            }
            try {
                o = result(st, heavy(heavyLimit));
            } catch (SQLException e) {
                o = "err:" + e.getErrorCode() + " " + String.valueOf(e.getMessage()).split("\n")[0];
            } finally {
                running.set(false);
                if (pending != null) pending.cancel(false);
            }
        }
        int cd = code(o);
        inc((byTimeout ? "timeout" : "cancel") + (cd == 0 ? ":completed" : ":err" + cd));
        if (cd != 0 && cd != ERR_INTERRUPTED && !o.toLowerCase().contains("timeout") && !o.toLowerCase().contains("interrupt")) {
            finding((byTimeout ? "timeout" : "cancel") + " ended in an unexpected error: " + abbrev(o));
        }
        T t = CHECKS.get(0);
        String after = outcome(c, t.q.sql(false, 0), 0);
        if (code(after) == ERR_INTERRUPTED) {   // a cancel already in flight when the query ended: count it, ask again
            inc("cancel:landed-on-next");
            after = outcome(c, t.q.sql(false, 0), 0);
        }
        if (!after.equals(reference.get(t.name))) {
            finding("after a " + (byTimeout ? "timeout" : "cancel") + " (" + abbrev(o) + ") the same connection answers " + t.name
                    + " with " + abbrev(after) + " instead of " + abbrev(reference.get(t.name)));
        }
    }

    static void checkAction(Connection c) {
        T t = CHECKS.get(ThreadLocalRandom.current().nextInt(CHECKS.size()));
        String o = outcome(c, t.q.sql(false, 0), 0);
        inc("check:" + t.name);
        if (!o.equals(reference.get(t.name))) finding("check " + t.name + " answered " + abbrev(o) + " instead of " + abbrev(reference.get(t.name)));
    }

    static void writer() {
        try (Connection c = open()) {
            c.setAutoCommit(false);
            while (!stop.get()) {
                int r = 1 + ThreadLocalRandom.current().nextInt(N_PT - 3000);
                try (Statement st = c.createStatement()) {
                    switch (ThreadLocalRandom.current().nextInt(3)) {
                        case 0 -> st.executeUpdate("update pt set a = a + 1, s = s || 'w' where id between " + r + " and " + (r + 2000));
                        case 1 -> st.executeUpdate("delete from pt where id between " + r + " and " + (r + 1000));
                        default -> st.executeUpdate("insert into pt select id + 10000000, g, a, s from pt where id between " + r + " and " + (r + 1000));
                    }
                    Thread.sleep(ThreadLocalRandom.current().nextInt(50));
                    inc("writer:rolled-back");
                } catch (SQLException e) {
                    inc("writer:err" + e.getErrorCode());
                } catch (InterruptedException e) {
                    return;
                } finally {
                    try { c.rollback(); } catch (SQLException ignored) { }
                }
            }
        } catch (SQLException e) {
            finding("writer could not connect: " + e.getMessage());
        }
    }

    static String abbrev(String s) {
        if (s == null) return "null";
        return s.length() > 160 ? s.substring(0, 160) + "…(" + s.length() + ")" : s;
    }

    public static void main(String[] args) throws Exception {
        if (args.length < 2) {
            System.err.println("usage: java -cp cubrid_jdbc.jar ParallelInject.java <jdbc url> <seconds> [client threads]");
            System.exit(2);
        }
        url = args[0];
        long seconds = Long.parseLong(args[1]);
        int threads = args.length > 2 ? Integer.parseInt(args[2]) : 6;
        Class.forName("cubrid.jdbc.driver.CUBRIDDriver");
        try (Connection c = open()) {
            long t0 = System.nanoTime();
            load(c);
            System.out.printf("data ready (%.0f s)%n", (System.nanoTime() - t0) / 1e9);
            System.out.println("== plans (SHOW TRACE) and references");
            for (T t : CHECKS) {
                String ser = outcome(c, t.q.sql(true, 0), 0), par = outcome(c, t.q.sql(false, 0), 0);
                System.out.printf("  %-14s %-16s %s%n", t.name, parallelOf(c, t.q.sql(false, 0)), abbrev(par));
                if (!ser.equals(par)) finding("check " + t.name + " differs before any injection: serial " + abbrev(ser) + " / parallel " + abbrev(par));
                reference.put(t.name, par);
            }
            for (T t : ERRORS) {
                System.out.printf("  %-14s %-16s (run clean; x = 0)%n", t.name, parallelOf(c, t.q.sql(false, 0)));
            }
            heavyLimit = 300;
            for (int i = 0; i < 6; i++) {   // the cancel target should run about 1.5 s so that cancels land while workers run
                long s = System.nanoTime();
                outcome(c, heavy(heavyLimit), 0);
                double sec = (System.nanoTime() - s) / 1e9;
                if (sec > 1.0 && sec < 3.0) break;
                heavyLimit = (int) Math.max(10, Math.min(N_PU, heavyLimit * 1.5 / Math.max(sec, 0.05)));
            }
            System.out.printf("  %-14s %-16s limit %d%n", "cancel-target", parallelOf(c, heavy(heavyLimit)), heavyLimit);
        }
        ScheduledExecutorService sch = Executors.newScheduledThreadPool(2);
        Thread w = new Thread(ParallelInject::writer, "writer");
        w.start();
        List<Thread> ts = new ArrayList<>();
        long end = System.currentTimeMillis() + seconds * 1000;
        for (int i = 0; i < threads; i++) {
            Thread t = new Thread(() -> {
                try (Connection c = open()) {
                    while (System.currentTimeMillis() < end) {
                        int r = ThreadLocalRandom.current().nextInt(10);
                        if (r < 4) errorAction(c);
                        else if (r < 7) cancelAction(c, sch);
                        else checkAction(c);
                    }
                } catch (SQLException e) {
                    finding("client lost its connection: " + e.getErrorCode() + " " + e.getMessage());
                }
            }, "client-" + i);
            t.start();
            ts.add(t);
        }
        for (Thread t : ts) t.join();
        stop.set(true);
        w.join(30_000);
        sch.shutdownNow();
        try (Connection c = open()) {
            for (T t : CHECKS) {
                String o = outcome(c, t.q.sql(false, 0), 0);
                if (!o.equals(reference.get(t.name))) finding("final check " + t.name + " answered " + abbrev(o));
            }
        }
        System.out.println("== counts");
        for (Map.Entry<String, AtomicLong> e : new TreeMap<>(count).entrySet()) System.out.printf("  %-34s %d%n", e.getKey(), e.getValue().get());
        System.out.println(findings.isEmpty() ? "NO FINDING" : findings.size() + " FINDING(S) — see the FINDING lines");
        System.exit(findings.isEmpty() ? 0 : 1);
    }
}
