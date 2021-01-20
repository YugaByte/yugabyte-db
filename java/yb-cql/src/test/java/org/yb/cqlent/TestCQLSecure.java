// Copyright (c) YugaByte, Inc.
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
package org.yb.cqlent;

import com.datastax.driver.core.Cluster;
import com.datastax.driver.core.Row;
import org.apache.commons.lang3.RandomStringUtils;
import org.junit.BeforeClass;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.yb.YBTestRunner;
import org.yb.client.TestUtils;
import org.yb.cql.BaseCQLTest;

import java.nio.file.FileSystem;
import java.nio.file.FileSystems;
import java.util.ArrayList;

import static org.yb.AssertionWrappers.assertEquals;

@RunWith(value=YBTestRunner.class)
public class TestCQLSecure extends BaseCQLTest {
  public TestCQLSecure() {
    tserverArgs = new ArrayList<>();
    tserverArgs.add("--use_client_to_server_encryption=true");
    tserverArgs.add(String.format("--certs_for_client_dir=%s", certsDir()));
    useIpWithCertificate = true;
  }

  @BeforeClass
  public static void setUpBeforeClass() {
    LOG.info("TestCQLSecure.setUpBeforeClass is running");

    System.setProperty("javax.net.ssl.trustStore", certsDir() + "/client.truststore");
    System.setProperty("javax.net.ssl.trustStorePassword", "password");
  }

  @Override
  public int getTestMethodTimeoutSec() {
    // No need to adjust for TSAN vs. non-TSAN here, it will be done automatically.
    return 240;
  }

  @Test
  public void testInsert() {
    final int STRING_SIZE = 64;
    String tableName = "test_insert";
    String create_stmt = String.format(
        "CREATE TABLE %s (h int PRIMARY KEY, c varchar);", tableName);
    session.execute(create_stmt);
    String ins_stmt = String.format("INSERT INTO %s (h, c) VALUES (1, ?);", tableName);
    String value = RandomStringUtils.randomAscii(STRING_SIZE);
    session.execute(ins_stmt, value);
    String sel_stmt = String.format("SELECT h, c FROM %s WHERE h = 1 ;", tableName);
    Row row = runSelect(sel_stmt).next();
    assertEquals(1, row.getInt(0));
    assertEquals(value, row.getString(1));
  }

  @Override
  public Cluster.Builder getDefaultClusterBuilder() {
    return super.getDefaultClusterBuilder().withSSL();
  }

  private static String certsDir() {
    FileSystem fs = FileSystems.getDefault();
    return fs.getPath(TestUtils.getBinDir()).resolve(fs.getPath("../ent/test_certs")).toString();
  }
}
