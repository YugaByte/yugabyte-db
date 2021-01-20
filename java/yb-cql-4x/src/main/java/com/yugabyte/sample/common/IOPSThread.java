// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

package com.yugabyte.sample.common;

import org.apache.log4j.Logger;

import com.yugabyte.sample.apps.AppBase;

/**
 * A class that encapsulates a single IO thread. The thread has an index (which is an integer),
 * models an OLTP app and an IO type (read or write). It performs the required IO as long as
 * the app has not completed all its IO.
 */
public class IOPSThread extends Thread {
  private static final Logger LOG = Logger.getLogger(IOPSThread.class);

  // The thread id.
  protected int threadIdx;

  /**
   * The IO types supported by this class.
   */
  public static enum IOType {
    Write,
    Read,
  }
  // The io type this thread performs.
  IOType ioType;

  // The app that is being run.
  protected AppBase app;

  private int numExceptions = 0;

  private volatile boolean ioThreadFailed = false;

  private final boolean printAllExceptions;

  public IOPSThread(int threadIdx, AppBase app, IOType ioType, boolean printAllExceptions) {
    this.threadIdx = threadIdx;
    this.app = app;
    this.ioType = ioType;
    this.printAllExceptions = printAllExceptions;
  }

  public int getNumExceptions() {
    return numExceptions;
  }

  public boolean hasFailed() {
    return ioThreadFailed;
  }

  public long numOps() {
    return app.numOps();
  }

  /**
   * Cleanly shuts down the IOPSThread.
   */
  public void stopThread() {
    this.app.stopApp();
  }

  /**
   * Method that performs the desired type of IO in the IOPS thread.
   */
  @Override
  public void run() {
    try {
      LOG.debug("Starting " + ioType.toString() + " IOPS thread #" + threadIdx);
      int numConsecutiveExceptions = 0;
      while (!app.hasFinished()) {
        try {
          switch (ioType) {
            case Write: app.performWrite(threadIdx); break;
            case Read: app.performRead(); break;
          }
          numConsecutiveExceptions = 0;
        } catch (RuntimeException e) {
          numExceptions++;
          if (numConsecutiveExceptions++ % 10 == 0 || printAllExceptions) {
            app.reportException(e);
          }
          // Reset state only for redis workload. CQL workloads will hit 'InvalidQueryException'
          // with prepared statements if reset and the same statement is re-executed.

          if (numConsecutiveExceptions > 500) {
            LOG.error("Had more than " + numConsecutiveExceptions
                      + " consecutive exceptions. Exiting.", e);
            ioThreadFailed = true;
            return;
          }
          try {
            Thread.sleep(1000);
          } catch (InterruptedException ie) {
            LOG.error("Sleep interrupted.", ie);
            ioThreadFailed = true;
            return;
          }
        }
      }
    } finally {
      LOG.debug("IOPS thread #" + threadIdx + " finished");
      app.terminate();
    }
  }
}
