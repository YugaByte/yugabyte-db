// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.common;

import org.apache.commons.io.FileUtils;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.InjectMocks;
import org.mockito.Mock;
import org.mockito.junit.MockitoJUnitRunner;

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;

import static org.hamcrest.CoreMatchers.*;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertThat;
import static org.mockito.Mockito.when;

@RunWith(MockitoJUnitRunner.class)
public class ShellProcessHandlerTest {
    @InjectMocks
    ShellProcessHandler shellProcessHandler;

    @Mock
    play.Configuration appConfig;
    static String TMP_STORAGE_PATH = "/tmp/yugaware_tests";

    @Before
    public void beforeTest() {
        new File(TMP_STORAGE_PATH).mkdirs();
        when(appConfig.getString("yb.devops.home")).thenReturn(TMP_STORAGE_PATH);
    }

    @After
    public void tearDown() throws IOException {
        FileUtils.deleteDirectory(new File(TMP_STORAGE_PATH));
    }

    @Test
    public void testRunWithValidCommandAndDevopsHome() {
        List<String> command = new ArrayList<String>();
        command.add("pwd");
        ShellResponse response = shellProcessHandler.run(command, new HashMap<>());
        assertEquals(0, response.code);
        assertThat(response.message, allOf(notNullValue(), containsString(TMP_STORAGE_PATH)));
        assertEquals(response.message.trim(), response.message);
    }

    @Test
    public void testRunWithInvalidDevopsHome() {
        when(appConfig.getString("yb.devops.home")).thenReturn("/foo");
        List<String> command = new ArrayList<String>();
        command.add("pwd");
        ShellResponse response = shellProcessHandler.run(command, new HashMap<>());
        assertEquals(-1, response.code);
        assertThat(response.message, allOf(notNullValue(),
                equalTo("Cannot run program \"pwd\" (in directory \"/foo\"): " +
                        "error=2, No such file or directory")));
    }

    @Test
    public void testRunWithInvalidCommand() throws IOException {
        String fileName = createTestShellScript();
        List<String> command = new ArrayList<String>();
        command.add(fileName);
        ShellResponse response = shellProcessHandler.run(command, new HashMap<>());
        assertEquals(255, response.code);
        assertThat(response.message.trim(), allOf(notNullValue(), equalTo("error")));
    }

    private String createTestShellScript() throws IOException {
        String fileName = TMP_STORAGE_PATH + "/test.sh";
        FileWriter fw = new FileWriter(fileName);
        fw.write(">&2 echo error; sleep 2; echo foobar; sleep 2; echo more; exit 255");
        fw.close();
        // Set the file as a executable
        File file = new File(fileName);
        file.setExecutable(true);
        return fileName;
    }
}
