// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.common;

import com.google.common.collect.ImmutableList;
import com.yugabyte.yw.commissioner.Common;
import com.yugabyte.yw.commissioner.tasks.subtasks.KubernetesCommandExecutor;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.Provider;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.ArgumentCaptor;
import org.mockito.InjectMocks;
import org.mockito.Mock;
import org.mockito.Mockito;
import org.mockito.runners.MockitoJUnitRunner;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;

import static org.junit.Assert.*;
import static org.mockito.Matchers.anyString;
import static org.mockito.Matchers.anyList;
import static org.mockito.Matchers.anyMap;
import static org.mockito.Matchers.any;
import static org.mockito.Matchers.eq;

import static org.mockito.Mockito.when;
import static org.mockito.Mockito.description;
import static org.mockito.Mockito.times;

@RunWith(MockitoJUnitRunner.class)
public class KubernetesManagerTest extends FakeDBApplication {

  @Mock
  ShellProcessHandler shellProcessHandler;

  @Mock
  play.Configuration mockAppConfig;

  @InjectMocks
  KubernetesManager kubernetesManager;

  Provider defaultProvider;
  Customer defaultCustomer;


  ArgumentCaptor<ArrayList> command;
  ArgumentCaptor<HashMap> config;
  ArgumentCaptor<String> description;
  Map<String, String> configProvider = new HashMap<String, String>();

  @Before
  public void setUp() {
    defaultCustomer = ModelFactory.testCustomer();
    defaultProvider = ModelFactory.newProvider(defaultCustomer, Common.CloudType.kubernetes);
    configProvider.put("KUBECONFIG_SERVICE_ACCOUNT", "demo-account");
    configProvider.put("KUBECONFIG", "test");
    defaultProvider.setConfig(configProvider);
    defaultProvider.save();
    command = ArgumentCaptor.forClass(ArrayList.class);
    config = ArgumentCaptor.forClass(HashMap.class);
    description = ArgumentCaptor.forClass(String.class);
  }

  private void runCommand(KubernetesCommandExecutor.CommandType commandType) {
    ShellResponse response = new ShellResponse();
    when(shellProcessHandler.run(anyList(), anyMap(), anyString())).thenReturn(response);

    int numOfCalls = 1;
    switch(commandType) {
      case HELM_INSTALL:
        kubernetesManager.helmInstall(configProvider, defaultProvider.uuid, "demo-universe",
            "/tmp/override.yml");
        break;
      case HELM_UPGRADE:
        kubernetesManager.helmUpgrade(configProvider, "demo-universe", "/tmp/override.yml");
        break;
      case POD_INFO:
        kubernetesManager.getPodInfos(configProvider, "demo-universe");
        break;
      case HELM_DELETE:
        kubernetesManager.helmDelete(configProvider, "demo-universe");
        break;
      case VOLUME_DELETE:
        kubernetesManager.deleteStorage(configProvider, "demo-universe");
        numOfCalls = 2;
        break;
    }

    Mockito.verify(shellProcessHandler, times(numOfCalls)).run(command.capture(),
        (Map<String, String>) config.capture(), description.capture());
  }

  @Test
  public void testHelmUpgrade() {
    when(mockAppConfig.getString("yb.helm.package")).thenReturn("/my/helm.tgz");
    when(mockAppConfig.getLong("yb.helm.timeout_secs")).thenReturn((long)600);
    runCommand(KubernetesCommandExecutor.CommandType.HELM_UPGRADE);
    assertEquals(ImmutableList.of("helm",  "upgrade",  "demo-universe", "/my/helm.tgz", "-f",
        "/tmp/override.yml", "--namespace", "demo-universe", "--timeout", "600s", "--wait"),
        command.getValue());
    assertEquals(config.getValue(), configProvider);
  }

  @Test
  public void testHelmUpgradeNoTimeout() {
    when(mockAppConfig.getString("yb.helm.package")).thenReturn("/my/helm.tgz");
    runCommand(KubernetesCommandExecutor.CommandType.HELM_UPGRADE);
    assertEquals(ImmutableList.of("helm",  "upgrade",  "demo-universe", "/my/helm.tgz", "-f",
        "/tmp/override.yml", "--namespace", "demo-universe", "--timeout", "300s", "--wait"),
        command.getValue());
    assertEquals(config.getValue(), configProvider);
  }

  @Test
  public void testHelmUpgradeFailWithNoConfig() {
    try {
      runCommand(KubernetesCommandExecutor.CommandType.HELM_UPGRADE);
    } catch (RuntimeException e) {
      assertEquals("Helm Package path not provided.", e.getMessage());
    }
  }

  @Test
  public void helmInstallWithRequiredConfig() {
    when(mockAppConfig.getString("yb.helm.package")).thenReturn("/my/helm.tgz");
    when(mockAppConfig.getLong("yb.helm.timeout_secs")).thenReturn((long)600);
    runCommand(KubernetesCommandExecutor.CommandType.HELM_INSTALL);
    assertEquals(ImmutableList.of("helm", "install", "demo-universe", "/my/helm.tgz",
        "--namespace", "demo-universe", "-f",
        "/tmp/override.yml", "--timeout", "600s", "--wait"),
        command.getValue());
    assertEquals(config.getValue(), configProvider);
  }

  @Test
  public void helmInstallWithNoTimeout() {
    when(mockAppConfig.getString("yb.helm.package")).thenReturn("/my/helm.tgz");
    runCommand(KubernetesCommandExecutor.CommandType.HELM_INSTALL);
    assertEquals(ImmutableList.of("helm", "install", "demo-universe", "/my/helm.tgz",
        "--namespace", "demo-universe", "-f",
        "/tmp/override.yml", "--timeout", "300s", "--wait"),
        command.getValue());
    assertEquals(config.getValue(), configProvider);
  }

  @Test
  public void helmInstallWithoutRequiredConfig() {
    try {
      runCommand(KubernetesCommandExecutor.CommandType.HELM_INSTALL);
    } catch (RuntimeException e) {
      assertEquals("Helm Package path not provided.", e.getMessage());
    }
  }

  @Test
  public void getPodInfos() {
    runCommand(KubernetesCommandExecutor.CommandType.POD_INFO);
    assertEquals(ImmutableList.of("kubectl", "get", "pods",
        "--namespace", "demo-universe", "-o", "json",  "-l", "release=demo-universe"),
        command.getValue());
    assertEquals(config.getValue(), configProvider);
  }

  @Test
  public void helmDelete() {
    runCommand(KubernetesCommandExecutor.CommandType.HELM_DELETE);
    assertEquals(ImmutableList.of("helm", "delete", "demo-universe", "-n", "demo-universe"),
        command.getValue());
    assertEquals(config.getValue(), configProvider);
  }

  @Test
  public void deleteStorage() {
    runCommand(KubernetesCommandExecutor.CommandType.VOLUME_DELETE);
    assertEquals(ImmutableList.of(
        ImmutableList.of("kubectl", "delete", "pvc",
            "--namespace", "demo-universe", "-l", "app=yb-master"),
        ImmutableList.of("kubectl", "delete", "pvc",
            "--namespace", "demo-universe", "-l", "app=yb-tserver")),
        command.getAllValues());
    assertEquals(config.getValue(), configProvider);
  }

  @Test
  public void getMasterServiceIPs() {
      kubernetesManager.getServiceIPs(configProvider, "demo-universe", true);
      Mockito.verify(shellProcessHandler, times(1))
          .run(command.capture(), (Map<String, String>) config.capture(), description.capture());
      assertEquals(ImmutableList.of("kubectl", "get", "svc",
          "yb-master-service", "--namespace", "demo-universe", "-o",
          "jsonpath={.spec.clusterIP}|{.status.*.ingress[0].ip}|{.status.*.ingress[0].hostname}"), command.getValue());
  }

  @Test
  public void getTserverServiceIPs() {
    kubernetesManager.getServiceIPs(configProvider, "demo-universe", false);
    Mockito.verify(shellProcessHandler, times(1))
        .run(command.capture(), (Map<String, String>) config.capture(), description.capture());
    assertEquals(ImmutableList.of("kubectl", "get", "svc",
        "yb-tserver-service", "--namespace", "demo-universe", "-o",
        "jsonpath={.spec.clusterIP}|{.status.*.ingress[0].ip}|{.status.*.ingress[0].hostname}"), command.getValue());
  }

  @Test
  public void getServices() {
    kubernetesManager.getServices(configProvider, "demo-universe");
    Mockito.verify(shellProcessHandler, times(1))
        .run(command.capture(), (Map<String, String>) config.capture(), description.capture());
    assertEquals(ImmutableList.of("kubectl", "get", "services",
        "--namespace", "demo-universe", "-o", "json", "-l", "release=" + "demo-universe"),
        command.getValue());
  }
}
