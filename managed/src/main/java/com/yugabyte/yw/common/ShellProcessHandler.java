/*
 * Copyright 2019 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 *     https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw.common;

import com.google.inject.Inject;
import com.google.common.base.Joiner;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import javax.inject.Singleton;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.util.List;
import java.util.Map;
import java.util.stream.Collectors;
import java.util.concurrent.TimeUnit;

import com.yugabyte.yw.common.ShellResponse;

@Singleton
public class ShellProcessHandler {
    public static final Logger LOG = LoggerFactory.getLogger(ShellProcessHandler.class);


    @Inject
    play.Configuration appConfig;

    public ShellResponse run(
        List<String> command,
        Map<String, String> extraEnvVars,
        boolean logCmdOutput) {
            return run(command, extraEnvVars, logCmdOutput, null /*description*/);
        }

    public ShellResponse run(
        List<String> command,
        Map<String, String> extraEnvVars,
        boolean logCmdOutput,
        String description) {
        ProcessBuilder pb = new ProcessBuilder(command);
        Map envVars = pb.environment();
        if (extraEnvVars != null && !extraEnvVars.isEmpty()) {
            envVars.putAll(extraEnvVars);
        }
        String devopsHome = appConfig.getString("yb.devops.home");
        if (devopsHome != null) {
            pb.directory(new File(devopsHome));
        }

        ShellResponse response = new ShellResponse();
        response.code = -1;
        if (description == null) {
            response.setDescription(command);
        } else {
            response.description = description;
        }

        File tempOutputFile = null;
        File tempErrorFile = null;
        long startMs = 0;
        try {
            tempOutputFile = File.createTempFile("shell_process_out", "tmp");
            tempErrorFile = File.createTempFile("shell_process_err", "tmp");
            pb.redirectOutput(tempOutputFile);
            pb.redirectError(tempErrorFile);
            startMs = System.currentTimeMillis();
            LOG.info("Starting proc (abbrev cmd) - {}", response.description);
            String fullCommand = "'" + String.join("' '", command) + "'";
            if (appConfig.getBoolean("yb.log.logEnvVars", false) && extraEnvVars != null) {
                fullCommand = Joiner.on(" ").withKeyValueSeparator("=").join(extraEnvVars) +
                                fullCommand;
            }
            LOG.debug("Starting proc (full cmd) - {} - logging stdout={}, stderr={}",
                fullCommand, tempOutputFile.getAbsolutePath(),
                tempErrorFile.getAbsolutePath());
            Process process = pb.start();
            String processOutput = "";
            String processError = "";
            StringBuilder outSb = new StringBuilder();
            StringBuilder errSb = new StringBuilder();
            BufferedReader outputStream = new BufferedReader(
                new InputStreamReader(new FileInputStream(tempOutputFile)));
            BufferedReader errorStream = new BufferedReader(
                new InputStreamReader(new FileInputStream(tempErrorFile)));
            while (!process.waitFor(1, TimeUnit.SECONDS)) {
                appendStream(outputStream, outSb, logCmdOutput);
                appendStream(errorStream, errSb, logCmdOutput);
            }
            appendStream(outputStream, outSb, logCmdOutput);
            appendStream(errorStream, errSb, logCmdOutput);
            outputStream.close();
            errorStream.close();
            processOutput = outSb.toString().trim();
            processError = errSb.toString().trim();
            if (logCmdOutput) {
                LOG.debug("Proc stdout | " + processOutput);
                LOG.debug("Proc stderr | " + processError);
            }

            response.code = process.exitValue();
            response.message = (response.code == 0) ? processOutput : processError;
        } catch (IOException | InterruptedException e) {
            response.code = -1;
            LOG.error("Exception running command", e);
            response.message = e.getMessage();
        } finally {
            if (startMs > 0) {
                response.durationMs = System.currentTimeMillis() - startMs;
            }
            String status = (0 == response.code) ? "success" :
                            ("failure code=" + Integer.toString(response.code));
            LOG.info("Completed proc '{}' status={} [ {} ms ]",
                    response.description, status, response.durationMs);
            if (tempOutputFile != null && tempOutputFile.exists()) {
                tempOutputFile.delete();
            }
            if (tempErrorFile != null && tempErrorFile.exists()) {
                tempErrorFile.delete();
            }
        }

        return response;
    }

    public ShellResponse run(List<String> command, Map<String, String> extraEnvVars) {
        return run(command, extraEnvVars, true /*logCommandOutput*/);
    }

    public ShellResponse run(
        List<String> command,
        Map<String, String> extraEnvVars,
        String description) {
        return run(command, extraEnvVars, true /*logCommandOutput*/, description);
    }

    private static void appendStream(
        BufferedReader br,
        StringBuilder sb,
        boolean logCmdOutput) throws IOException {

        String line = null;
        while ((line = br.readLine()) != null) {
            sb.append(line + System.lineSeparator());
            if (logCmdOutput) {
                if (line.contains("[app]")) {
                    LOG.info(line);
                }
            }
        }
    }
}
