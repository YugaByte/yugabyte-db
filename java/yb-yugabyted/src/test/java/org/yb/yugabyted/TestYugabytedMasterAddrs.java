package org.yb.yugabyted;

import static org.yb.AssertionWrappers.assertEquals;
import static org.yb.AssertionWrappers.assertFalse;
import static org.yb.AssertionWrappers.assertTrue;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.YBTestRunner;
import org.yb.minicluster.MiniYugabytedClusterParameters;
import org.yb.minicluster.MiniYugabytedNodeConfigurations;
import org.yb.minicluster.YugabytedCommands;
import org.yb.minicluster.YugabytedTestUtils;
import org.yb.util.Timeouts;

import com.google.common.net.HostAndPort;

/**
 * Regression test for host-less entries in the saved master address list.
 *
 * yugabyted could persist a host-less entry in current_masters (e.g. ",127.0.0.1:7100"). On the
 * next start that reached yb-tserver as --tserver_master_addrs=,127.0.0.1:7100, and the tserver
 * retried DNS on the empty host inside ResolveMasterAddresses() for master_discovery_timeout_ms
 * (1 hour by default) before logging anything past "Initializing tablet server...". yugabyted
 * kept saving the bad value, so every later start hung the same way.
 *
 * Unit coverage of the parsing itself lives in
 * python/yugabyte/test_yugabyted_master_addrs.py.
 */
@RunWith(value = YBTestRunner.class)
public class TestYugabytedMasterAddrs extends BaseYbdClientTest {

    private static final Logger LOG = LoggerFactory.getLogger(TestYugabytedMasterAddrs.class);

    private static final String CURRENT_MASTERS_KEY = "current_masters";

    /** Max time to wait for the node to come back up after the poisoned restart. */
    private static final long RESTART_TIMEOUT_MS = Timeouts.adjustTimeoutSecForBuildType(180000);
    private static final long RESTART_POLL_MS = 5000;

    /** Max time to wait for `yugabyted status` to report Stopped after `yugabyted stop`. */
    private static final long STOP_TIMEOUT_MS = 60000;
    private static final long STOP_POLL_MS = 5000;

    public TestYugabytedMasterAddrs() {
        clusterParameters = new MiniYugabytedClusterParameters.Builder()
                                .numNodes(1)
                                .build();

        clusterConfigurations = new ArrayList<>();
        for (int i = 0; i < clusterParameters.numNodes; i++) {
            MiniYugabytedNodeConfigurations nodeConfigurations =
                                    new MiniYugabytedNodeConfigurations.Builder()
                .build();

            clusterConfigurations.add(nodeConfigurations);
        }
    }

    /**
     * Writes a host-less entry into the saved config of a stopped node and checks that the next
     * start recovers. Without the fix the tserver blocks on DNS for master_discovery_timeout_ms,
     * so the node never reaches Running and this times out.
     */
    @Test(timeout = 900000)
    public void testRestartWithHostLessMasterAddress() throws Exception {
        String baseDir = clusterConfigurations.get(0).getBaseDir();
        HostAndPort node = miniYugabytedCluster.getYugabytedNodes().keySet().iterator().next();
        int ysqlPort = miniYugabytedCluster.getPostgresContactPoints().get(0).getPort();

        String healthyMasters = readCurrentMasters(baseDir);
        LOG.info("current_masters on the running node: " + healthyMasters);
        assertFalse("current_masters should be set on a running node", healthyMasters.isEmpty());

        // Stop first, so that the config we poison is the one the next start reads back.
        assertTrue("yugabyted stop failed", YugabytedCommands.stop(baseDir));
        waitForNodeToStop(baseDir);

        String poisonedMasters = "," + healthyMasters;
        writeCurrentMasters(baseDir, poisonedMasters);

        assertTrue("yugabyted start failed after poisoning " + CURRENT_MASTERS_KEY,
                YugabytedCommands.start(baseDir));
        YugabytedTestUtils.waitForNodeToStart(baseDir, node.getHost(), ysqlPort,
                RESTART_TIMEOUT_MS, RESTART_POLL_MS);

        String recoveredMasters = readCurrentMasters(baseDir);
        LOG.info("current_masters after the restart: " + recoveredMasters);
        for (String addr : recoveredMasters.split(",", -1)) {
            assertFalse("current_masters still has a host-less entry: " + recoveredMasters,
                    addr.trim().isEmpty());
        }
        // The order of the list is not guaranteed, so compare the addresses as sets.
        assertEquals("current_masters should be back to the pre-poisoning list",
                sortedAddrs(healthyMasters), sortedAddrs(recoveredMasters));

        // The tserver only serves YSQL once it has registered with a master, so a working
        // connection also shows it was not started with a host-less --tserver_master_addrs.
        assertTrue("YSQL is not reachable after the restart",
                YugabytedTestUtils.testYsqlConnection(baseDir, node.getHost()));
    }

    private static void waitForNodeToStop(String baseDir) throws Exception {
        long deadline = System.currentTimeMillis() + STOP_TIMEOUT_MS;
        while (System.currentTimeMillis() < deadline) {
            if (YugabytedTestUtils.checkNodeStatus(baseDir, "Stopped")) {
                return;
            }
            Thread.sleep(STOP_POLL_MS);
        }
        throw new Exception("Node did not stop within " + (STOP_TIMEOUT_MS / 1000) + " seconds");
    }

    private static List<String> sortedAddrs(String mastersCsv) {
        List<String> addrs = new ArrayList<>(Arrays.asList(mastersCsv.split(",")));
        Collections.sort(addrs);
        return addrs;
    }

    private static Path yugabytedConfPath(String baseDir) {
        String expandedBaseDir = baseDir;
        if (expandedBaseDir.startsWith("~")) {
            expandedBaseDir = expandedBaseDir.replaceFirst("~", System.getProperty("user.home"));
        }
        return Paths.get(expandedBaseDir, "conf", "yugabyted.conf");
    }

    private static JSONObject readYugabytedConf(String baseDir) throws IOException {
        Path confPath = yugabytedConfPath(baseDir);
        return new JSONObject(
                new String(Files.readAllBytes(confPath), StandardCharsets.UTF_8));
    }

    private static String readCurrentMasters(String baseDir) throws IOException {
        return readYugabytedConf(baseDir).optString(CURRENT_MASTERS_KEY, "");
    }

    private static void writeCurrentMasters(String baseDir, String mastersCsv) throws IOException {
        Path confPath = yugabytedConfPath(baseDir);
        JSONObject conf = readYugabytedConf(baseDir);
        conf.put(CURRENT_MASTERS_KEY, mastersCsv);
        Files.write(confPath, conf.toString(4).getBytes(StandardCharsets.UTF_8));
        LOG.info("Wrote " + CURRENT_MASTERS_KEY + "=" + mastersCsv + " to " + confPath);
    }
}
