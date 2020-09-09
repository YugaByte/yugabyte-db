// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.common;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.google.common.collect.ImmutableMap;
import com.google.inject.Inject;
import com.yugabyte.yw.commissioner.Common;
import com.yugabyte.yw.models.AccessKey;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.Region;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import javax.inject.Singleton;
import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.attribute.PosixFilePermission;
import java.nio.file.attribute.PosixFilePermissions;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.UUID;

@Singleton
public class AccessManager extends DevopsBase {
  public static final Logger LOG = LoggerFactory.getLogger(AccessManager.class);

  @Inject
  play.Configuration appConfig;

  private static final String YB_CLOUD_COMMAND_TYPE = "access";
  private static final String PEM_PERMISSIONS = "r--------";
  private static final String PUB_PERMISSIONS = "rw-r--r--";

  @Override
  protected String getCommandType() {
    return YB_CLOUD_COMMAND_TYPE;
  }

  public enum KeyType {
    PUBLIC,
    PRIVATE;

    public String getExtension() {
      switch(this) {
        case PUBLIC:
          return ".pub";
        case PRIVATE:
          return ".pem";
        default:
          return null;
      }
    }
  }

  private String getOrCreateKeyFilePath(UUID providerUUID) {
    File keyBasePathName = new File(appConfig.getString("yb.storage.path"), "/keys");
    // Protect against multi-threaded access and validate that we only error out if mkdirs fails
    // correctly, by NOT creating the final dir path.
    synchronized(this) {
      if (!keyBasePathName.exists() && !keyBasePathName.mkdirs() && !keyBasePathName.exists()) {
        throw new RuntimeException("Key path " +
            keyBasePathName.getAbsolutePath() + " doesn't exist.");
      }
    }

    File keyFilePath = new File(keyBasePathName.getAbsoluteFile(), providerUUID.toString());
    if (keyFilePath.isDirectory() || keyFilePath.mkdirs()) {
      return keyFilePath.getAbsolutePath();
    }

    throw new RuntimeException("Unable to create key file path " + keyFilePath.getAbsolutePath());
  }

  private String getOrCreateKeyFilePath(String path) {
    File keyBasePathName = new File(appConfig.getString("yb.storage.path"), "/keys");
    // Protect against multi-threaded access and validate that we only error out if mkdirs fails
    // correctly, by NOT creating the final dir path.
    synchronized(this) {
      if (!keyBasePathName.exists() && !keyBasePathName.mkdirs() && !keyBasePathName.exists()) {
        throw new RuntimeException("Key path " +
            keyBasePathName.getAbsolutePath() + " doesn't exist.");
      }
    }

    File keyFilePath = new File(keyBasePathName.getAbsoluteFile(), path);
    if (keyFilePath.isDirectory() || keyFilePath.mkdirs()) {
      return keyFilePath.getAbsolutePath();
    }

    throw new RuntimeException("Unable to create key file path " + keyFilePath.getAbsolutePath());
  }

  // This method would upload the provided key file to the provider key file path.
  public AccessKey uploadKeyFile(UUID regionUUID, File uploadedFile,
                                 String keyCode, KeyType keyType,
                                 String sshUser, Integer sshPort,
                                 boolean airGapInstall) {
    Region region = Region.get(regionUUID);
    String keyFilePath = getOrCreateKeyFilePath(region.provider.uuid);
    AccessKey accessKey = AccessKey.get(region.provider.uuid, keyCode);
    if (accessKey != null) {
      throw new RuntimeException("Duplicate Access KeyCode: " + keyCode);
    }
    Path source = Paths.get(uploadedFile.getAbsolutePath());
    Path destination = Paths.get(keyFilePath, keyCode + keyType.getExtension());
    if (!Files.exists(source)) {
      throw new RuntimeException("Key file " + source.getFileName() + " not found.");
    }
    if (Files.exists(destination) ) {
      throw new RuntimeException("File " + destination.getFileName() + " already exists.");
    }

    try {
      Files.move(source, destination);
      Set<PosixFilePermission> permissions = PosixFilePermissions.fromString(PEM_PERMISSIONS);
      if (keyType == AccessManager.KeyType.PUBLIC) {
        permissions = PosixFilePermissions.fromString(PUB_PERMISSIONS);
      }
      Files.setPosixFilePermissions(destination, permissions);
    } catch (IOException e) {
      LOG.error(e.getMessage(), e);
      throw new RuntimeException("Unable to upload key file " + source.getFileName());
    }

    AccessKey.KeyInfo keyInfo = new AccessKey.KeyInfo();
    if (keyType == AccessManager.KeyType.PUBLIC) {
      keyInfo.publicKey = destination.toAbsolutePath().toString();
    } else {
      keyInfo.privateKey = destination.toAbsolutePath().toString();
    }
    JsonNode vaultResponse = createVault(regionUUID, keyInfo.privateKey);
    if (vaultResponse.has("error")) {
      throw new RuntimeException(vaultResponse.get("error").asText());
    }
    keyInfo.vaultFile = vaultResponse.get("vault_file").asText();
    keyInfo.vaultPasswordFile = vaultResponse.get("vault_password").asText();
    keyInfo.sshUser = sshUser;
    keyInfo.sshPort = sshPort;
    keyInfo.airGapInstall = airGapInstall;
    return AccessKey.create(region.provider.uuid, keyCode, keyInfo);
  }

  // This method would create a public/private key file and upload that to
  // the provider cloud account. And store the credentials file in the keyFilePath
  // and return the file names. It will also create the vault file.
  public AccessKey addKey(UUID regionUUID, String keyCode, Integer sshPort, boolean airGapInstall) {
    return addKey(regionUUID, keyCode, null, null, sshPort, airGapInstall);
  }

  public AccessKey addKey(UUID regionUUID, String keyCode, File privateKeyFile, String sshUser,
      Integer sshPort, boolean airGapInstall) {
    List<String> commandArgs = new ArrayList<String>();
    Region region = Region.get(regionUUID);
    String keyFilePath = getOrCreateKeyFilePath(region.provider.uuid);
    AccessKey accessKey = AccessKey.get(region.provider.uuid, keyCode);

    commandArgs.add("--key_pair_name");
    commandArgs.add(keyCode);
    commandArgs.add("--key_file_path");
    commandArgs.add(keyFilePath);

    String privateKeyFilePath = null;
    if (accessKey != null && accessKey.getKeyInfo().privateKey != null) {
      privateKeyFilePath = accessKey.getKeyInfo().privateKey;
    } else if (privateKeyFile != null) {
      privateKeyFilePath = privateKeyFile.getAbsolutePath();
    }
    // If we have a private key file to use, add in the param.
    if (privateKeyFilePath != null) {
      commandArgs.add("--private_key_file");
      commandArgs.add(privateKeyFilePath);
    }

    JsonNode response = execAndParseCommandRegion(regionUUID, "add-key", commandArgs);
    if (response.has("error")) {
      throw new RuntimeException(response.get("error").asText());
    }

    if (accessKey == null) {
      AccessKey.KeyInfo keyInfo = new AccessKey.KeyInfo();
      keyInfo.publicKey = response.get("public_key").asText();
      keyInfo.privateKey = response.get("private_key").asText();
      JsonNode vaultResponse = createVault(regionUUID, keyInfo.privateKey);
      if (response.has("error")) {
        throw new RuntimeException(response.get("error").asText());
      }
      keyInfo.vaultFile = vaultResponse.get("vault_file").asText();
      keyInfo.vaultPasswordFile = vaultResponse.get("vault_password").asText();
      if (sshUser != null) {
        keyInfo.sshUser = sshUser;
      }
      keyInfo.sshPort = sshPort;
      keyInfo.airGapInstall = airGapInstall;
      accessKey = AccessKey.create(region.provider.uuid, keyCode, keyInfo);
    }
    return accessKey;
  }

  public JsonNode createVault(UUID regionUUID, String privateKeyFile) {
    List<String> commandArgs = new ArrayList<String>();

    if (!new File(privateKeyFile).exists()) {
      throw new RuntimeException("File " + privateKeyFile + " doesn't exists.");
    }
    commandArgs.add("--private_key_file");
    commandArgs.add(privateKeyFile);
    return execAndParseCommandRegion(regionUUID, "create-vault", commandArgs);
  }

  public JsonNode listKeys(UUID regionUUID) {
    return execAndParseCommandRegion(regionUUID, "list-keys", Collections.emptyList());
  }

  public JsonNode deleteKey(UUID regionUUID, String keyCode) {
    List<String> commandArgs = new ArrayList<String>();
    Region region = Region.get(regionUUID);
    if (region == null) {
      throw new RuntimeException("Invalid Region UUID: " + regionUUID);
    }

    switch(Common.CloudType.valueOf(region.provider.code)) {
      case aws:
      case azu:
        String keyFilePath = getOrCreateKeyFilePath(region.provider.uuid);

        commandArgs.add("--key_pair_name");
        commandArgs.add(keyCode);
        commandArgs.add("--key_file_path");
        commandArgs.add(keyFilePath);
        JsonNode response = execAndParseCommandRegion(regionUUID, "delete-key", commandArgs);
        if (response.has("error")) {
          throw new RuntimeException(response.get("error").asText());
        }
        return response;
      default:
        return null;
    }
  }

  public String createCredentialsFile(UUID providerUUID, JsonNode credentials)
      throws IOException {
    ObjectMapper mapper = new ObjectMapper();
    String credentialsFilePath = getOrCreateKeyFilePath(providerUUID) + "/credentials.json";
    mapper.writeValue(new File(credentialsFilePath), credentials);
    return credentialsFilePath;
  }

  public String createKubernetesConfig(String path, Map<String, String> config, boolean edit) throws IOException {
    // Grab the kubernetes config file name and file content and create the physical file.
    String configFileName = config.remove("KUBECONFIG_NAME");
    String configFileContent = config.remove("KUBECONFIG_CONTENT");

    // In case of edit, don't throw exception if conf file isn't provided.
    if (edit && (configFileName == null || configFileContent == null)) {
      return null;
    }

    if (configFileName == null) {
      throw new RuntimeException("Missing KUBECONFIG_NAME data in the provider config.");
    } else if (configFileContent == null) {
      throw new RuntimeException("Missing KUBECONFIG_CONTENT data in the provider config.");
    }
    String configFilePath = getOrCreateKeyFilePath(path);
    Path configFile = Paths.get(configFilePath, configFileName);
    if (!edit && Files.exists(configFile)) {
      throw new RuntimeException("File " + configFile.getFileName() + " already exists.");
    }
    Files.write(configFile, configFileContent.getBytes());

    return configFile.toAbsolutePath().toString();
  }

  public String createPullSecret(UUID providerUUID, Map<String, String> config, boolean edit) throws IOException {
    // Grab the kubernetes config file name and file content and create the physical file.
    String pullSecretFileName = config.remove("KUBECONFIG_PULL_SECRET_NAME");
    String pullSecretFileContent = config.remove("KUBECONFIG_PULL_SECRET_CONTENT");
    if (pullSecretFileName == null) {
      throw new RuntimeException("Missing KUBECONFIG_PULL_SECRET_NAME data in the provider config.");
    } else if (pullSecretFileContent == null) {
      throw new RuntimeException("Missing KUBECONFIG_PULL_SECRET_CONTENT data in the provider config.");
    }
    String pullSecretFilePath = getOrCreateKeyFilePath(providerUUID);
    Path pullSecretFile = Paths.get(pullSecretFilePath, pullSecretFileName);
    if (!edit && Files.exists(pullSecretFile)) {
      throw new RuntimeException("File " + pullSecretFile.getFileName() + " already exists.");
    }
    Files.write(pullSecretFile, pullSecretFileContent.getBytes());

    return pullSecretFile.toAbsolutePath().toString();
  }
}
