/*
 * Copyright 2019 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 *     https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw.common.kms.util;

import com.amazonaws.services.identitymanagement.AmazonIdentityManagement;
import com.amazonaws.services.identitymanagement.AmazonIdentityManagementClientBuilder;
import com.amazonaws.services.identitymanagement.model.GetRoleRequest;
import com.amazonaws.services.identitymanagement.model.GetRoleResult;
import com.amazonaws.services.identitymanagement.model.Role;
import com.amazonaws.services.identitymanagement.model.User;
import com.amazonaws.services.kms.AWSKMS;
import com.amazonaws.services.kms.AWSKMSClientBuilder;
import com.amazonaws.services.kms.model.AliasListEntry;
import com.amazonaws.services.kms.model.CreateAliasRequest;
import com.amazonaws.services.kms.model.CreateKeyRequest;
import com.amazonaws.services.kms.model.CreateKeyResult;
import com.amazonaws.services.kms.model.DecryptRequest;
import com.amazonaws.services.kms.model.DeleteAliasRequest;
import com.amazonaws.services.kms.model.GenerateDataKeyWithoutPlaintextRequest;
import com.amazonaws.services.kms.model.KeyListEntry;
import com.amazonaws.services.kms.model.ListAliasesRequest;
import com.amazonaws.services.kms.model.ListAliasesResult;
import com.amazonaws.services.kms.model.ListKeysRequest;
import com.amazonaws.services.kms.model.ListKeysResult;
import com.amazonaws.services.kms.model.UpdateAliasRequest;
import com.amazonaws.services.securitytoken.AWSSecurityTokenService;
import com.amazonaws.services.securitytoken.AWSSecurityTokenServiceClientBuilder;
import com.amazonaws.services.securitytoken.AWSSecurityTokenServiceClient;
import com.amazonaws.services.securitytoken.model.GetCallerIdentityRequest;
import com.amazonaws.services.securitytoken.model.GetCallerIdentityResult;
import io.ebean.annotation.EnumValue;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.yugabyte.yw.commissioner.Common.CloudType;
import com.yugabyte.yw.models.KmsConfig;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.Region;
import java.nio.ByteBuffer;
import java.util.Map;
import java.util.Properties;
import java.util.UUID;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import play.Application;
import play.api.Play;

public class AwsEARServiceUtil {
    private static final String CMK_POLICY = "default_cmk_policy.json";

    private static final Logger LOG = LoggerFactory.getLogger(AwsEARServiceUtil.class);

    public enum KeyType {
        @EnumValue("CMK")
        CMK,
        @EnumValue("DATA_KEY")
        DATA_KEY;
    }

    private enum CredentialType {
        @EnumValue("KMS_CONFIG")
        KMS_CONFIG,
        @EnumValue("CLOUD_PROVIDER")
        CLOUD_PROVIDER;
    }

    private static CredentialType getCredentialType(UUID customerUUID, UUID configUUID) {
        CredentialType type = null;
        ObjectNode authConfig = EncryptionAtRestUtil.getAuthConfig(configUUID, KeyProvider.AWS);
        Provider provider = Provider.get(customerUUID, CloudType.aws);
        Map<String, String> providerConfig = provider == null ? null : provider.getConfig();
        if (authConfig == null &&
                provider != null &&
                providerConfig.get("AWS_ACCESS_KEY_ID") != null &&
                providerConfig.get("AWS_SECRET_ACCESS_KEY") != null &&
                ((Region) provider.regions.toArray()[0]).code != null) {
            type = CredentialType.CLOUD_PROVIDER;
        } else if (authConfig != null) {
            type = CredentialType.KMS_CONFIG;
        } else {
            throw new RuntimeException("Could not find AWS credentials for AWS KMS integration");
        }
        return type;
    }

    // Rely on the AWS credential provider chain
    // We set system properties as the first-choice option
    // To debug, a user can set AWS_ACCESS_KEY_ID, AWS_SECRET_KEY, AWS_REGION env vars
    // which will override any existing KMS configuration credentials
    // https://docs.aws.amazon.com/sdk-for-java/v1/developer-guide/credentials.html
    private static void setUserCredentials(UUID configUUID) {
        UUID customerUUID = KmsConfig.get(configUUID).customerUUID;
        Properties p = new Properties(System.getProperties());
        CredentialType credentialType = getCredentialType(customerUUID, configUUID);
        switch (credentialType) {
            case KMS_CONFIG:
                ObjectNode authConfig = EncryptionAtRestUtil.getAuthConfig(
                        configUUID,
                        KeyProvider.AWS
                );
                if (authConfig.get("AWS_ACCESS_KEY_ID") != null &&
                        authConfig.get("AWS_SECRET_ACCESS_KEY") != null &&
                        authConfig.get("AWS_REGION") != null) {
                    p.setProperty("aws.accessKeyId", authConfig.get("AWS_ACCESS_KEY_ID").asText());
                    p.setProperty(
                            "aws.secretKey",
                            authConfig.get("AWS_SECRET_ACCESS_KEY").asText()
                    );
                    p.setProperty("aws.region", authConfig.get("AWS_REGION").asText());
                }
                break;
            case CLOUD_PROVIDER:
                Provider provider = Provider.get(customerUUID, CloudType.aws);
                Map<String, String> config = provider.getConfig();
                String accessKeyId = config.get("AWS_ACCESS_KEY_ID");
                String secretAccessKey = config.get("AWS_SECRET_ACCESS_KEY");
                String region = ((Region) provider.regions.toArray()[0]).code;
                p.setProperty("aws.accessKeyId", accessKeyId);
                p.setProperty("aws.secretKey", secretAccessKey);
                p.setProperty("aws.region", region);
                break;
        }
        System.setProperties(p);
    }

    public static AWSKMS getKMSClient(UUID configUUID) {
        setUserCredentials(configUUID);
        return AWSKMSClientBuilder.defaultClient();
    }

    public static AmazonIdentityManagement getIAMClient(UUID configUUID) {
        setUserCredentials(configUUID);
        return AmazonIdentityManagementClientBuilder.defaultClient();
    }

    public static AWSSecurityTokenService getSTSClient(UUID configUUID) {
        setUserCredentials(configUUID);
        return AWSSecurityTokenServiceClientBuilder.defaultClient();
    }

    public static ObjectNode getPolicyBase() {
        ObjectNode policy = null;
        try {
            ObjectMapper mapper = new ObjectMapper();
            Application application = Play.current().injector().instanceOf(Application.class);
            policy = (ObjectNode) mapper.readTree(application.resourceAsStream(CMK_POLICY));
        } catch (Exception e) {
            String errMsg = "Error occurred retrieving default cmk policy base";
            LOG.error(errMsg, e);
        }
        return policy;
    }

    private static ObjectNode bindArnToStatement(ObjectNode statement, String arn) {
        String principalField = "Principal";
        ObjectNode principal = (ObjectNode) statement.get(principalField);
        principal.put("AWS", arn);
        statement.set(principalField, principal);
        return statement;
    }

    public static ObjectNode bindParamsToPolicyBase(
            ObjectNode policyBase,
            String userArn,
            String rootArn
    ) {
        String statementField = "Statement";
        ArrayNode statements = (ArrayNode) policyBase.get(statementField);
        statements.set(0, bindArnToStatement((ObjectNode) statements.get(0), rootArn));
        statements.set(1, bindArnToStatement((ObjectNode) statements.get(1), userArn));
        return (ObjectNode) policyBase.set(statementField, statements);
    }

    private static GetCallerIdentityResult getCallerIdentity(UUID configUUID) {
        GetCallerIdentityRequest req = new GetCallerIdentityRequest();
        return getSTSClient(configUUID).getCallerIdentity(req);
    }

    private static String getResourceNameFromArn(String arn) {
        return (arn.split(":")[5]).split("/")[1];
    }

    public static String generateDefaultPolicy(UUID configUUID) {
        ObjectNode policyBase = getPolicyBase();
        GetCallerIdentityResult callerIdentity = getCallerIdentity(configUUID);
        if (callerIdentity != null) {
            String credentialsArn = callerIdentity.getArn();
            if (credentialsArn.contains(":assumed-role/")) {
                credentialsArn = getRole(
                        configUUID,
                        getResourceNameFromArn(credentialsArn)
                ).getArn();
            } else if (!credentialsArn.contains(":user/")) {
                throw new RuntimeException(
                        "Credentials provided are not associated to a user or role"
                );
            }
            String rootArn = String.format("arn:aws:iam::%s:root", callerIdentity.getAccount());
            return bindParamsToPolicyBase(policyBase, credentialsArn, rootArn).toString();
        } else {
            LOG.error("Could not get AWS caller identity from provided credentials");
            return null;
        }
    }

    private static void createAlias(UUID configUUID, String kId, String aliasName) {
        final CreateAliasRequest req = new CreateAliasRequest()
                .withAliasName(aliasName)
                .withTargetKeyId(kId);
        AwsEARServiceUtil.getKMSClient(configUUID).createAlias(req);
    }

    private static void updateAlias(UUID configUUID, String kId, String aliasName) {
        final UpdateAliasRequest req = new UpdateAliasRequest()
                .withAliasName(aliasName)
                .withTargetKeyId(kId);
        AwsEARServiceUtil.getKMSClient(configUUID).updateAlias(req);
    }

    public static void deleteAlias(UUID configUUID, String aliasName) {
        final DeleteAliasRequest req = new DeleteAliasRequest()
                .withAliasName(aliasName);
        AwsEARServiceUtil.getKMSClient(configUUID).deleteAlias(req);
    }

    public static AliasListEntry getAlias(UUID configUUID, String aliasName) {
        ListAliasesRequest req = new ListAliasesRequest().withLimit(100);
        AliasListEntry uniAlias = null;
        boolean done = false;
        AWSKMS client = AwsEARServiceUtil.getKMSClient(configUUID);
        while (!done) {
            ListAliasesResult result = client.listAliases(req);
            for (AliasListEntry alias : result.getAliases()) {
                if (alias.getAliasName().equals(aliasName)) {
                    uniAlias = alias;
                    done = true;
                    break;
                }
            }
            req.setMarker(result.getNextMarker());
            if (!result.getTruncated()) done = true;
        }
        return uniAlias;
    }

    private static Role getRole(UUID configUUID, String roleName) {
        GetRoleRequest req = new GetRoleRequest().withRoleName(roleName);
        GetRoleResult result = getIAMClient(configUUID).getRole(req);
        return result.getRole();
    }

    public static KeyListEntry getCMK(UUID configUUID, String cmkId) {
        KeyListEntry cmk = null;
        ListKeysRequest req = new ListKeysRequest().withLimit(1000);
        boolean done = false;
        AWSKMS client = AwsEARServiceUtil.getKMSClient(configUUID);
        while (!done) {
            ListKeysResult result = client.listKeys(req);
            for (KeyListEntry key : result.getKeys()) {
                if (key.getKeyId().equals(cmkId)) {
                    cmk = key;
                    done = true;
                    break;
                }
            }
            req.setMarker(result.getNextMarker());
            if (!result.getTruncated()) done = true;
        }
        return cmk;
    }

    public static byte[] decryptUniverseKey(UUID configUUID, byte[] encryptedUniverseKey) {
        if (encryptedUniverseKey == null) return null;
        ByteBuffer encryptedKeyBuffer = ByteBuffer.wrap(encryptedUniverseKey);
        encryptedKeyBuffer.rewind();
        final DecryptRequest req = new DecryptRequest().withCiphertextBlob(encryptedKeyBuffer);
        ByteBuffer decryptedKeyBuffer = AwsEARServiceUtil
                .getKMSClient(configUUID)
                .decrypt(req)
                .getPlaintext();
        decryptedKeyBuffer.rewind();
        byte[] decryptedUniverseKey = new byte[decryptedKeyBuffer.remaining()];
        decryptedKeyBuffer.get(decryptedUniverseKey);
        return decryptedUniverseKey;
    }

    public static byte[] generateDataKey(
            UUID configUUID,
            String cmkId,
            String algorithm,
            int keySize
    ) {
        final String keySpecBase = "%s_%s";
        final GenerateDataKeyWithoutPlaintextRequest dataKeyRequest =
                new GenerateDataKeyWithoutPlaintextRequest()
                        .withKeyId(cmkId)
                        .withKeySpec(
                                String.format(keySpecBase, algorithm, Integer.toString(keySize))
                        );
        ByteBuffer encryptedKeyBuffer =
                AwsEARServiceUtil.getKMSClient(configUUID)
                        .generateDataKeyWithoutPlaintext(dataKeyRequest)
                        .getCiphertextBlob();
        encryptedKeyBuffer.rewind();
        byte[] encryptedKeyBytes = new byte[encryptedKeyBuffer.remaining()];
        encryptedKeyBuffer.get(encryptedKeyBytes);
        return encryptedKeyBytes;
    }

    public static CreateKeyResult createCMK(UUID configUUID, String description, String policy) {
        CreateKeyRequest req = new CreateKeyRequest();
        if (description != null) req = req.withDescription(description);
        if (policy != null && policy.length() > 0) {
            req = req.withPolicy(policy);
        } else {
            req = req.withPolicy(AwsEARServiceUtil.generateDefaultPolicy(configUUID));
        }
        return AwsEARServiceUtil.getKMSClient(configUUID).createKey(req);
    }

    public static void createOrUpdateCMKAlias(
            UUID configUUID,
            String cmkId,
            String aliasBaseName
    ) {
        final String aliasName = generateAliasName(aliasBaseName);
        AliasListEntry existingAlias = getAlias(configUUID, aliasName);
        if (existingAlias == null) {
            createAlias(configUUID, cmkId, aliasName);
        } else if (!existingAlias.getTargetKeyId().equals(cmkId)) {
            updateAlias(configUUID, cmkId, aliasName);
        }
    }

    public static String generateAliasName(String aliasName) {
        final String aliasNameBase = "alias/%s";
        return String.format(aliasNameBase, aliasName);
    }
}
