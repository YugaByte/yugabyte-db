/*
 * Copyright 2020 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 *     https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw.commissioner;

import java.util.ArrayList;
import java.util.List;
import java.util.stream.Collectors;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.core.util.DefaultIndenter;
import com.fasterxml.jackson.core.util.DefaultPrettyPrinter;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.google.common.base.Objects;
import com.google.inject.Singleton;
import com.yugabyte.yw.models.Universe;

@Singleton
public class HealthCheckerReport {

  // @formatter:off
  private static final String STYLE_FONT =
      "font-family: SF Pro Display, SF Pro, Helvetica Neue, Helvetica, sans-serif;";

  /**
   * Decorates the passed report into the HTML view.
   *
   * @param u  Universe
   * @param report  Source json object
   * @param reportOnlyErrors
   * @return  Decorated report (String)
   */
  public String asHtml(Universe u, JsonNode report, boolean reportOnlyErrors) {
    StringBuilder content = new StringBuilder();

    List<String> nodeNames = u.getNodes().stream().map(node -> node.nodeName)
        .sorted().collect(Collectors.toList());

    for (String nodeName : nodeNames) {
      boolean nodeHasError = false;
      boolean isFirstCheck = true;
      StringBuilder nodeContentData = new StringBuilder();

      ArrayNode nodes = (ArrayNode) report.get("data");
      boolean nodeFound = false;

      String nodeNameFromJson = "";
      String nodeIp = "";

      for (JsonNode jsonNode : nodes) {
        nodeNameFromJson = jsonNode.get("node_name").asText();
        if (!Objects.equal(nodeNameFromJson, nodeName)) {
          continue;
        }

        nodeFound = true;
        nodeIp = jsonNode.get("node").asText();
        if (jsonNode.get("has_error").asBoolean()) {
          nodeHasError = true;
        } else if (reportOnlyErrors) {
          continue;
        }

        nodeContentData
            .append(assembleMailRow(jsonNode, isFirstCheck, jsonNode.get("timestamp").asText()));
        isFirstCheck = false;
      }

      if (!nodeFound || (reportOnlyErrors && !nodeHasError)) {
        continue;
      }

      String nodeHeaderFgColor = nodeHasError ? "#ffffff;" : "#289b42;";
      String nodeHeaderBgColor = nodeHasError ? "#E8473F;" : "#ffffff";
      String nodeHeaderStyleColors = String.format("background-color:%s;color:%s",
          nodeHeaderBgColor, nodeHeaderFgColor);
      String badgeCaption = nodeHasError ? "Error" : "Running fine";

      String badgeStyle = String.format(
          "style=\"font-weight:400;margin-left:10px;\n" +
          "font-size:0.6em;vertical-align:middle;\n" +
          "%s\n" +
          "border-radius:4px;padding:2px 6px;\"",
          nodeHeaderStyleColors);

      String nodeHeaderTitle = String.format("%s<br>%s<span %s>%s</span>\n", nodeName, nodeIp,
          badgeStyle, badgeCaption);

      String h2Style = String.format(
          "font-weight:700;line-height:1em;color:#202951;font-size:2.5em;margin:0;\n%s",
          STYLE_FONT);

      String nodeHeader = String.format("%s<h2 style=\"%s\">%s</h2>\n", makeSubtitle("Cluster"),
          h2Style, nodeHeaderTitle);

      String tableContainerStyle = "background-color:#ffffff;padding:15px 20px 7px;\n" +
          "border-radius:10px;margin-top:30px;margin-bottom:50px;";

      content.append(String.format("%s<div style=\"%s\">%s</div>", nodeHeader,
          tableContainerStyle, tableContainer(nodeContentData.toString())));
    }

    if (content.length() == 0) {
      content.append("<b>No errors to report.</b>");
    }

    String style = String
        .format("%s font-size: 14px;background-color:#f7f7f7;padding:25px 30px 5px;", STYLE_FONT);
    // add timestamp to avoid gmail collapsing
    String timestamp = String.format("<span style=\"color:black;font-size:10px;\">%s</span>",
        report.path("timestamp").asText());

    String header = String.format(
        "<table width=\"100%%\">\n" +
        "    <tr>\n" +
        "        <td style=\"text-align:left\">%s</td>\n" +
        "        <td style=\"text-align:right\">%s</td>\n" +
        "    </tr>\n" +
        "    <tr>\n" +
        "        <td style=\"text-align:left\">%s</td>\n" +
        "    </tr>\n" +
        "</table>\n",
        makeHeaderLeft("Universe name", u.name),
        timestamp,
        makeHeaderLeft("Universe version", report.path("yb_version").asText()));

    return String.format("<html><body><pre style=\"%s\">%s\n%s %s</pre></body></html>", style,
        header, content.toString(), timestamp);
  }
  // @formatter:on

  private static String makeSubtitle(String content) {
    return String.format("<span style=\"color:#8D8F9D;font-weight:400;\">%s:</span>\n", content);
  }

  private static String thElement(String style, String content) {
    return String.format("<th style=\"%s\">%s</th>\n", style, content);
  }

  private static String trElement(String content) {
    return String.format("<tr style=\"text-transform:uppercase;font-weight:500;\">%s</tr>\n",
        content);
  }

  private static String tableElement(String content) {
    return String.format(
        "<table cellspacing=\"0\" style=\"width:auto;text-align:left;"
            + "font-size:12px;\">%s</table>\n",
        content);
  }

  // @formatter:off
  private static String tableContainer(String content) {
    return tableElement(
      trElement(
            thElement("width:15%;padding:0 30px 10px 0;", "Check type") +
            thElement("width:15%;white-space:nowrap;padding:0 30px 10px 0;", "Details") +
            thElement("", "")) +
      content);
  }

  private static String makeHeaderLeft(String title, String content) {
    return String.format(
      "%s<h1 style=\"%sline-height:1em;color:#202951;font-weight:700;font-size:1.85em;\n" +
      "padding-bottom:15px;margin:0;\">%s</h1>",
      makeSubtitle(title), STYLE_FONT, content);
  }

  private static String tdElement(String border_color, String padding, String weight,
      String content) {
    return  String.format(
      "<td style=\"border-top: 1px solid %s;padding:%s;%svertical-align:top;\">%s</td>\n",
      border_color, padding, weight, content);
  }

  private static String preElement(String content) {
    return String.format(
      "<pre style=\"background-color:#f7f7f7;border: 1px solid #e5e5e9;padding:10px;\n" +
      "white-space: pre-wrap;border-radius:5px;\">%s</pre>\n", content);
  }
  // @formatter:off

  private static String assembleMailRow(JsonNode rowData, boolean isFirstCcheck, String timestamp) {

    String tsSpan = isFirstCcheck
        ? String.format("<div style=\"color:#ffffff;font-size:5px;\">%s</div>", timestamp)
        : "";
    String process = rowData.has("process") ? rowData.get("process").asText() : "";
    String borderColor = isFirstCcheck ? "transparent" : "#e5e5e9";

    // @formatter:off
    String badge = rowData.get("has_error").asBoolean()
        ? "<span style=\"font-size:0.8em;background-color:#E8473F;color:#ffffff;\n" +
          "border-radius:2px;margin-right:8px;padding:2px 4px;font-weight:400;\">\n" +
          "Failed</span>\n"
        : "";
    // @formatter:on

    String detailsContentOk = "<div style=\"padding:11px 0;\">Ok</div>";
    ArrayNode dataDetails = (ArrayNode) rowData.get("details");
    List<String> dataDetailsStr = new ArrayList<>();
    dataDetails.forEach(value -> dataDetailsStr.add(value.asText()));

    String detailsContent = dataDetails.size() == 0 ? detailsContentOk
        : preElement(
            String.join("Disk utilization".equals(rowData.get("message").asText()) ? "\n" : " ",
                dataDetailsStr));

    return String.format("<tr>%s %s %s</tr>\n",
        tdElement(borderColor, "10px 20px 10px 0", "font-weight:700;font-size:1.2em;",
            badge + rowData.get("message") + tsSpan),
        tdElement(borderColor, "10px 20px 10px 0", "", process),
        tdElement(borderColor, "0", "", detailsContent));
  }

  /**
   * Pretty prints the passed report excluding some nodes if needed.
   *
   * @param report  Source json object
   * @param reportOnlyErrors
   * @return  Pretty-printed object (String)
   */
  public String asPlainText(JsonNode report, boolean reportOnlyErrors) {
    if (!reportOnlyErrors) {
      return prettyPrint(report);
    }

    JsonNode localReport = report.deepCopy();
    ArrayNode data = (ArrayNode) localReport.path("data");

    for (int i = data.size() - 1; i >= 0; i--) {
      JsonNode item = data.get(i);
      if (!item.path("has_error").asBoolean()) {
        data.remove(i);
      }
    }
    return prettyPrint(localReport);
  }

  private static String prettyPrint(JsonNode obj) {
    ObjectMapper mapper = new ObjectMapper();

    // Setup a pretty printer with an indenter (2 spaces)
    DefaultPrettyPrinter.Indenter indenter = new DefaultIndenter("  ", DefaultIndenter.SYS_LF);
    DefaultPrettyPrinter printer = new DefaultPrettyPrinter();
    printer.indentObjectsWith(indenter);
    printer.indentArraysWith(indenter);

    try {
      return mapper.writer(printer).writeValueAsString(obj);
    } catch (JsonProcessingException e) {
      throw new RuntimeException("Can not pretty print a Json object.");
    }
  }
}
