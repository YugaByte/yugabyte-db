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

package com.yugabyte.sample.common;

import java.security.MessageDigest;
import java.util.HashSet;
import java.util.Random;
import java.util.Set;
import java.util.concurrent.ThreadLocalRandom;
import java.util.concurrent.atomic.AtomicLong;

import org.apache.commons.codec.binary.Hex;
import org.apache.log4j.Logger;

public class SimpleLoadGenerator {
  private static final Logger LOG = Logger.getLogger(SimpleLoadGenerator.class);

  public static class Key {
    // The underlying key is an integer.
    Long key;
    // The randomized loadtester prefix.
    String keyPrefix = (CmdLineOpts.loadTesterUUID != null)
                           ? CmdLineOpts.loadTesterUUID.toString()
                           : "key";

    public Key(long key, String keyPrefix) {
      this.key = new Long(key);
      if (keyPrefix != null) {
        this.keyPrefix = keyPrefix;
      }
    }

    public long asNumber() {
      return key;
    }

    public String asString() { return keyPrefix + ":" + key.toString(); }

    public String getKeyWithHashPrefix() throws Exception {
      String k = asString();
      MessageDigest md = MessageDigest.getInstance("MD5");
      md.update(k.getBytes());
      return Hex.encodeHexString(md.digest()) + ":" + k;
    }

    public String getValueStr() {
      return ("val:" + key.toString());
    }

    public String getValueStr(int idx, int size) {
      StringBuilder sb = new StringBuilder();
      sb.append("val");
      sb.append(idx);
      sb.append(":");
      sb.append(key.toString());
      for (int i = sb.length(); i < size; ++i) {
        sb.append("_");
      }
      return sb.toString();
    }

    public void verify(String value) {
      if (value == null || !value.equals(getValueStr())) {
        LOG.fatal("Value mismatch for key: " + key.toString() +
                  ", expected: " + getValueStr() +
                  ", got: " + value);
      }
    }

    @Override
    public String toString() {
      return "Key: " + key + ", value: " + getValueStr();
    }
  }

  // The key to start from.
  final long startKey;
  // The key to write till.
  final long endKey;
  // The max key that was successfully written consecutively.
  AtomicLong maxWrittenKey;
  // The max key that has been generated and handed out so far.
  AtomicLong maxGeneratedKey;
  // Set of keys that failed to write.
  final Set<Long> failedKeys;
  // Keys that have been written above maxWrittenKey.
  final Set<Long> writtenKeys;
  // A background thread to track keys written and increment maxWrittenKey.
  Thread writtenKeysTracker;
  // The prefix for the key.
  String keyPrefix;
  // Random number generator.
  Random random = new Random();

  public SimpleLoadGenerator(long startKey, final long endKey,
                             long maxWrittenKey) {
    this.startKey = startKey;
    this.endKey = endKey;
    this.maxWrittenKey = new AtomicLong(maxWrittenKey);
    this.maxGeneratedKey = new AtomicLong(maxWrittenKey);
    failedKeys = new HashSet<Long>();
    writtenKeys = new HashSet<Long>();
    writtenKeysTracker = new Thread("Written Keys Tracker") {
        @Override
        public void run() {
          do {
            long key = SimpleLoadGenerator.this.maxWrittenKey.get() + 1;
            synchronized (this) {
              if (failedKeys.contains(key) || writtenKeys.remove(key)) {
                SimpleLoadGenerator.this.maxWrittenKey.set(key);
                if (key == endKey - 1) {
                  // We've inserted all requested keys, no need to track
                  // maxWrittenKey/writtenKeys anymore.
                  break;
                }
              } else {
                try {
                  wait();
                } catch (InterruptedException e) {
                  // Ignore
                }
              };
            }
          } while (true);
        }
      };
    // Make tracker a daemon thread so that it will not block the load tester from exiting
    // when done.
    writtenKeysTracker.setDaemon(true);
    writtenKeysTracker.start();
  }

  public void setKeyPrefix(String prefix) {
    keyPrefix = prefix;
  }

  public void recordWriteSuccess(Key key) {
    if (key.asNumber() > maxWrittenKey.get()) {
      synchronized (writtenKeysTracker) {
        writtenKeys.add(key.asNumber());
        writtenKeysTracker.notify();
      }
    }
  }

  public void recordWriteFailure(Key key) {
    synchronized (writtenKeysTracker) {
      if (key != null) {
        failedKeys.add(key.asNumber());
        writtenKeysTracker.notify();
      }
    }
  }

  // Always returns a non-null key.
  public Key getKeyToWrite() {
    Key retKey = null;
    do {
      long maxKey = maxWrittenKey.get();
      // Return a random key to update if we have already written all keys.
      if (maxKey != -1 && maxKey == endKey - 1) {
        retKey = generateKey(ThreadLocalRandom.current().nextLong(maxKey));
      } else {
        retKey = generateKey(maxGeneratedKey.incrementAndGet());
      }

      if (retKey == null) {
        try {
          Thread.sleep(1 /* millisecs */);
        } catch (InterruptedException e) { /* Ignore */ }
      }
    } while (retKey == null);

    return retKey;
  }

  public Key getKeyToRead() {
    long maxKey = maxWrittenKey.get();
    if (maxKey < 0) {
      return null;
    } else if (maxKey == 0) {
      return generateKey(0);
    }
    do {
      long key = ThreadLocalRandom.current().nextLong(maxKey);
      if (!failedKeys.contains(key))
        return generateKey(key);
    } while (true);
  }

  public long getMaxWrittenKey() {
    return maxWrittenKey.get();
  }

  public long getMaxGeneratedKey() {
    return maxGeneratedKey.get();
  }

  public Key generateKey(long key) {
    return new Key(key, keyPrefix);
  }

  public boolean stillLoading() {
    return maxGeneratedKey.get() < endKey - 1;
  }
}
