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
package com.yugabyte.jedis;

import org.apache.commons.text.RandomStringGenerator;
import org.junit.After;
import org.junit.Before;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.client.GetTableSchemaResponse;
import org.yb.client.YBClient;
import org.yb.Common.PartitionSchemaPB.HashSchema;
import org.yb.minicluster.BaseMiniClusterTest;
import org.yb.util.SanitizerUtil;
import redis.clients.jedis.HostAndPort;
import redis.clients.jedis.Jedis;
import redis.clients.jedis.JedisCluster;
import redis.clients.jedis.JedisCommands;
import redis.clients.jedis.JedisPoolConfig;
import redis.clients.jedis.YBJedis;
import redis.clients.jedis.exceptions.JedisConnectionException;
import redis.clients.jedis.exceptions.JedisDataException;
import redis.clients.jedis.exceptions.JedisMovedDataException;
import redis.clients.jedis.exceptions.JedisClusterMaxRedirectionsException;

import java.net.InetSocketAddress;
import java.util.ArrayList;
import java.util.Collection;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Random;
import java.util.Set;
import java.util.concurrent.Callable;
import java.util.concurrent.ThreadLocalRandom;
import java.util.stream.Collectors;

import static junit.framework.TestCase.assertEquals;
import static junit.framework.TestCase.assertTrue;
import static org.yb.AssertionWrappers.fail;

import org.yb.YBTestRunner;

import org.junit.runner.RunWith;

@RunWith(value=YBTestRunner.class)
public abstract class BaseJedisTest extends BaseMiniClusterTest {

  protected enum JedisClientType { JEDIS, JEDISCLUSTER, YBJEDIS };

  protected static final String DEFAULT_DB_NAME = "0";
  protected static final int JEDIS_SOCKET_TIMEOUT_MS = 10000;
  private static final int MIN_BACKOFF_TIME_MS = 1000;
  private static final int MAX_BACKOFF_TIME_MS = 50000;
  private static final int MAX_RETRIES = 20;
  private static final int KEY_LENGTH = 20;
  protected JedisClientType jedisClientType;
  protected JedisCommands jedis_client;
  protected JedisCommands jedis_client_1;
  private static final Logger LOG = LoggerFactory.getLogger(BaseJedisTest.class);

  class TSValuePairs {
    TSValuePairs(int size) {
      pairs = new HashMap<>();
      minTS = Long.MAX_VALUE;
      maxTS = Long.MIN_VALUE;

      long timestamp;
      for (int i = 0; i < size; i++) {
        do {
          timestamp = ThreadLocalRandom.current().nextLong(Long.MAX_VALUE);
        } while (pairs.containsKey(timestamp));

        String v = String.format("v%d", ThreadLocalRandom.current().nextInt());
        pairs.put(timestamp, v);
        timestamps.add(timestamp);

        minTS = Math.min(minTS, timestamp);
        maxTS = Math.max(maxTS, timestamp);
      }
      Collections.sort(timestamps);
    }

    // If the map contains only one element, this method will return the timestamp twice. Otherwise,
    // this method will return two random timestamps contained in the map 'pairs'. The first
    // timestamp will always be less than the second one.
    public List<Long> GetRandomTimestamps() {
      if (pairs.size() < 1) {
        throw new IndexOutOfBoundsException("Empty hash map");
      }
      List<Long> list = new ArrayList<>();
      if (pairs.size() == 1) {
        list.add(timestamps.get(0));
        list.add(timestamps.get(0));
        return list;
      }
      Random rand = new Random();
      int index1, index2;
      index1 = rand.nextInt(timestamps.size());
      do {
        index2 = rand.nextInt(timestamps.size());
      } while (index1 == index2);
      if (index1 < index2) {
        list.add(timestamps.get(index1));
        list.add(timestamps.get(index2));
      } else {
        list.add(timestamps.get(index2));
        list.add(timestamps.get(index1));
      }
      return list;
    }

    public String MinValue() throws Exception {
      if (pairs.size() < 1) {
        throw new IndexOutOfBoundsException("Empty hash map");
      }
      return pairs.get(minTS);
    }

    public String MaxValue() throws Exception {
      if (pairs.size() < 1) {
        throw new IndexOutOfBoundsException("Empty hash map");
      }
      return pairs.get(maxTS);
    }

    public List<String> GetValuesList(boolean reverse_order, long min, long max) {
      if (pairs.size() < 1) {
        throw new IndexOutOfBoundsException("Empty hash map");
      }
      List<String> list = new ArrayList<>();
      for (long ts : timestamps) {
        if (ts < min) {
          continue;
        }
        if (ts > max) {
          break;
        }
        String value = pairs.get(ts);
        if (reverse_order) {
          list.add(0, value);
          list.add(0, Long.toString(ts));
        } else {
          list.add(Long.toString(ts));
          list.add(value);
        }
      }
      return list;
    }

    public List<String> GetReverseValuesList() {
      return GetValuesList(true, minTS, maxTS);
    }

    public List<String> GetValuesList() {
      return GetValuesList(false, minTS, maxTS);
    }

    public List<String> GetReverseValuesList(long min, long max) {
      return GetValuesList(true, min, max);
    }

    public List<String> GetValuesList(long min, long max) {
      return GetValuesList(false, min, max);
    }

    public Map<Long, String> pairs;

    // A sorted list of timestamps.
    List<Long> timestamps = new ArrayList<Long>();

    // Minimum timestamp stored in pairs.
    public long minTS;
    // Maximum timestamp stored in pairs.
    public long maxTS;
    private Random random;
  }

  protected void createRedisTableForDB(String db) throws Exception {
    String tableName = YBClient.REDIS_DEFAULT_TABLE_NAME +
                          (db.equals(DEFAULT_DB_NAME) ? "" : "_" + db);
    // Create the redis table.
    miniCluster.getClient().createRedisTableOnly(tableName);

    // TODO(bogdan): Fake sleep until after #4663 is fixed, as we're seeing issues with test work
    // starting, while initial leaders are still moving.
    Thread.sleep((long)(10000 * SanitizerUtil.getTimeoutMultiplier()));

    GetTableSchemaResponse tableSchema = miniCluster.getClient().getTableSchema(
        YBClient.REDIS_KEYSPACE_NAME, tableName);

    assertEquals(HashSchema.REDIS_HASH_SCHEMA, tableSchema.getPartitionSchema().getHashSchema());
  }

  @Before
  public void setUpJedis() throws Exception {
    // Wait for all tserver heartbeats.
    waitForTServersAtMasterLeader();

    miniCluster.getClient().createRedisNamespace();

    createRedisTableForDB(DEFAULT_DB_NAME);

    jedis_client = getClientForDB(DEFAULT_DB_NAME);
  }

  public BaseJedisTest(JedisClientType jedisClientType) {
    this.jedisClientType = jedisClientType;
  }

  protected JedisCommands getClientForDB(String db) {
    // Setup the Jedis client.
    List<InetSocketAddress> redisContactPoints = miniCluster.getRedisContactPoints();
    assertTrue(redisContactPoints.size() > 0);

    switch (jedisClientType) {
      case JEDIS:
        Jedis jedis_client  = new Jedis(redisContactPoints.get(0).getHostName(),
            redisContactPoints.get(0).getPort(), JEDIS_SOCKET_TIMEOUT_MS);
        jedis_client.select(db);
        return jedis_client;
      case JEDISCLUSTER:
        return new JedisCluster(new HostAndPort(redisContactPoints.get(0).getHostName(),
            redisContactPoints.get(0).getPort()), JEDIS_SOCKET_TIMEOUT_MS, JEDIS_SOCKET_TIMEOUT_MS,
            /* maxAttempts */ 100, /* password */ null, db, new JedisPoolConfig());
      case YBJEDIS:
        Set<HostAndPort> contactPoints = redisContactPoints.stream().map(inet ->
            new HostAndPort(inet.getHostName(), inet.getPort())).collect(Collectors.toSet());
        return new YBJedis(contactPoints, JEDIS_SOCKET_TIMEOUT_MS, db);
    }
    return null;
  }

  protected int doWithRetries(int maxRetries,
                              int backoffTimeMin,
                              int backoffTimeMax,
                              Callable c) throws Exception {
    int numRetries = 1;
    int sleepTime = backoffTimeMin;
    while (numRetries <= maxRetries) {
      try {
        c.call();
        break;
      } catch (JedisMovedDataException e) {
        LOG.debug("JedisMovedDataException exception " + e.toString());
      } catch (JedisConnectionException e) {
        LOG.info("JedisConnectionException exception " + e.toString());
      } catch (JedisClusterMaxRedirectionsException e) {
        LOG.info("JedisClusterMaxRedirectionsException exception " + e.toString());
      } catch (JedisDataException e) {
        LOG.info("JedisDataException exception " + e.toString());
      }
        ++numRetries;
        Thread.sleep(sleepTime);
      sleepTime = Math.min(sleepTime * 2, backoffTimeMax);
    }
    return numRetries;
  }

  protected void setWithRetries(JedisCommands client, String key, String value) throws Exception {
    int retries =
        doWithRetries(MAX_RETRIES, MIN_BACKOFF_TIME_MS, MAX_BACKOFF_TIME_MS,
            new Callable<Void>() {
              public Void call() throws Exception {
                assertEquals("OK", client.set(key, "v"));
                return null;
              }
            });
    if (retries > MAX_RETRIES) {
      fail("Exceeded maximum number of retries");
    }
    if (retries > 1 && retries <= MAX_RETRIES) {
      LOG.info("Set Operation on key {} took {} retries", key, retries);
    }
  }

  protected void readAndWriteFromDBs(Collection<String> dbs, int numKeys) throws Exception {
    RandomStringGenerator generator = new RandomStringGenerator.Builder()
        .withinRange('a', 'z').build();
    ArrayList<String> keys = new ArrayList<String>(numKeys);
    for (int i = 0; i < numKeys; i++) {
      keys.add(generator.generate(KEY_LENGTH));
    }

    LOG.info("Writing and reading " + numKeys + " keys.");
    for (String db : dbs) {
      JedisCommands client = getClientForDB(db);
      LOG.info("Writing to db " + db);
      for (String key : keys) {
        setWithRetries(client, key, "v");
      }
    }
    for (String db : dbs) {
      JedisCommands client = getClientForDB(db);
      LOG.info("Reading from db " + db);
      for (String key : keys) {
        int retries = doWithRetries(MAX_RETRIES, MIN_BACKOFF_TIME_MS,
                                    MAX_BACKOFF_TIME_MS, new Callable<Void>() {
                                      public Void call() throws Exception {
                                        assertEquals("v", client.get(key));
                                        return null;
                                      }
                                    });
        if (retries > MAX_RETRIES) {
          fail("Exceeded maximum number of retries");
        }
        if (retries > 1 && retries <= MAX_RETRIES) {
          LOG.info("Get Operation on key {} took {} retries", key, retries);
        }
      }
    }
    LOG.info("Done writing and reading " + numKeys + " key/values to " +
             dbs.size() + " databases.");
  }

  @After
  public void tearDownAfter() throws Exception {
    destroyMiniCluster();

    if (jedis_client != null) {
      switch (jedisClientType) {
        case JEDIS:
          ((Jedis) jedis_client).close();
          break;
        case YBJEDIS:
          ((YBJedis) jedis_client).close();
          break;
      }
    }
  }
}
