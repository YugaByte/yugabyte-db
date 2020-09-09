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
import com.yugabyte.yw.common.kms.algorithms.SupportedAlgorithmInterface;
import com.yugabyte.yw.forms.UniverseTaskParams.EncryptionAtRestConfig;
import com.yugabyte.yw.common.kms.util.EncryptionAtRestUtil;
import com.yugabyte.yw.common.kms.util.EncryptionAtRestUtil.BackupEntry;
import com.yugabyte.yw.common.kms.util.KeyProvider;
import com.yugabyte.yw.models.KmsConfig;
import com.yugabyte.yw.models.KmsHistory;
import com.yugabyte.yw.models.KmsHistoryId;
import com.yugabyte.yw.models.Universe;
import java.util.Arrays;
import java.util.ArrayList;
import java.util.Base64;
import java.util.List;
import java.util.UUID;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import play.libs.Json;

/**
 * An interface to be implemented for each encryption key provider service that YugaByte supports
 */
public abstract class EncryptionAtRestService<T extends SupportedAlgorithmInterface> {

    protected static final Logger LOG = LoggerFactory.getLogger(EncryptionAtRestService.class);

    protected KeyProvider keyProvider;

    protected abstract T[] getSupportedAlgorithms();

    private T validateEncryptionAlgorithm(String algorithm) {
        return Arrays.stream(getSupportedAlgorithms())
                .filter(algo -> algo.name().equals(algorithm))
                .findFirst()
                .orElse(null);
    }

    private boolean validateKeySize(int keySize, T algorithm) {
        return algorithm.getKeySizes()
                .stream()
                .anyMatch(supportedKeySize -> supportedKeySize == keySize);
    }

    protected abstract byte[] createKeyWithService(
            UUID universeUUID,
            UUID configUUID,
            EncryptionAtRestConfig config
    );

    public byte[] createKey(
            UUID universeUUID,
            UUID configUUID,
            EncryptionAtRestConfig config
    ) {
        byte[] result = null;
        try {
            final byte[] existingEncryptionKey = retrieveKey(universeUUID, configUUID, config);
            if (existingEncryptionKey != null && existingEncryptionKey.length > 0) {
                final String errMsg = String.format(
                        "Encryption key for universe %s already exists" +
                                " with provider %s",
                        universeUUID.toString(),
                        this.keyProvider.name()
                );
                LOG.error(errMsg);
                throw new IllegalArgumentException(errMsg);
            }
            final byte[] ref = createKeyWithService(universeUUID, configUUID, config);
            if (ref == null || ref.length == 0) {
                final String errMsg = "createKeyWithService returned empty key ref";
                LOG.error(errMsg);
                throw new RuntimeException(errMsg);
            }
            result = ref;
        } catch (Exception e) {
            LOG.error("Error occurred attempting to create encryption key", e);
        }
        return result;
    }

    protected abstract byte[] rotateKeyWithService(
            UUID universeUUID,
            UUID configUUID,
            EncryptionAtRestConfig config
    );

    public byte[] rotateKey(
            UUID universeUUID,
            UUID configUUID,
            EncryptionAtRestConfig config
    ) {
        byte[] result = null;
        try {
            final byte[] ref = rotateKeyWithService(universeUUID, configUUID, config);
            if (ref == null || ref.length == 0) {
                final String errMsg = "rotateKeyWithService returned empty key ref";
                LOG.error(errMsg);
                throw new RuntimeException(errMsg);
            }
            result = ref;
        } catch (Exception e) {
            LOG.error("Error occurred attempting to rotate encryption key", e);
        }

        return result;
    }

    protected abstract byte[] retrieveKeyWithService(
            UUID universeUUID,
            UUID configUUID,
            byte[] keyRef,
            EncryptionAtRestConfig config
    );

    public byte[] retrieveKey(
            UUID universeUUID,
            UUID configUUID,
            byte[] keyRef,
            EncryptionAtRestConfig config
    ) {
        if (keyRef == null) {
            String errMsg = String.format(
                    "Retrieve key could not find a key ref for universe %s...",
                    universeUUID.toString()
            );
            LOG.warn(errMsg);
            return null;
        }
        // Attempt to retrieve cached entry
        byte[] keyVal = EncryptionAtRestUtil.getUniverseKeyCacheEntry(universeUUID, keyRef);
        // Retrieve through KMS provider if no cache entry exists
        if (keyVal == null) {
            LOG.debug("Universe key cache entry empty. Retrieving key from service");
            keyVal = retrieveKeyWithService(universeUUID, configUUID, keyRef, config);
        }
        return keyVal;
    }

    public byte[] retrieveKey(UUID universeUUID, UUID configUUID, byte[] keyRef) {
        Universe u = Universe.get(universeUUID);
        return retrieveKey(
                universeUUID,
                configUUID,
                keyRef,
                u.getUniverseDetails().encryptionAtRestConfig
        );
    }

    public byte[] retrieveKey(
            UUID universeUUID,
            UUID configUUID,
            EncryptionAtRestConfig config
    ) {
        byte[] key = null;
        KmsHistory activeKey = EncryptionAtRestUtil.getLatestConfigKey(universeUUID, configUUID);
        if (activeKey != null) {
            key = retrieveKey(
                    universeUUID,
                    configUUID,
                    Base64.getDecoder().decode(activeKey.uuid.keyRef),
                    config
            );
        }

        return key;
    }

    protected void cleanupWithService(UUID universeUUID, UUID configUUID) {}

    public void cleanup(UUID universeUUID, UUID configUUID) {
        EncryptionAtRestUtil.removeKeyRotationHistory(universeUUID, configUUID);
        cleanupWithService(universeUUID, configUUID);
    }

    protected EncryptionAtRestService(KeyProvider keyProvider) {
        this.keyProvider = keyProvider;
    }

    protected ObjectNode validateEncryptionKeyParams(String algorithm, int keySize) {
        final T encryptionAlgorithm = validateEncryptionAlgorithm(algorithm);
        ObjectNode result = Json.newObject().put("result", false);
        if (encryptionAlgorithm == null) {
            final String errMsg = String.format(
                    "Requested encryption algorithm \"%s\" is not currently supported",
                    algorithm
            );
            LOG.error(errMsg);
            result.put("errors", errMsg);
        } else if (!validateKeySize(keySize, encryptionAlgorithm)) {
            final String errMsg = String.format(
                    "Requested key size %d bits is not supported by requested encryption " +
                            "algorithm \"%s\"",
                    keySize,
                    algorithm
            );
            LOG.error(errMsg);
            result.put("errors", errMsg);
        } else {
            result.put("result", true);
        }
        return result;
    }

    protected ObjectNode createAuthConfigWithService(UUID configUUID, ObjectNode config) {
        return config;
    }

    public KmsConfig createAuthConfig(UUID customerUUID, String configName, ObjectNode config) {
        ObjectNode maskedConfig = EncryptionAtRestUtil.maskConfigData(
                customerUUID,
                config,
                this.keyProvider
        );
        KmsConfig result = KmsConfig.createKMSConfig(
                customerUUID,
                this.keyProvider,
                maskedConfig,
                configName
        );
        UUID configUUID = result.configUUID;
        ObjectNode existingConfig = getAuthConfig(configUUID);
        ObjectNode updatedConfig = createAuthConfigWithService(configUUID, existingConfig);
        if (updatedConfig != null) {
          ObjectNode updatedMaskedConfig = EncryptionAtRestUtil.maskConfigData(
            customerUUID,
            updatedConfig,
            this.keyProvider
          );
          KmsConfig.updateKMSConfig(configUUID, updatedMaskedConfig);
        } else {
          result.delete();
          result = null;
        }

        return result;
    }

    public ObjectNode getAuthConfig(UUID configUUID) {
        return EncryptionAtRestUtil.getAuthConfig(configUUID, this.keyProvider);
    }

    public List<KmsHistory> getKeyRotationHistory(UUID configUUID, UUID universeUUID) {
        List<KmsHistory> rotationHistory = KmsHistory.getAllConfigTargetKeyRefs(
                configUUID,
                universeUUID,
                KmsHistoryId.TargetType.UNIVERSE_KEY
        );
        if (rotationHistory == null) {
            LOG.warn(String.format(
                    "No rotation history exists for universe %s",
                    universeUUID.toString()
            ));
            rotationHistory = new ArrayList<KmsHistory>();
        }
        return rotationHistory;
    }

    public void deleteKMSConfig(UUID configUUID) {
        if (!EncryptionAtRestUtil.configInUse(configUUID)) {
            final KmsConfig config = getKMSConfig(configUUID);
            if (config != null) config.delete();
        } else throw new IllegalArgumentException(String.format(
                "Cannot delete %s KMS Configuration %s since at least 1 universe" +
                        " exists using encryptionAtRest with this KMS Configuration",
                this.keyProvider.name(),
                configUUID.toString()
        ));
    }

    public KmsConfig getKMSConfig(UUID configUUID) {
        return KmsConfig.get(configUUID);
    }

    public BackupEntry getBackupEntry(KmsHistory history) {
        return new BackupEntry(Base64.getDecoder().decode(history.uuid.keyRef), this.keyProvider);
    }

    // Add backed up keyRefs to the kms_history table for universeUUID and configUUID
    public void restoreBackupEntry(UUID universeUUID, UUID configUUID, byte[] keyRef) {
        if (!EncryptionAtRestUtil.keyRefExists(universeUUID, keyRef)) {
            EncryptionAtRestUtil.addKeyRef(universeUUID, configUUID, keyRef);
        }
    }
}
