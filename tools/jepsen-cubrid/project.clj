(defproject jepsen.cubrid "0.1.0-SNAPSHOT"
  :description "Jepsen tests for CUBRID (single node, JDBC through the broker, local-shell nemesis)"
  :main jepsen.cubrid
  :dependencies [[org.clojure/clojure "1.12.0"]
                 [jepsen "0.3.15"]
                 [com.github.seancorfield/next.jdbc "1.3.955"]]
  ;; the CUBRID JDBC driver of the install under test (not on Maven Central)
  :resource-paths [~(str (System/getenv "HOME") "/CUBRID/jdbc/cubrid_jdbc.jar")]
  :jvm-opts ["-Xmx8g" "-Djava.awt.headless=true" "-server"])
