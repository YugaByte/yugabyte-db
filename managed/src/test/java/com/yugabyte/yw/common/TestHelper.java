// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.common;

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.text.SimpleDateFormat;
import java.util.Date;

public class TestHelper {
  public static String TMP_PATH = "/tmp/yugaware_tests";

  public static String createTempFile(String basePath, String fileName, String data) {
    FileWriter fw;
    try {
      String filePath = TMP_PATH;
      if (basePath != null) {
        filePath = basePath;
      }
      File tmpFile = new File(filePath, fileName);
      fw = new FileWriter(tmpFile);
      fw.write(data);
      fw.close();
      return tmpFile.getAbsolutePath();
    } catch (IOException ex) { return null; }
  }


  public static String createTempFile(String fileName, String data) {
    return createTempFile(null, fileName, data);
  }

  public static String createTempFile(String data) {
    String fileName = new SimpleDateFormat("yyyyMMddhhmm").format(new Date());
    return createTempFile(fileName, data);
  }
}
