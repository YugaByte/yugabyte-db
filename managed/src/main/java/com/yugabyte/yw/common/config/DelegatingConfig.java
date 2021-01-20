package com.yugabyte.yw.common.config;

import com.typesafe.config.*;

import java.time.Duration;
import java.time.Period;
import java.time.temporal.TemporalAmount;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/**
 * DelegatingConfig that delegates to another config instance for some methods and
 * throws UnsupportedOperationException on other methods.
 * <p>
 * We are having to resort to laborious delegation as all of typesafe config implementation
 * classes are `final` and package private.
 * Ideally we would get away with overriding just couple of core methods in `SimpleConfig`
 * <p>
 * Interface implementations are also discouraged. But if the Config interface changes we
 * will already have work to be done. Also it seems highly unlikely that it will change in
 * major way. Finally, what we are doing here is dumb delegation and will be easy to change for
 * any new changes to the Config interface.
 * <p>
 * We only want to expose ability to use `get*(path)` methods that do not expose underlying
 * ConfigObject or its components.
 * Probably many of these operations can be made to work fine but we do not want to incur
 * testing overhead. Also we do not want to expose methods where config is mutated (with copy)
 * through `with*` methods or expose config components that can be mutated similarly.
 * This is because we will be providing separate mutate method that will also persist the
 * config change so do not want any other mutation-with-copy mechanism's exposed.
 * <p>
 * Unsupported methods are also marked @Deprecated so that IDE finds it easy to tell
 * whats supported and whats not.
 */
class DelegatingConfig implements Config {
  AtomicReference<Config> delegate;

  protected DelegatingConfig(Config delegatesTo) {
    delegate = new AtomicReference<>(delegatesTo);
  }

  Config delegate() {
    return delegate.get();
  }

  protected void setValueInternal(String path, ConfigValue newValue) {
    delegate.set(delegate().withValue(path, newValue));
  }

  @Deprecated
  @Override
  public ConfigObject root() {
    throw new UnsupportedOperationException();
  }

  @Deprecated
  @Override
  public ConfigOrigin origin() {
    throw new UnsupportedOperationException();
  }

  @Deprecated
  @Override
  public Config withFallback(ConfigMergeable other) {
    throw new UnsupportedOperationException();
  }

  @Deprecated
  @Override
  public Config resolve() {
    throw new UnsupportedOperationException();
  }

  @Deprecated
  @Override
  public Config resolve(ConfigResolveOptions options) {
    throw new UnsupportedOperationException();
  }

  @Deprecated
  @Override
  public Config resolveWith(Config source) {
    throw new UnsupportedOperationException();
  }

  @Deprecated
  @Override
  public Config resolveWith(Config source, ConfigResolveOptions options) {
    throw new UnsupportedOperationException();
  }


  @Deprecated
  @Override
  public ConfigObject getObject(String path) {
    throw new UnsupportedOperationException();
  }

  @Deprecated
  @Override
  public Config getConfig(String path) {
    throw new UnsupportedOperationException();
  }

  @Deprecated
  @Override
  public Object getAnyRef(String path) {
    throw new UnsupportedOperationException();
  }

  @Deprecated
  @Override
  public ConfigValue getValue(String path) {
    throw new UnsupportedOperationException();
  }

  @Deprecated
  @Override
  public Config withOnlyPath(String path) {
    throw new UnsupportedOperationException();
  }

  @Deprecated
  @Override
  public Config withoutPath(String path) {
    throw new UnsupportedOperationException();
  }

  @Deprecated
  @Override
  public Config atPath(String path) {
    throw new UnsupportedOperationException();
  }

  @Deprecated
  @Override
  public Config atKey(String key) {
    throw new UnsupportedOperationException();
  }

  @Deprecated
  @Override
  public Config withValue(String path, ConfigValue value) {
    throw new UnsupportedOperationException();
  }

  @Deprecated
  @Override
  public ConfigList getList(String path) {
    throw new UnsupportedOperationException();
  }

  @Deprecated
  @Override
  public Set<Map.Entry<String, ConfigValue>> entrySet() {
    throw new UnsupportedOperationException();
  }

  @Deprecated
  @Override
  public List<? extends ConfigObject> getObjectList(String path) {
    throw new UnsupportedOperationException();
  }

  @Deprecated
  @Override
  public List<? extends Config> getConfigList(String path) {
    throw new UnsupportedOperationException();
  }

  @Deprecated
  @Override
  public List<?> getAnyRefList(String path) {
    throw new UnsupportedOperationException();
  }

  // ============= Delegated methods ====================================
  @Override
  public boolean isResolved() {
    return delegate().isResolved();
  }

  @Override
  public void checkValid(Config reference, String... restrictToPaths) {
    delegate().checkValid(reference, restrictToPaths);
  }

  @Override
  public boolean hasPath(String path) {
    return delegate().hasPath(path);
  }

  @Override
  public boolean hasPathOrNull(String path) {
    return delegate().hasPathOrNull(path);
  }

  @Override
  public boolean isEmpty() {
    return delegate().isEmpty();
  }

  @Override
  public boolean getIsNull(String path) {
    return delegate().getIsNull(path);
  }

  @Override
  public boolean getBoolean(String path) {
    return delegate().getBoolean(path);
  }

  @Override
  public Number getNumber(String path) {
    return delegate().getNumber(path);
  }

  @Override
  public int getInt(String path) {
    return delegate().getInt(path);
  }

  @Override
  public long getLong(String path) {
    return delegate().getLong(path);
  }

  @Override
  public double getDouble(String path) {
    return delegate().getDouble(path);
  }

  @Override
  public String getString(String path) {
    return delegate().getString(path);
  }

  @Override
  public <T extends Enum<T>> T getEnum(Class<T> enumClass, String path) {
    return delegate().getEnum(enumClass, path);
  }

  @Override
  public Long getBytes(String path) {
    return delegate().getBytes(path);
  }

  @Override
  public ConfigMemorySize getMemorySize(String path) {
    return delegate().getMemorySize(path);
  }

  @Override
  public Long getMilliseconds(String path) {
    throw new UnsupportedOperationException("Use getDuration");
  }

  @Override
  public Long getNanoseconds(String path) {
    throw new UnsupportedOperationException("Use getDuration");
  }

  @Override
  public long getDuration(String path, TimeUnit unit) {
    return delegate().getDuration(path, unit);
  }

  @Override
  public Duration getDuration(String path) {
    return delegate().getDuration(path);
  }

  @Override
  public Period getPeriod(String path) {
    return delegate().getPeriod(path);
  }

  @Override
  public TemporalAmount getTemporal(String path) {
    return delegate().getTemporal(path);
  }

  @Override
  public List<Boolean> getBooleanList(String path) {
    return delegate().getBooleanList(path);
  }

  @Override
  public List<Number> getNumberList(String path) {
    return delegate().getNumberList(path);
  }

  @Override
  public List<Integer> getIntList(String path) {
    return delegate().getIntList(path);
  }

  @Override
  public List<Long> getLongList(String path) {
    return delegate().getLongList(path);
  }

  @Override
  public List<Double> getDoubleList(String path) {
    return delegate().getDoubleList(path);
  }

  @Override
  public List<String> getStringList(String path) {
    return delegate().getStringList(path);
  }

  @Override
  public <T extends Enum<T>> List<T> getEnumList(Class<T> enumClass, String path) {
    return delegate().getEnumList(enumClass, path);
  }

  @Override
  public List<Long> getBytesList(String path) {
    return delegate().getBytesList(path);
  }

  @Override
  public List<ConfigMemorySize> getMemorySizeList(String path) {
    return delegate().getMemorySizeList(path);
  }

  @Override
  public List<Long> getMillisecondsList(String path) {
    throw new UnsupportedOperationException("Use getDurationList");
  }

  @Override
  public List<Long> getNanosecondsList(String path) {
    throw new UnsupportedOperationException("Use getDurationList");
  }

  @Override
  public List<Long> getDurationList(String path, TimeUnit unit) {
    return delegate().getDurationList(path, unit);
  }

  @Override
  public List<Duration> getDurationList(String path) {
    return delegate().getDurationList(path);
  }
}
