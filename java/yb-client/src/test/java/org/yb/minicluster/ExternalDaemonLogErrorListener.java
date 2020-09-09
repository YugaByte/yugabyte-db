/**
 * Copyright (c) YugaByte, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License.  You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
 * or implied.  See the License for the specific language governing permissions and limitations
 * under the License.
 */
package org.yb.minicluster;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

public class ExternalDaemonLogErrorListener implements LogErrorListener {

  private static final Logger LOG = LoggerFactory.getLogger(ExternalDaemonLogErrorListener.class);

  private final Object serverStartEventMonitor = new Object();
  private boolean sawServerStarting = false;
  private LogPrinter logPrinter;

  // These messages in the log will cause a test failure in the end.
  private static final String[] ERROR_LOG_PATTERNS = {
      "LeakSanitizer: ",
      "AddressSanitizer: ",
      "ThreadSanitizer: ",
      "Segmentation fault: ",
      "UndefinedBehaviorSanitizer: "
  };

  // TODO: consider collecting all matching lines here, up to a certain number.
  private String errorLogLine ;
  private String processDescription;

  public ExternalDaemonLogErrorListener(String processDescription) {
    this.processDescription = processDescription;
  }

  @Override
  public void handleLine(String line) {
    synchronized (serverStartEventMonitor) {
      if (sawServerStarting)
        return;
    }
    if (line.contains("RPC server started.")) {
      synchronized (serverStartEventMonitor) {
        sawServerStarting = true;
        serverStartEventMonitor.notifyAll();
      }
    }
    if (errorLogLine == null) {
      for (String pattern : ERROR_LOG_PATTERNS) {
        if (line.contains(pattern)) {
          errorLogLine = line;
          break;
        }
      }
    }
  }

  @Override
  public void reportErrorsAtEnd() {
    if (errorLogLine != null) {
      LOG.error("Error log line causing the test to fail: ", errorLogLine);
      throw new AssertionError(
          "An error found in the log: " + processDescription + ": " + errorLogLine);
    }
  }

  public void waitForServerStartingLogLine(long deadlineMs) throws InterruptedException {
    long timeoutMs = deadlineMs - System.currentTimeMillis();
    synchronized (serverStartEventMonitor) {
      long timeLeftMs = deadlineMs - System.currentTimeMillis();
      while (timeLeftMs > 0 && !sawServerStarting) {
        serverStartEventMonitor.wait(timeLeftMs);
        timeLeftMs = deadlineMs - System.currentTimeMillis();
      }
      if (!sawServerStarting) {
        throw new RuntimeException(
            "Timed out waiting for a 'server starting' message to appear. " +
            "Waited for " + timeoutMs + ". Log: " + processDescription);
      }
    }

  }

  @Override
  public void associateWithLogPrinter(LogPrinter logPrinter) {
    if (this.logPrinter != null && this.logPrinter != logPrinter) {
      throw new IllegalStateException(
          "Attempt to associate log error listener " + this + " with two different log printers: " +
              this.logPrinter + " and " + logPrinter);
    }
    this.logPrinter = logPrinter;
  }

}
