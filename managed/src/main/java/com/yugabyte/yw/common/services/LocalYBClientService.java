// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.common.services;

import javax.inject.Singleton;

import org.yb.client.YBClient;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

@Singleton
public class LocalYBClientService implements YBClientService {
  public static final Logger LOG = LoggerFactory.getLogger(LocalYBClientService.class);

  @Override
  public synchronized YBClient getClient(String masterHostPorts) {
    return getClient(masterHostPorts, null);
  }

  @Override
  public synchronized YBClient getClient(String masterHostPorts, String certFile) {
    if (masterHostPorts != null) {
      return getNewClient(masterHostPorts, certFile);
    }
    return null;
  }

  @Override
  public synchronized void closeClient(YBClient client, String masterHostPorts) {
    if (client != null) {
      LOG.debug("Closing client masters={}.", masterHostPorts);
      try {
        client.close();
      } catch (Exception e) {
        LOG.warn("Closing client with masters={} hit error {}", masterHostPorts, e.getMessage());
      }
    } else {
      LOG.warn("Client for masters {} was null, cannot close", masterHostPorts);
    }
  }

  private YBClient getNewClient(String masterHPs, String certFile) {
    return new YBClient.YBClientBuilder(masterHPs)
                       .defaultAdminOperationTimeoutMs(120000)
                       .sslCertFile(certFile)
                       .build();
  }
}
