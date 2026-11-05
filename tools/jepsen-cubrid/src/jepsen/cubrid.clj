(ns jepsen.cubrid
  "Jepsen tests for one CUBRID node.

  Clients speak JDBC through the broker (CAS) like applications do; faults are local process signals on the
  cub_server of the database under test (kill -9 and restart, SIGSTOP/SIGCONT) and on one broker's CAS processes, so
  the test runs with --no-ssh against the active install (`~/CUBRID`).

  The database is the test's own: created under --db-dir when absent, deleted by teardown (at the start of a run, so
  each run begins empty, and at the end unless --leave-db-running). A database of the same name registered outside
  --db-dir is used as it is and never deleted. Workloads:

    append  Elle list-append transactions over 4 tables -- isolation anomalies (G0, G1a-c, G-single, G2 ...)
    wr      Elle rw-register transactions (plain UPDATE, INSERT when absent) -- lost updates and the same cycles
    bank    transfers between accounts and full reads -- the total must never change, balances stay >= 0
    set     unique inserts and reads -- every acknowledged insert must survive (crash recovery, durability)"
  (:require [clojure.java.shell :as shell]
            [clojure.string :as str]
            [clojure.tools.logging :refer [info warn]]
            [jepsen [checker :as checker]
                    [cli :as cli]
                    [client :as client]
                    [db :as db]
                    [generator :as gen]
                    [nemesis :as nemesis]
                    [os :as os]
                    [tests :as tests]]
            [jepsen.checker.timeline :as timeline]
            [jepsen.util :as util]
            [jepsen.nemesis.combined :as nc]
            [jepsen.tests.bank :as bank]
            [jepsen.tests.cycle.append :as append]
            [jepsen.tests.cycle.wr :as wr])
  (:import (java.sql Connection DriverManager SQLException)))

;; ---------------------------------------------------------------- local processes

(defn sh!
  "Runs a local command; returns {:exit :out :err}. The JVM inherits the CUBRID environment of the shell that
  started lein (CUBRID, CUBRID_DATABASES, PATH, LD_LIBRARY_PATH)."
  [& args]
  (apply shell/sh args))

(defn daemon!
  "Runs a command that starts a daemon (cubrid server/broker start). Its output goes to a file: through
  clojure.java.shell the daemon would inherit the stdout pipe and sh would wait for EOF forever."
  [& args]
  (let [dir (str (or (System/getenv "JEPSEN_CUBRID_OUT") (System/getProperty "user.dir")) "/logs")]
    (shell/sh "bash" "-c" (str "mkdir -p " dir " && " (str/join " " args) " >> " dir "/daemon.log 2>&1"))))

(defn server-pid
  "The pid of the cub_server serving dbname, from `cubrid server status` (the master's registry), or nil."
  [dbname]
  (some->> (:out (sh! "cubrid" "server" "status"))
           (re-find (re-pattern (str "Server " dbname " \\(rel [^,]+, pid (\\d+)\\)")))
           second))

(defn cas-pids
  "Pids of the CAS processes of one broker. Every broker's CAS has the executable name cub_cas; the broker shows in
  argv[0] (`broker1_cub_cas_3`, `query_editor_cub_cas_1`) -- an idle broker's CAS serves none of our clients."
  [broker]
  (->> (:out (sh! "pgrep" "-x" "cub_cas"))
       str/split-lines
       (remove str/blank?)
       (filter (fn [p]
                 (try (str/starts-with? (slurp (str "/proc/" p "/cmdline")) (str broker "_cub_cas_"))
                      (catch Exception _ false))))))

(defn registered-path
  "The volume path databases.txt registers for dbname, or nil."
  [dbname]
  (let [dir (or (System/getenv "CUBRID_DATABASES") (str (System/getenv "CUBRID") "/databases"))
        f   (java.io.File. (str dir "/databases.txt"))]
    (when (.exists f)
      (some (fn [line]
              (let [[n p] (str/split (str/trim line) #"\s+")]
                (when (= n dbname) p)))
            (str/split-lines (slurp f))))))

(defn owned?
  "The database is this tool's to delete: registered under --db-dir."
  [test]
  (when-let [p (registered-path (:dbname test))]
    (str/starts-with? p (str (:db-dir test) "/"))))

(defn wait-for-server
  "Waits until the master lists the server; returns its pid or nil after timeout-s seconds."
  [dbname timeout-s]
  (loop [i 0]
    (if-let [p (server-pid dbname)]
      p
      (when (< i timeout-s)
        (Thread/sleep 1000)
        (recur (inc i))))))

(defrecord CubridDB [dbname]
  db/DB
  (setup! [_ test node]
    (when-not (registered-path dbname)
      (let [dir (str (:db-dir test) "/" dbname)]
        (sh! "mkdir" "-p" dir)
        (info "creating" dbname "under" dir
              (sh! "cubrid" "createdb" "-F" dir "-L" dir "--db-volume-size=512M" "--log-volume-size=256M"
                   dbname "en_US.utf8"))))
    (when-not (server-pid dbname)
      (info "starting server" dbname (daemon! "cubrid" "server" "start" dbname)))
    ;; the broker of the active install; "already running" is fine
    (daemon! "cubrid" "broker" "start")
    (or (wait-for-server dbname 60)
        (throw (ex-info (str "server " dbname " did not start; see $JEPSEN_CUBRID_OUT/logs/daemon.log") {}))))

  (teardown! [_ test node]
    ;; a paused server would not stop
    (when-let [p (server-pid dbname)] (sh! "kill" "-CONT" p))
    (if (owned? test)
      (do (sh! "cubrid" "server" "stop" dbname)
          (info "deleting" dbname (sh! "cubrid" "deletedb" dbname))
          ;; deletedb leaves the (now empty) directories; remove only empty ones
          (sh! "find" (str (:db-dir test) "/" dbname) "-depth" "-type" "d" "-empty" "-delete"))
      (when (registered-path dbname)
        (warn dbname "is registered outside" (:db-dir test) "-- used as it is, never deleted"))))

  db/Kill
  (kill! [_ test node]
    (if-let [p (server-pid dbname)]
      (do (sh! "kill" "-9" p) (str "killed " p))
      :not-running))

  (start! [_ test node]
    ;; cub_master may revive the server by itself (auto_restart_server); start it only when it is absent
    (loop [i 0]
      (cond (server-pid dbname) (str "running " (server-pid dbname))
            (< i 5) (do (daemon! "cubrid" "server" "start" dbname)
                        (Thread/sleep 1000)
                        (recur (inc i)))
            :else :start-failed)))

  db/Pause
  (pause! [_ test node]
    (if-let [p (server-pid dbname)] (do (sh! "kill" "-STOP" p) (str "paused " p)) :not-running))
  (resume! [_ test node]
    (if-let [p (server-pid dbname)] (do (sh! "kill" "-CONT" p) (str "resumed " p)) :not-running))

)
;; No db/LogFiles on purpose: when a DB implements both LogFiles and Kill, jepsen.core/snarf-logs! kill -9s it before
;; collecting logs at the end of every run (11-04: two "silent" server deaths). The server logs stay in $CUBRID/log.

(defn cas-kill-nemesis
  "Kills one random CAS process of the test's broker (kill -9): the client connection it served breaks."
  []
  (reify
    nemesis/Reflection
    (fs [_] #{:kill-cas})
    nemesis/Nemesis
    (setup! [this test] this)
    (invoke! [_ test op]
      (let [ps (cas-pids (:broker test))]
        (if (seq ps)
          (let [p (rand-nth ps)] (sh! "kill" "-9" p) (assoc op :value (str "killed cas " p)))
          (assoc op :value :no-cas))))
    (teardown! [_ test])))

;; ---------------------------------------------------------------- JDBC

(Class/forName "cubrid.jdbc.driver.CUBRIDDriver")


(def isolation-levels
  {:read-committed  Connection/TRANSACTION_READ_COMMITTED
   :repeatable-read Connection/TRANSACTION_REPEATABLE_READ
   :serializable    Connection/TRANSACTION_SERIALIZABLE})

(defn open-conn
  ^Connection [test]
  (let [url (str "jdbc:cubrid:localhost:" (:broker-port test) ":" (:dbname test) ":dba::"
                 "?logOnException=false&connectTimeout=10&queryTimeout=" (:query-timeout test))
        c   (DriverManager/getConnection url)]
    (.setAutoCommit c false)
    (.setTransactionIsolation c (isolation-levels (:isolation test)))
    c))

(def abort-codes
  "CUBRID errors after which the transaction is certainly rolled back: deadlock victim (-72), page latch timeout
  (-76), unique violation (-670), lock timeouts (-966..-968), MVCC reevaluation and serialization conflicts
  (-1154, -1155, -1158)."
  #{-72 -76 -670 -966 -967 -968 -1154 -1155 -1158})

(defn quiet-rollback [^Connection c]
  (try (.rollback c) (catch Throwable _)))

(defn exec-txn!
  "Runs (f conn) and commits, within --op-timeout seconds.

  Statement errors roll back -- the transaction cannot have committed, so the op :fail s; an error from commit
  itself is indeterminate (:info) unless CUBRID says it rolled back. A failed connect :fail s (nothing ran).
  Communication errors drop the connection so the next op reconnects.

  A paused server (SIGSTOP) blocks JDBC forever -- CUBRID's queryTimeout is enforced by the server -- so every op
  has a client-side deadline. The thread left behind keeps running, so commit and abandonment race through one
  atom: whoever moves it off :running first wins. Abandoned before commit -> the late thread rolls back instead of
  committing (:fail); commit already started -> :info."
  [client test op f]
  (let [conn-atom (:conn client)
        state     (atom :running)
        body      (fn []
                    (let [c (try (or @conn-atom (reset! conn-atom (open-conn test)))
                                 (catch SQLException e e))]
                      (if (instance? SQLException c)
                        [:connect c nil]
                        (try
                          (let [v (f c)]
                            (if (compare-and-set! state :running :committing)
                              (try (.commit ^Connection c)
                                   [:ok v c]
                                   (catch SQLException e [:commit e c]))
                              (do (quiet-rollback c) [:abandoned nil c])))
                          (catch SQLException e [:stmt e c])))))
        res       (util/timeout (* 1000 (:op-timeout test)) ::timeout (body))]
    (if (= ::timeout res)
      (let [c @conn-atom]
        (reset! conn-atom nil)
        ;; close blocks on the driver's monitor until the stuck call returns; do it off this thread
        (when c (future (try (.close ^Connection c) (catch Throwable _))))
        (if (compare-and-set! state :running :abandoned)
          (assoc op :type :fail :error :timeout-before-commit)
          (assoc op :type :info :error :timeout-during-commit)))
      (let [[phase result c] res
            drop! (fn [] (when c (try (.close ^Connection c) (catch Throwable _))) (reset! conn-atom nil))]
        (case phase
          :ok        (assoc op :type :ok :value result)
          :connect   (do (reset! conn-atom nil)
                         (assoc op :type :fail :error [:connect (.getErrorCode ^SQLException result)
                                                       (.getMessage ^SQLException result)]))
          :abandoned (assoc op :type :fail :error :abandoned)
          :stmt      (let [code (.getErrorCode ^SQLException result)]
                       (quiet-rollback c)
                       (when-not (abort-codes code) (drop!))
                       (assoc op :type :fail :error [code (.getMessage ^SQLException result)]))
          :commit    (let [code (.getErrorCode ^SQLException result)]
                       (drop!)
                       (assoc op :type (if (abort-codes code) :fail :info)
                              :error [:commit code (.getMessage ^SQLException result)])))))))

(defn exec! [^Connection c sql & params]
  (with-open [st (.prepareStatement c sql)]
    (doseq [[i p] (map-indexed vector params)] (.setObject st (int (inc i)) p))
    (.executeUpdate st)))

(defn query [^Connection c sql & params]
  (with-open [st (.prepareStatement c sql)]
    (doseq [[i p] (map-indexed vector params)] (.setObject st (int (inc i)) p))
    (with-open [rs (.executeQuery st)]
      (let [n (.getColumnCount (.getMetaData rs))]
        (loop [rows []]
          (if (.next rs)
            (recur (conj rows (mapv #(.getObject rs (int %)) (range 1 (inc n)))))
            rows))))))

(defn with-setup-conn [test f]
  (let [c (open-conn test)]
    (try (f c) (.commit c) (finally (.close c)))))

;; ---------------------------------------------------------------- append

(def append-tables 4)
(defn append-table [k] (str "jl" (mod k append-tables)))

(defn read-list [c k]
  (when-let [s (ffirst (query c (str "select val from " (append-table k) " where id = ?") (long k)))]
    (mapv #(Long/parseLong %) (str/split s #","))))

(defn append! [c k v]
  (exec! c (str "insert into " (append-table k) " (id, val) values (?, ?)"
                " on duplicate key update val = concat(val, ',', ?)")
         (long k) (str v) (str v)))

(defrecord AppendClient [conn]
  client/Client
  (open! [this test node] (assoc this :conn (atom nil)))
  (setup! [this test]
    (with-setup-conn test
      (fn [c]
        (doseq [i (range append-tables)]
          (try (exec! c (str "drop table jl" i)) (catch SQLException _))
          (exec! c (str "create table jl" i " (id bigint primary key, val varchar(16384))"))))))
  (invoke! [this test op]
    (exec-txn! this test op
               (fn [c]
                 (mapv (fn [[f k v]]
                         (case f
                           :r      [f k (read-list c k)]
                           :append (do (append! c k v) [f k v])))
                       (:value op)))))
  (teardown! [this test])
  (close! [this test] (some-> @conn (.close))))

(def consistency-model
  {:read-committed :read-committed, :repeatable-read :snapshot-isolation, :serializable :serializable})

(defn append-workload [opts]
  (assoc (append/test {:key-count          10
                       :min-txn-length     1
                       :max-txn-length     4
                       :max-writes-per-key 32
                       :consistency-models [(or (:expect opts) (consistency-model (:isolation opts)))]})
         :client (map->AppendClient {})))

;; ---------------------------------------------------------------- wr (rw-register)

(defn wr-table [k] (str "jr" (mod k append-tables)))

(defn read-reg [c k]
  (some-> (ffirst (query c (str "select val from " (wr-table k) " where id = ?") (long k))) long))

(defn write-reg! [c k v]
  ;; the plain UPDATE path; a concurrent first insert of the key makes this insert fail with a unique
  ;; violation (-670), which rolls the transaction back (:fail)
  (when (zero? (exec! c (str "update " (wr-table k) " set val = ? where id = ?") (long v) (long k)))
    (exec! c (str "insert into " (wr-table k) " (id, val) values (?, ?)") (long k) (long v))))

(defrecord WrClient [conn]
  client/Client
  (open! [this test node] (assoc this :conn (atom nil)))
  (setup! [this test]
    (with-setup-conn test
      (fn [c]
        (doseq [i (range append-tables)]
          (try (exec! c (str "drop table jr" i)) (catch SQLException _))
          (exec! c (str "create table jr" i " (id bigint primary key, val bigint)"))))))
  (invoke! [this test op]
    (exec-txn! this test op
               (fn [c]
                 (mapv (fn [[f k v]]
                         (case f
                           :r [f k (read-reg c k)]
                           :w (do (write-reg! c k v) [f k v])))
                       (:value op)))))
  (teardown! [this test])
  (close! [this test] (some-> @conn (.close))))

(defn wr-workload [opts]
  (assoc (wr/test {:key-count          10
                   :min-txn-length     1
                   :max-txn-length     4
                   :max-writes-per-key 32
                   ;; a transaction that reads k and then writes k writes the version after the one it read
                   :wfr-keys?          true
                   :consistency-models [(or (:expect opts) (consistency-model (:isolation opts)))]})
         :client (map->WrClient {})))

;; ---------------------------------------------------------------- bank

(defn bank-balances [c]
  (into (sorted-map) (map (fn [[id b]] [(long id) (long b)])) (query c "select id, balance from jb")))

(defrecord BankClient [conn]
  client/Client
  (open! [this test node] (assoc this :conn (atom nil)))
  (setup! [this test]
    (with-setup-conn test
      (fn [c]
        (try (exec! c "drop table jb") (catch SQLException _))
        (exec! c "create table jb (id int primary key, balance bigint not null)")
        (let [accts (:accounts test) n (count accts) total (:total-amount test)]
          (doseq [[i a] (map-indexed vector accts)]
            (exec! c "insert into jb values (?, ?)" (int a)
                   (long (+ (quot total n) (if (zero? i) (rem total n) 0)))))))))
  (invoke! [this test op]
    (case (:f op)
      :read (exec-txn! this test op bank-balances)
      :transfer
      (let [{:keys [from to amount]} (:value op)]
        (if (= from to)
          (assoc op :type :fail :error :same-account)
          (let [r (exec-txn! this test op
                             (fn [c]
                               (if (= :atomic (:bank-mode test))
                                 ;; one statement per account: no read-modify-write window
                                 (if (pos? (exec! c "update jb set balance = balance - ? where id = ? and balance >= ?"
                                                  (long amount) (int from) (long amount)))
                                   (do (exec! c "update jb set balance = balance + ? where id = ?" (long amount) (int to))
                                       :transferred)
                                   :insufficient)
                                 ;; read, check, write absolute values: lost updates show up unless isolation prevents them
                                 (let [b1 (long (ffirst (query c "select balance from jb where id = ?" (int from))))
                                       b2 (long (ffirst (query c "select balance from jb where id = ?" (int to))))]
                                   (if (< (- b1 amount) 0)
                                     :insufficient
                                     (do (exec! c "update jb set balance = ? where id = ?" (- b1 amount) (int from))
                                         (exec! c "update jb set balance = ? where id = ?" (+ b2 amount) (int to))
                                         :transferred))))))]
            (if (and (= :ok (:type r)) (= :insufficient (:value r)))
              (assoc r :type :fail :value (:value op) :error :insufficient)
              (assoc r :value (:value op))))))))
  (teardown! [this test])
  (close! [this test] (some-> @conn (.close))))

(defn bank-workload [opts]
  (let [b (bank/test {:negative-balances? false})]
    (assoc b
           :client  (map->BankClient {})
           ;; bank/test also plots with gnuplot, which this host lacks
           :checker (bank/checker {:negative-balances? false}))))

;; ---------------------------------------------------------------- set

(defrecord SetClient [conn]
  client/Client
  (open! [this test node] (assoc this :conn (atom nil)))
  (setup! [this test]
    (with-setup-conn test
      (fn [c]
        (try (exec! c "drop table js") (catch SQLException _))
        (exec! c "create table js (v bigint primary key)"))))
  (invoke! [this test op]
    (case (:f op)
      :add  (exec-txn! this test op (fn [c] (exec! c "insert into js values (?)" (long (:value op))) (:value op)))
      :read (exec-txn! this test op (fn [c] (into (sorted-set) (map (comp long first)) (query c "select v from js"))))))
  (teardown! [this test])
  (close! [this test] (some-> @conn (.close))))

(defn set-workload [opts]
  (let [adds (map (fn [x] {:f :add, :value x}) (range))]
    {:client          (map->SetClient {})
     :generator       (gen/mix [adds (repeat {:f :read})])
     :final-generator (gen/each-thread {:f :read})
     :checker         (checker/set-full {:linearizable? true})}))

;; ---------------------------------------------------------------- test

(def workloads {:append append-workload, :wr wr-workload, :bank bank-workload, :set set-workload})

(defn cubrid-test [opts]
  (let [workload ((workloads (:workload opts)) opts)
        db       (CubridDB. (:dbname opts))
        faults   (set (:faults opts))
        ;; only the process faults: nemesis-package would also set up partition/clock/packet nemeses, which run
        ;; commands over SSH and fail under --no-ssh
        pkg      (nc/db-package {:db       db
                                 :nodes    (:nodes opts)
                                 :faults   (disj faults :kill-cas)
                                 :interval (:nemesis-interval opts)
                                 :kill     {:targets [:all]}
                                 :pause    {:targets [:all]}})
        cas?     (faults :kill-cas)
        nem      (nemesis/compose (cond-> [(:nemesis pkg)] cas? (conj (cas-kill-nemesis))))
        nem-gen  (cond-> [] (:generator pkg) (conj (:generator pkg))
                   cas? (conj (gen/stagger (:nemesis-interval opts) (repeat {:type :info :f :kill-cas}))))
        wrap     (:wrap-generator workload identity)]
    (merge tests/noop-test
           opts
           (dissoc workload :client :checker :generator :final-generator :wrap-generator)
           {:name      (str "cubrid-" (name (:workload opts)) "-" (name (:isolation opts))
                            (when (= :bank (:workload opts)) (str "-" (name (:bank-mode opts))))
                            (when (seq faults) (str "-" (str/join "+" (map name (sort faults))))))
            :os        os/noop
            :db        db
            :client    (:client workload)
            :nemesis   nem
            :checker   (checker/compose {:stats      (checker/stats)
                                         :exceptions (checker/unhandled-exceptions)
                                         :timeline   (timeline/html)
                                         :workload   (:checker workload)})
            :generator (wrap
                         (gen/phases
                           (->> (:generator workload)
                                (gen/stagger (/ (:rate opts)))
                                (gen/nemesis (when (seq nem-gen) (gen/mix nem-gen)))
                                (gen/time-limit (:time-limit opts)))
                           (gen/nemesis (:final-generator pkg))
                           (gen/log "faults healed; letting the server recover")
                           (gen/sleep (:recovery-time opts))
                           (when-let [fg (:final-generator workload)] (gen/clients fg))))})))

(defn parse-faults [s]
  (->> (str/split s #",") (remove str/blank?) (map keyword) set))

(def cli-opts
  [[nil "--workload NAME" "append, wr, bank or set" :default :append :parse-fn keyword
    :validate [workloads "unknown workload"]]
   [nil "--isolation LEVEL" "read-committed, repeatable-read or serializable" :default :serializable
    :parse-fn keyword :validate [isolation-levels "unknown isolation level"]]
   [nil "--expect MODEL" "Elle consistency model to check (default follows --isolation)" :parse-fn keyword]
   [nil "--faults LIST" "comma-separated: kill, pause, kill-cas (none if empty)" :default #{} :parse-fn parse-faults]
   [nil "--nemesis-interval SECS" "seconds between fault operations" :default 10 :parse-fn read-string]
   [nil "--rate HZ" "approximate client operations per second" :default 50 :parse-fn read-string]
   [nil "--dbname NAME" "CUBRID database under test (created under --db-dir when absent)" :default "jepsen"]
   [nil "--db-dir DIR" "where the test's database lives; only a database registered under it is ever deleted"
    :default (str (or (System/getenv "JEPSEN_CUBRID_OUT") (str (System/getenv "HOME") "/dev/scratch/jepsen-cubrid")) "/db")]
   [nil "--broker NAME" "broker whose CAS processes kill-cas targets (argv prefix, lower case)" :default "broker1"]
   [nil "--broker-port PORT" "that broker's BROKER_PORT" :default 33000 :parse-fn read-string]
   [nil "--recovery-time SECS" "wait after healing before the final reads" :default 20 :parse-fn read-string]
   [nil "--bank-mode MODE" "rmw (read-modify-write) or atomic" :default :rmw :parse-fn keyword]
   [nil "--query-timeout SECS" "JDBC queryTimeout" :default 30 :parse-fn read-string]
   [nil "--op-timeout SECS" "client-side deadline per operation (a paused server blocks JDBC)" :default 20
    :parse-fn read-string]])

(defn -main [& args]
  (cli/run! (merge (cli/single-test-cmd {:test-fn cubrid-test, :opt-spec cli-opts})
                   (cli/serve-cmd))
            args))
