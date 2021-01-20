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
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.google.common.collect.ImmutableMap;
import com.yugabyte.yw.common.ApiHelper;
import com.yugabyte.yw.common.FakeDBApplication;
import com.yugabyte.yw.common.kms.EncryptionAtRestManager;
import com.yugabyte.yw.common.kms.util.KeyProvider;
import com.yugabyte.yw.forms.UniverseTaskParams.EncryptionAtRestConfig;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import static org.mockito.Mockito.*;
import static org.mockito.Matchers.anyString;
import static org.mockito.Matchers.anyMap;
import static org.mockito.Matchers.eq;
import org.mockito.runners.MockitoJUnitRunner;
import java.util.Base64;
import java.util.HashMap;
import java.util.Map;
import java.util.UUID;
import play.libs.Json;

@RunWith(MockitoJUnitRunner.class)
public class SmartKeyEARServiceTest extends FakeDBApplication {
    TestEncryptionAtRestService encryptionService;
    EncryptionAtRestManager mockUtil;

    KeyProvider testKeyProvider = KeyProvider.SMARTKEY;
    String testAlgorithm = "AES";
    int testKeySize = 256;
    UUID testUniUUID = UUID.randomUUID();
    UUID testCustomerUUID = UUID.randomUUID();
    EncryptionAtRestConfig config;

    String mockEncodedEncryptionKey =
            "RjZiNzVGekljNFh5Zmh0NC9FQ1dpM0FaZTlMVGFTbW1Wa1dnaHRzdDhRVT0=";
    String mockKid = "9ffd3e51-19e5-41db-ab30-e78910ec743d";
    byte[] mockEncryptionKey = Base64.getDecoder().decode(mockEncodedEncryptionKey);

    String getKeyMockResponse = String.format(
            "{\n" +
                    "    \"acct_id\": \"f1e307cb-1931-45ca-a0cb-216b7001a4a9\",\n" +
                    "    \"activation_date\": \"20190924T232220Z\",\n" +
                    "    \"created_at\": \"20190924T232220Z\",\n" +
                    "    \"creator\": {\n" +
                    "        \"app\": \"49d3c1b9-20ca-48ef-b82a-94877cfb2f3e\"\n" +
                    "    },\n" +
                    "    \"description\": \"Test Description\",\n" +
                    "    \"enabled\": true,\n" +
                    "    \"key_ops\": [\n" +
                    "        \"EXPORT\",\n" +
                    "        \"APPMANAGEABLE\"\n" +
                    "    ],\n" +
                    "    \"key_size\": 256,\n" +
                    "    \"kid\": \"4da0ddf6-7283-4456-a636-26b4e1171390\",\n" +
                    "    \"lastused_at\": \"19700101T000000Z\",\n" +
                    "    \"name\": \"Test Daniel Object 11\",\n" +
                    "    \"never_exportable\": false,\n" +
                    "    \"obj_type\": \"AES\",\n" +
                    "    \"origin\": \"FortanixHSM\",\n" +
                    "    \"public_only\": false,\n" +
                    "    \"state\": \"Active\",\n" +
                    "    \"value\": \"%s\",\n" +
                    "    \"group_id\": \"bd5260f9-7448-49ff-b0af-276f801227cb\"\n" +
                    "}",
            mockEncodedEncryptionKey
    );
    String getKeyListMockResponse = "[]";
    String getAccessTokenMockResponse = "{\n" +
            "    \"token_type\": \"Bearer\",\n" +
            "    \"expires_in\": 18000,\n" +
            "    \"access_token\": \"LVSg7Qcjke2Vw3-VQ3nRpsGRMvQnbHBmsLex-a-Xjcm" +
            "9T4RolbwHHHLWyg0oOmhC2QbH5z4fuwV8EzPC-jmIzA\",\n" +
            "    \"entity_id\": \"49d3c1b9-20ca-48ef-b82a-94877cfb2f3e\"\n" +
            "}";
    String postCreateMockResponse = String.format(
            "{\n" +
                    "    \"acct_id\": \"f1e307cb-1931-45ca-a0cb-216b7001a4a9\",\n" +
                    "    \"activation_date\": \"20190924T232220Z\",\n" +
                    "    \"created_at\": \"20190924T232220Z\",\n" +
                    "    \"creator\": {\n" +
                    "        \"app\": \"49d3c1b9-20ca-48ef-b82a-94877cfb2f3e\"\n" +
                    "    },\n" +
                    "    \"description\": \"Test Description\",\n" +
                    "    \"enabled\": true,\n" +
                    "    \"key_ops\": [\n" +
                    "        \"EXPORT\",\n" +
                    "        \"APPMANAGEABLE\"\n" +
                    "    ],\n" +
                    "    \"key_size\": 256,\n" +
                    "    \"kid\": \"%s\",\n" +
                    "    \"lastused_at\": \"19700101T000000Z\",\n" +
                    "    \"name\": \"Test Daniel Object 11\",\n" +
                    "    \"never_exportable\": false,\n" +
                    "    \"obj_type\": \"AES\",\n" +
                    "    \"origin\": \"FortanixHSM\",\n" +
                    "    \"public_only\": false,\n" +
                    "    \"state\": \"Active\",\n" +
                    "    \"group_id\": \"bd5260f9-7448-49ff-b0af-276f801227cb\"\n" +
                    "}",
            mockKid
    );

    String rekeyMockResponse = String.format(
            "{\n" +
                    "    \"acct_id\": \"f1e307cb-1931-45ca-a0cb-216b7001a4a9\",\n" +
                    "    \"activation_date\": \"20191007T195723Z\",\n" +
                    "    \"created_at\": \"20191007T195723Z\",\n" +
                    "    \"creator\": {\n" +
                    "        \"app\": \"49d3c1b9-20ca-48ef-b82a-94877cfb2f3e\"\n" +
                    "    },\n" +
                    "    \"description\": \"Test Description\",\n" +
                    "    \"enabled\": true,\n" +
                    "    \"key_ops\": [\n" +
                    "        \"EXPORT\",\n" +
                    "        \"APPMANAGEABLE\"\n" +
                    "    ],\n" +
                    "    \"key_size\": 256,\n" +
                    "    \"kid\": \"%s\",\n" +
                    "    \"lastused_at\": \"19700101T000000Z\",\n" +
                    "    \"links\": {\n" +
                    "        \"replaced\": \"b5a60380-4f42-4b96-b28c-e88e5352ab08\"\n" +
                    "    },\n" +
                    "    \"name\": \"Test Daniel Object 11\",\n" +
                    "    \"never_exportable\": false,\n" +
                    "    \"obj_type\": \"AES\",\n" +
                    "    \"origin\": \"FortanixHSM\",\n" +
                    "    \"public_only\": false,\n" +
                    "    \"state\": \"Active\",\n" +
                    "    \"group_id\": \"bd5260f9-7448-49ff-b0af-276f801227cb\"\n" +
                    "}",
            mockKid
    );

    String postCreateMockErroneousResponse = "JSON error: unknown variant `ABC`, expected one " +
            "of `Aes`, `Des`, `Des3`, `Rsa`, `Ec`, `Opaque`, `Hmac`, `Secret`, `Certificate` " +
            "at line 4 column 22";

    ArrayNode keyOps = Json.newArray().add("EXPORT").add("APPMANAGEABLE");
    ObjectNode payload = Json.newObject();

    private class TestEncryptionAtRestService extends SmartKeyEARService {
        boolean createRequest;
        TestEncryptionAtRestService(boolean createRequest) {
            super();
            this.createRequest = createRequest;
        }
        TestEncryptionAtRestService() {
            this(false);
        }
        @Override
        public ObjectNode getAuthConfig(UUID customerUUID) {
            return Json.newObject()
                    .put("api_key", "test key value")
                    .put("base_url", "api.amer.smartkey.io");
        }
    }

    @Before
    public void setUp() {
        config = new EncryptionAtRestConfig();
        payload.put("name", testUniUUID.toString());
        payload.put("obj_type", testAlgorithm);
        payload.put("key_size", testKeySize);
        payload.set("key_ops", keyOps);
        when(mockApiHelper.postRequest(anyString(), eq(Json.newObject()), anyMap()))
                .thenReturn(Json.parse(getAccessTokenMockResponse));
        when(mockApiHelper.postRequest(
                eq("https://api.amer.smartkey.io/crypto/v1/keys"), eq(payload), anyMap())
        ).thenReturn(Json.parse(postCreateMockResponse));
        encryptionService = new TestEncryptionAtRestService();
    }

    @Test
    public void testCreateAndRetrieveEncryptionKeySuccess() {
        when(mockApiHelper.postRequest(
                eq("https://api.amer.smartkey.io/sys/v1/session/auth"),
                eq(Json.newObject()),
                any(Map.class)
        )).thenReturn(Json.newObject().put("access_token", "some_access_token"));
        encryptionService.createAuthConfig(
                testCustomerUUID,
                "some_config_name",
                Json.newObject().put("some_key", "some_val")
        );
        byte[] encryptionKey = encryptionService.createKey(testUniUUID, testCustomerUUID, config);
        assertNotNull(encryptionKey);
        assertEquals(new String(encryptionKey), mockKid);
    }

    @Test
    public void testCreateAndRetrieveEncryptionKeyFailure() {
        ObjectNode testPayload = Json.newObject()
                .put("name", testUniUUID.toString())
                .put("obj_type", testAlgorithm)
                .put("key_size", 256);
        testPayload.set("key_ops", keyOps);
        when(mockApiHelper.postRequest(anyString(), eq(testPayload), anyMap()))
                .thenReturn(Json.newObject().put("error", postCreateMockErroneousResponse));
        UUID configUUID = null;
        assertNull(encryptionService.createKey(testUniUUID, configUUID, config));
    }
}
