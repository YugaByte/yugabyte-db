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

/**
 * This is used with LogPrinter to look for error messages reported by external processes.
 */
public interface LogErrorListener {
  void handleLine(String line);

  /**
   * This could throw {@link AssertionError} in case errors were found.
   */
  void reportErrorsAtEnd();

  /**
   * This is called when a {@link LogPrinter} is begin initialized that has been given this instance
   * of a {@link LogErrorListener}. This allows us to detect cases when the same log listener
   * is being associated with multiple log printers, which is not allowed.
   * @param printer log printer instance
   */
  void associateWithLogPrinter(LogPrinter printer);

}
