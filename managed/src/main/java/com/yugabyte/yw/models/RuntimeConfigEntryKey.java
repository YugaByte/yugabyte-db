package com.yugabyte.yw.models;

import javax.persistence.Embeddable;
import javax.persistence.Entity;
import java.io.Serializable;
import java.util.Objects;
import java.util.UUID;

@Entity
@Embeddable
public class RuntimeConfigEntryKey implements Serializable {
  /**
   * scope for the runtime config - for example customerUUID if this is a per customer config.
   */
  private final UUID scopeUUID;

  /**
   * path like `yb.gc.timeout`
   */
  private final String path;

  public RuntimeConfigEntryKey(UUID scopeUUID, String path) {
    this.scopeUUID = scopeUUID;
    this.path = path;
  }

  public UUID getScopeUUID() {
    return scopeUUID;
  }

  public String getPath() {
    return path;
  }

  @Override
  public boolean equals(Object o) {
    if (this == o)
      return true;
    if (o == null || getClass() != o.getClass())
      return false;
    RuntimeConfigEntryKey that = (RuntimeConfigEntryKey) o;
    return scopeUUID.equals(that.scopeUUID) &&
      path.equals(that.path);
  }

  @Override
  public int hashCode() {
    return Objects.hash(scopeUUID, path);
  }

  @Override
  public String toString() {
    return scopeUUID + ":" + path;
  }
}
