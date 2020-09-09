/*
 * Copyright 2019 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 *     https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw.common.kms.services;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.yugabyte.yw.common.kms.algorithms.AwsAlgorithm;
import com.yugabyte.yw.common.kms.util.AwsEARServiceUtil;
import com.yugabyte.yw.common.kms.util.EncryptionAtRestUtil;
import com.yugabyte.yw.common.kms.util.KeyProvider;
import com.yugabyte.yw.forms.UniverseTaskParams.EncryptionAtRestConfig;

import java.util.UUID;

/**
 * An implementation of EncryptionAtRestService to communicate with AWS KMS
 * https://aws.amazon.com/kms/
 */
public class AwsEARService extends EncryptionAtRestService<AwsAlgorithm> {

    public AwsEARService() {
        super(KeyProvider.AWS);
    }

    private byte[] generateUniverseDataKey(
            UUID configUUID,
            UUID universeUUID,
            String algorithm,
            int keySize,
            String cmkId
    ) {
        byte[] result = null;
        try {
            final ObjectNode validateResult = validateEncryptionKeyParams(algorithm, keySize);
            if (!validateResult.get("result").asBoolean()) {
                final String errMsg = String.format(
                        "Invalid encryption key parameters detected for create/rotate data key" +
                                " operation in universe %s: %s",
                        universeUUID,
                        validateResult.get("errors").asText()
                );
                LOG.error(errMsg);
                throw new IllegalArgumentException(errMsg);
            }
            result = AwsEARServiceUtil.generateDataKey(configUUID, cmkId, algorithm, keySize);
        } catch (Exception e) {
            LOG.error(String.format(
                    "Error generating universe key for universe %s", universeUUID.toString()
            ));
        }
        return result;
    }

    private String getCMKId(UUID configUUID) {
        final ObjectNode authConfig = getAuthConfig(configUUID);
        final JsonNode cmkNode = authConfig.get("cmk_id");
        return cmkNode == null ? null : cmkNode.asText();
    }

    @Override
    protected AwsAlgorithm[] getSupportedAlgorithms() { return AwsAlgorithm.values(); }

    @Override
    protected ObjectNode createAuthConfigWithService(UUID configUUID, ObjectNode config) {
        // Skip creating a CMK for the KMS Configuration if the user inputted one
        if (config.get("cmk_id") != null) return config;
        final String description = String.format(
                "Yugabyte Master Key for KMS Configuration %s",
                configUUID.toString()
        );
        ObjectNode result = null;
        try {
            final String inputtedCMKPolicy = config.get("cmk_policy") == null ?
                    null : config.get("cmk_policy").asText();
            final String cmkId = AwsEARServiceUtil.createCMK(
                    configUUID,
                    description,
                    inputtedCMKPolicy
            ).getKeyMetadata().getKeyId();
            if (cmkId != null) {
                config.remove("cmk_policy");
                config.put("cmk_id", cmkId);
            }
            result = config;
        } catch (Exception e) {
            final String errMsg = String.format(
                    "Error attempting to create CMK with AWS KMS with config %s",
                    configUUID.toString()
            );
            LOG.error(errMsg, e);
        }
        return result;
    }

    @Override
    protected byte[] createKeyWithService(
            UUID universeUUID,
            UUID configUUID,
            EncryptionAtRestConfig config
    ) {
        byte[] result = null;
        final String cmkId = getCMKId(configUUID);
        if (cmkId != null) {
            // Ensure an alias exists from KMS CMK to universe UUID
            AwsEARServiceUtil.createOrUpdateCMKAlias(configUUID, cmkId, universeUUID.toString());
            switch (config.type) {
                case CMK:
                    result = AwsEARServiceUtil.getCMK(configUUID, cmkId).getKeyArn().getBytes();
                    break;
                default:
                case DATA_KEY:
                    String algorithm = "AES";
                    int keySize = 256;
                    result = generateUniverseDataKey(
                            configUUID,
                            universeUUID,
                            algorithm,
                            keySize,
                            cmkId
                    );
                    break;
            }
        }
        return result;
    }

    @Override
    protected byte[] rotateKeyWithService(
            UUID universeUUID,
            UUID configUUID,
            EncryptionAtRestConfig config
    ) {
        byte[] result = null;
        final String cmkId = getCMKId(configUUID);
        if (cmkId != null) {
            // Ensure an alias exists from KMS CMK to universe UUID
            AwsEARServiceUtil.createOrUpdateCMKAlias(configUUID, cmkId, universeUUID.toString());
            String algorithm = "AES";
            int keySize = 256;
            result = generateUniverseDataKey(
                    configUUID,
                    universeUUID,
                    algorithm,
                    keySize,
                    cmkId
            );
        }
        return result;
    }

    @Override
    public byte[] retrieveKeyWithService(
            UUID universeUUID,
            UUID configUUID,
            byte[] keyRef,
            EncryptionAtRestConfig config
    ) {
        byte[] keyVal = null;
        try {
            switch (config.type) {
                case CMK:
                    keyVal = keyRef;
                    break;
                default:
                case DATA_KEY:
                    keyVal = AwsEARServiceUtil.decryptUniverseKey(configUUID, keyRef);
                    if (keyVal == null) {
                        LOG.warn("Could not retrieve key from key ref through AWS KMS");
                    } else {
                        EncryptionAtRestUtil
                                .setUniverseKeyCacheEntry(universeUUID, keyRef, keyVal);
                    }
                    break;
            }
        } catch (Exception e) {
            final String errMsg = "Error occurred retrieving encryption key";
            LOG.error(errMsg, e);
            throw new RuntimeException(errMsg, e);
        }
        return keyVal;
    }

    @Override
    protected void cleanupWithService(UUID universeUUID, UUID configUUID) {
        final String aliasName = AwsEARServiceUtil.generateAliasName(universeUUID.toString());
        if (AwsEARServiceUtil.getAlias(configUUID, aliasName) != null) {
            AwsEARServiceUtil.deleteAlias(configUUID, aliasName);
        }
    }
}
