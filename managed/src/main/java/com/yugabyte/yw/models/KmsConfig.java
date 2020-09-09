/*
 * Copyright 2019 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 *     https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw.models;

import io.ebean.*;
import io.ebean.annotation.DbJson;
import io.ebean.annotation.JsonIgnore;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.yugabyte.yw.common.kms.EncryptionAtRestManager;
import com.yugabyte.yw.common.kms.util.KeyProvider;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import play.api.Play;
import play.data.validation.Constraints;
import play.libs.Json;

import javax.persistence.Column;
import javax.persistence.Entity;
import javax.persistence.Id;
import java.util.List;
import java.util.UUID;

@Entity
public class KmsConfig extends Model {
    public static final Logger LOG = LoggerFactory.getLogger(KmsConfig.class);

    public static final int SCHEMA_VERSION = 2;

    @Id
    public UUID configUUID;

    @Column(length=100, nullable = false)
    public String name;

    @Column(nullable = false)
    public UUID customerUUID;

    @Column(length=100, nullable = false)
    public KeyProvider keyProvider;

    @Constraints.Required
    @Column(nullable = false, columnDefinition = "TEXT")
    @DbJson
    @JsonIgnore
    public JsonNode authConfig;

    @Constraints.Required
    @Column(nullable = false)
    public int version;

    public static final Finder<UUID, KmsConfig> find =
      new Finder<UUID, KmsConfig>(KmsConfig.class){};

    public static KmsConfig get(UUID configUUID) {
        if (configUUID == null) return null;
        return KmsConfig.find.query().where()
                .idEq(configUUID)
                .findOne();
    }

    public static KmsConfig createKMSConfig(
            UUID customerUUID,
            KeyProvider keyProvider,
            ObjectNode authConfig,
            String name
    ) {
        KmsConfig kmsConfig = new KmsConfig();
        kmsConfig.keyProvider = keyProvider;
        kmsConfig.customerUUID = customerUUID;
        kmsConfig.authConfig = authConfig;
        kmsConfig.version = SCHEMA_VERSION;
        kmsConfig.name = name;
        kmsConfig.save();
        return kmsConfig;
    }

    public static ObjectNode getKMSAuthObj(UUID configUUID) {
        KmsConfig config = get(configUUID);
        if (config == null) return null;
        return (ObjectNode) config.authConfig.deepCopy();
    }

    public static List<KmsConfig> listKMSConfigs(UUID customerUUID) {
        return KmsConfig.find.query().where()
                .eq("customer_uuid", customerUUID)
                .eq("version", SCHEMA_VERSION)
                .findList();
    }

    public static KmsConfig updateKMSConfig(UUID configUUID, ObjectNode updatedConfig) {
        KmsConfig existingConfig = get(configUUID);
        if (existingConfig == null) return null;
        existingConfig.authConfig = updatedConfig;
        existingConfig.save();
        return existingConfig;
    }
}
