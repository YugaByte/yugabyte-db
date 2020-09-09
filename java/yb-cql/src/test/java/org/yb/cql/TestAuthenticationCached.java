package org.yb.cql;

import org.yb.YBTestRunner;
import org.yb.minicluster.BaseMiniClusterTest;

import org.junit.BeforeClass;
import org.junit.runner.RunWith;

@RunWith(value=YBTestRunner.class)
public class TestAuthenticationCached extends TestAuthentication {
  @BeforeClass
  public static void setupBeforeClass() throws Exception {
    BaseMiniClusterTest.tserverArgs.add("--use_cassandra_authentication=true");
    BaseMiniClusterTest.tserverArgs.add("--password_hash_cache_size=4");
    BaseCQLTest.setUpBeforeClass();
  }
}
