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
package org.yb.util;

import com.google.common.base.Joiner;
import com.google.common.collect.ImmutableSet;

import java.lang.reflect.Field;
import java.util.Set;

public final class ProcessUtil {

  // Used by pidOfProcess()
  private static String UNIX_PROCESS_CLASS_NAME =  "java.lang.UNIXProcess";
  private static String JDK9_PROCESS_IMPL_CLASS_NAME = "java.lang.ProcessImpl";

  private static Set<String> VALID_SIGNALS =  ImmutableSet.of("STOP", "CONT", "TERM", "KILL");

  private ProcessUtil() {
  }

  /**
   * Gets the pid of a specified process. Relies on reflection and only works on
   * UNIX process, not guaranteed to work on JDKs other than Oracle and OpenJDK.
   *
   * @param proc The specified process.
   * @return The process UNIX pid.
   * @throws IllegalArgumentException If the process is not a UNIXProcess.
   * @throws Exception                If there are other getting the pid via reflection.
   */
  public static int pidOfProcess(Process proc) throws NoSuchFieldException, IllegalAccessException {
    Class<?> procCls = proc.getClass();
    final String actualClassName = procCls.getName();
    if (!actualClassName.equals(UNIX_PROCESS_CLASS_NAME) &&
        !actualClassName.equals(JDK9_PROCESS_IMPL_CLASS_NAME)) {
      throw new IllegalArgumentException("pidOfProcess() expects an object of class " +
          UNIX_PROCESS_CLASS_NAME + " or " + JDK9_PROCESS_IMPL_CLASS_NAME + ", but " +
          procCls.getName() + " was passed in instead!");
    }
    Field pidField = procCls.getDeclaredField("pid");
    pidField.setAccessible(true);
    return (Integer) pidField.get(proc);
  }

  public static String pidStrOfProcess(Process proc) {
    try {
      return String.valueOf(pidOfProcess(proc));
    } catch (NoSuchFieldException | IllegalAccessException ex) {
      return "<error_getting_pid>";
    }
  }


  /**
   * Send a code specified by its string representation to the specified process.
   *
   * @param pid The pid of the process to send a signal to.
   * @param sig  The string representation of the process (e.g., STOP for SIGSTOP).
   * @throws IllegalArgumentException If the signal type is not supported.
   * @throws IllegalStateException    If we are unable to send the specified signal.
   */
  public static void signalProcess(int pid, String sig) throws Exception {
    if (!VALID_SIGNALS.contains(sig)) {
      throw new IllegalArgumentException(sig + " is not a supported signal, only " +
          Joiner.on(",").join(VALID_SIGNALS) + " are supported");
    }
    int rv = Runtime.getRuntime()
        .exec(String.format("kill -%s %d", sig, pid))
        .waitFor();
    if (rv != 0) {
      throw new IllegalStateException(
          String.format("unable to send SIG%s to process with pid=%d: " +
                        "expected return code from kill, but got %d instead", sig, pid, rv));
    }
  }

  public static void signalProcess(Process process, String sig) throws Exception {
    signalProcess(pidOfProcess(process), sig);
  }

  /**
   * Pause the specified process by sending a SIGSTOP using the kill command.
   * @param proc The specified process.
   * @throws Exception If error prevents us from pausing the process.
   */
  public static void pauseProcess(Process proc) throws Exception {
    signalProcess(proc, "STOP");
  }

  /**
   * Resumes the specified process by sending a SIGCONT using the kill command.
   * @param proc The specified process.
   * @throws Exception If error prevents us from resuming the process.
   */
  public static void resumeProcess(Process proc) throws Exception {
    signalProcess(proc, "CONT");
  }

}
