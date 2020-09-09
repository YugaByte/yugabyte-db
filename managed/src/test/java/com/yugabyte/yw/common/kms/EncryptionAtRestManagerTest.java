package com.yugabyte.yw.common.kms;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.google.common.collect.ImmutableMap;
import com.yugabyte.yw.common.FakeDBApplication;
import com.yugabyte.yw.common.ModelFactory;
import com.yugabyte.yw.common.kms.EncryptionAtRestManager;
import com.yugabyte.yw.common.kms.services.EncryptionAtRestService;
import com.yugabyte.yw.common.kms.util.KeyProvider;
import com.yugabyte.yw.forms.UniverseTaskParams.EncryptionAtRestConfig;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.Universe;
import java.util.Base64;
import java.util.Map;
import java.util.UUID;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertNotNull;
import org.junit.Before;
import org.junit.Ignore;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.InjectMocks;
import static org.mockito.Matchers.any;
import static org.mockito.Matchers.eq;
import static org.mockito.Mockito.when;
import org.mockito.runners.MockitoJUnitRunner;
import play.libs.Json;

@RunWith(MockitoJUnitRunner.class)
public class EncryptionAtRestManagerTest extends FakeDBApplication {
    @InjectMocks
    EncryptionAtRestManager testManager;

    Customer testCustomer;
    Universe testUniverse;
    String keyProvider = "SMARTKEY";
    EncryptionAtRestConfig keyConfig;

    @Before
    public void setUp() {
        testCustomer = ModelFactory.testCustomer();
        testUniverse = ModelFactory.createUniverse();
        EncryptionAtRestService keyService = testManager.getServiceInstance(keyProvider);
        keyService.createAuthConfig(testCustomer.uuid, "some_config_name", Json.newObject()
                .put("api_key", "some_api_key")
                .put("base_url", "some_base_url")
        );
        keyConfig = new EncryptionAtRestConfig();
    }

    @Test
    public void testGetServiceInstanceKeyProviderDoesNotExist() {
        EncryptionAtRestService keyService = testManager.getServiceInstance("NONSENSE");
        assertNull(keyService);
    }

    @Test
    public void testGetServiceInstance() {
        EncryptionAtRestService keyService = testManager.getServiceInstance("AWS");
        assertNotNull(keyService);
    }

    @Test
    @Ignore public void testGenerateUniverseKey() {
        // TODO: (Daniel) - Fix params
        byte[] universeKeyData = testManager.generateUniverseKey(
                testCustomer.uuid,
                testUniverse.universeUUID,
                keyConfig
        );
        assertNotNull(universeKeyData);
    }
}
