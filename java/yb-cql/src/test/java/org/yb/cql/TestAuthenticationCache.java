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
package org.yb.cql;

import com.datastax.driver.core.*;
import com.datastax.driver.core.ProtocolOptions.Compression;

import com.datastax.driver.core.exceptions.UnauthorizedException;
import org.junit.Test;

import static org.yb.AssertionWrappers.*;

import org.yb.YBTestRunner;

import org.junit.runner.RunWith;

import java.util.Map;

@RunWith(value=YBTestRunner.class)
public class TestAuthenticationCache extends BaseAuthenticationCommonCQLTest {
  @Override
  protected Map<String, String> getTServerFlags() {
    Map<String, String> flags = super.getTServerFlags();
    flags.put("use_cassandra_authentication", "true");
    flags.put("ycql_cache_login_info", "true");
    return flags;
  }
}
