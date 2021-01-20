// Copyright (c) Yugabyte, Inc.

package com.yugabyte.yw.models;

import com.yugabyte.yw.common.ExtraMigrationManager;
import com.yugabyte.yw.models.AccessKey;

import java.lang.reflect.Method;
import java.lang.ReflectiveOperationException;
import java.util.List;
import javax.persistence.Column;
import javax.persistence.Entity;
import javax.persistence.Id;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import io.ebean.*;
import io.ebean.annotation.*;

@Entity
public class ExtraMigration extends Model {
  ExtraMigrationManager extraMigrationManager;

  public static final Logger LOG = LoggerFactory.getLogger(ExtraMigration.class);

  @Id
  @Column(nullable = false, unique = true)
  public String migration;

  public static final Finder<String, ExtraMigration> find =
    new Finder<String, ExtraMigration>(ExtraMigration.class) {};

  public static List<ExtraMigration> getAll() {
    return find.query().orderBy("migration asc").findList();
  }

  public void run(ExtraMigrationManager extraMigrationManager) throws ReflectiveOperationException {
    this.extraMigrationManager = extraMigrationManager;
    LOG.info("Running migration '{}'.", this.migration);
    ExtraMigrationManager.class.getMethod(this.migration).invoke(this.extraMigrationManager);
    LOG.info("Completed migration '{}'.", this.migration);
    this.delete();
  }
}
