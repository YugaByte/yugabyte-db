package com.yugabyte.yw.common.config;

import com.typesafe.config.Config;
import com.typesafe.config.ConfigValueFactory;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.RuntimeConfigEntry;
import com.yugabyte.yw.models.Universe;
import io.ebean.Model;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

/**
 * This class implements most (but not all) methods of com.typesafe.config.Config
 * In addition this also provides for mutating the config using `setValue(path, value)`
 * method.
 * Any mutations will be persisted to database.
 */
public class RuntimeConfig<M extends Model> extends DelegatingConfig {
  private static final Logger LOG = LoggerFactory.getLogger(RuntimeConfig.class);

  private final M scope;

  RuntimeConfig(Config config) {
    this(null, config);
  }

  RuntimeConfig(M scope, Config config) {
    super(config);
    this.scope = scope;
  }

  /**
   * @return modify single path in underlying scoped config in the database and return modified
   * config view.
   */
  public RuntimeConfig<M> setValue(String path, String value) {
    if (scope == null) {
      RuntimeConfigEntry.upsertGlobal(path, value);
    } else if (scope instanceof Customer) {
      RuntimeConfigEntry.upsert((Customer) scope, path, value);
    } else if (scope instanceof Universe) {
      RuntimeConfigEntry.upsert((Universe) scope, path, value);
    } else if (scope instanceof Provider) {
      RuntimeConfigEntry.upsert((Provider) scope, path, value);
    } else {
      throw new UnsupportedOperationException("Unsupported Scope: " + scope);
    }
    super.setValueInternal(path, ConfigValueFactory.fromAnyRef(value));
    LOG.info("After setValue {}", delegate());
    return this;
  }
}
