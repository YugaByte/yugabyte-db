// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
package org.yb.client;

import com.google.common.net.HostAndPort;
import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.YBTestRunner;
import org.yb.minicluster.MiniYBCluster;
import org.yb.minicluster.MiniYBDaemon;
import org.yb.util.ServerInfo;

import static org.yb.AssertionWrappers.*;

import java.net.HttpURLConnection;
import java.net.MalformedURLException;
import java.net.URL;
import java.util.Map;
import java.util.Scanner;
import java.io.FileWriter;
import java.util.concurrent.TimeUnit;

@RunWith(value= YBTestRunner.class)
public class TestMasterStatus extends BaseYBClientTest {

    private static final Logger LOG = LoggerFactory.getLogger(TestMasterStatus.class);

    private String isMasterLeader(String host, int port) throws Exception {
        try {
            // call the <web>/api/v1/is-leader JSON endpoint
            String raft_role = "LEADER";
            String urlstr = String.format("http://%s:%d/api/v1/is-leader",
                host, port);

            URL url = new URL(urlstr);
            HttpURLConnection huc = (HttpURLConnection) url.openConnection();
            huc.setRequestMethod("HEAD");
            huc.connect();
            int statusCode = huc.getResponseCode();

            if (statusCode != 200){
                raft_role = "FOLLOWER";
            }

            LOG.info("Master RAFT Role: " + host + ":" + Integer.toString(port) +
                " = " + raft_role);
            return raft_role;
        }
        catch (MalformedURLException e) {
            throw new InternalError(e.getMessage());
        }
    }

    private String allMasterStatuses(String host, int port) throws Exception {
        try {
            // call the <web>/api/v1/masters JSON endpoint
            String urlstr = String.format("http://%s:%d/api/v1/masters",
                host, port);

            URL url = new URL(urlstr);
            Scanner scanner = new Scanner(url.openConnection().getInputStream());
            scanner.useDelimiter("\\A");
            String output = scanner.hasNext() ? scanner.next() : "";
            scanner.close();

            LOG.info("Master Statuses: \n" + host + ":" + Integer.toString(port) +
                " = " + output);
            return output;
        }
        catch (MalformedURLException e) {
            throw new InternalError(e.getMessage());
        }
    }

    @Test(timeout = 60000)
    public void testSingleLeader() throws Exception {
        Map<HostAndPort, MiniYBDaemon> masters = miniCluster.getMasters();
        HostAndPort leaderMasterHostAndPort = syncClient.getLeaderMasterHostAndPort();

        LOG.info("Leader host and port: " + leaderMasterHostAndPort.toString());
        for (HostAndPort host_and_port : masters.keySet()){
            String RAFT_role = isMasterLeader(host_and_port.getHost(),
                masters.get(host_and_port).getWebPort());
            if (RAFT_role == "LEADER") {
                assertEquals(host_and_port.toString(), leaderMasterHostAndPort.toString());
            }

            if (RAFT_role == "FOLLOWER") {
                assertNotEquals(host_and_port.toString(), leaderMasterHostAndPort.toString());
            }
        }
    }

    @Test(timeout = 60000)
    public void testMasterStatuses() throws Exception {
        Map<HostAndPort, MiniYBDaemon> masters = miniCluster.getMasters();
        int num_masters = masters.size();
        for (HostAndPort host_and_port : masters.keySet()){
            String ret_status = allMasterStatuses(host_and_port.getHost(),
                masters.get(host_and_port).getWebPort());
            JsonParser parser = new JsonParser();
            JsonObject masters_obj = parser.parse(ret_status).getAsJsonObject();
            JsonArray masters_arr = masters_obj.getAsJsonArray("masters");
            // Make sure that <web>/api/v1/masters is consistent across all masters.
            // Output should include all the master hosts & ports.
            assertEquals(masters_arr.size(), num_masters);
            for (int i = 0; i < masters_arr.size(); i++) {
                JsonObject master = masters_arr.get(i).getAsJsonObject();
                JsonArray master_reg =
                    master.getAsJsonObject("registration").getAsJsonArray("private_rpc_addresses");
                JsonObject privateRpcAddresses = master_reg.get(0).getAsJsonObject();
                String host = privateRpcAddresses.get("host").getAsString();
                int port = privateRpcAddresses.get("port").getAsInt();
                HostAndPort hnp = HostAndPort.fromParts(host, port);
                assertTrue(masters.containsKey(hnp));
            }
        }
    }
}
