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

import io.ebean.annotation.EnumValue;
import com.fasterxml.jackson.databind.JsonNode;
import java.io.Serializable;
import java.util.UUID;
import javax.persistence.Column;
import javax.persistence.Embeddable;
import javax.persistence.EnumType;
import javax.persistence.Enumerated;
import play.libs.Json;

@Embeddable
public class KmsHistoryId implements Serializable {
    public enum TargetType {
        @EnumValue("UNIVERSE_KEY")
        UNIVERSE_KEY;
    }

    @Column(name = "key_ref")
    public String keyRef;

    @Column(name = "target_uuid")
    public UUID targetUuid;

    @Enumerated(EnumType.STRING)
    @Column(name = "type")
    public TargetType type;

    public KmsHistoryId(String keyRef, UUID targetUuid, TargetType type) {
        this.keyRef = keyRef;
        this.targetUuid = targetUuid;
        this.type = type;
    }

    @Override
    public String toString() {
        return Json.newObject()
                .put("key_ref", keyRef)
                .put("target_uuid", targetUuid.toString())
                .put("type", type.name())
                .toString();
    }

    @Override
    public boolean equals(Object o) {
        if (!(o instanceof KmsHistoryId)) return false;
        KmsHistoryId oCast = (KmsHistoryId) o;
        return oCast.keyRef.equals(this.keyRef) &&
                oCast.targetUuid.equals(this.targetUuid) &&
                oCast.type.equals(this.type);
    }

    @Override
    public int hashCode() {
        int result = 11;
        result = 31 * result + keyRef.hashCode();
        result = 31 * result + targetUuid.hashCode();
        result = 31 * result + type.hashCode();
        return result;
    }
}
