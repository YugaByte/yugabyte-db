// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
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
package org.yb.client;

import com.google.common.base.Charsets;
import com.google.common.collect.ImmutableList;
import com.google.common.net.HostAndPort;
import com.google.protobuf.ByteString;
import com.google.protobuf.UnsafeByteOperations;
import org.yb.*;
import org.yb.annotations.InterfaceAudience;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.Charset;
import java.util.ArrayList;
import java.util.List;
import java.util.stream.Collectors;

@InterfaceAudience.Private
public class ProtobufHelper {

  /**
   * Utility method to convert a Schema to its wire format.
   * @param schema Schema to convert
   * @return a list of ColumnSchemaPB
   */
  public static List<Common.ColumnSchemaPB> schemaToListPb(Schema schema) {
    ArrayList<Common.ColumnSchemaPB> columns =
        new ArrayList<Common.ColumnSchemaPB>(schema.getColumnCount());
    Common.ColumnSchemaPB.Builder schemaBuilder = Common.ColumnSchemaPB.newBuilder();
    for (ColumnSchema col : schema.getColumns()) {
      columns.add(columnToPb(schemaBuilder, col));
      schemaBuilder.clear();
    }
    return columns;
  }

  public static Common.SchemaPB schemaToPb(Schema schema) {
    Common.SchemaPB.Builder builder = Common.SchemaPB.newBuilder();
    builder.addAllColumns(schemaToListPb(schema));
    Common.TablePropertiesPB.Builder tablePropsBuilder = builder.getTablePropertiesBuilder();
    tablePropsBuilder.setDefaultTimeToLive(schema.getTimeToLiveInMillis());
    builder.setTableProperties(tablePropsBuilder.build());
    return builder.build();
  }

  public static Common.ColumnSchemaPB columnToPb(ColumnSchema column) {
    return columnToPb(Common.ColumnSchemaPB.newBuilder(), column);
  }

  public static Common.QLTypePB QLTypeToPb(QLType yqlType) {
    Common.QLTypePB.Builder typeBuilder = Common.QLTypePB.newBuilder();
    typeBuilder.setMain(yqlType.getMain());
    typeBuilder.addAllParams(yqlType.getParams().stream()
                                                .map(ProtobufHelper::QLTypeToPb)
                                                .collect(Collectors.toList()));
    // User-defined types have additional information (set declared keyspace and type name).
    if (yqlType.getMain() == Common.DataType.USER_DEFINED_TYPE) {
      Common.QLTypePB.UDTypeInfo.Builder udtBuilder = Common.QLTypePB.UDTypeInfo.newBuilder();
      udtBuilder.setName(yqlType.getUdtName());
      udtBuilder.setKeyspaceName(yqlType.getUdtKeyspaceName());
      typeBuilder.setUdtypeInfo(udtBuilder);
    }

    return typeBuilder.build();
  }

  public static Common.ColumnSchemaPB
  columnToPb(Common.ColumnSchemaPB.Builder schemaBuilder, ColumnSchema column) {
    schemaBuilder
        .setName(column.getName())
        .setType(QLTypeToPb(column.getQLType()))
        .setIsKey(column.isKey())
        .setIsHashKey(column.isHashKey())
        .setIsNullable(column.isNullable());
    if (column.getSortOrder() != ColumnSchema.SortOrder.NONE) {
      schemaBuilder.setSortingType(column.getSortOrder().getValue());
    }
    return schemaBuilder.build();
  }

  public static Schema pbToSchema(Common.SchemaPB schemaPB) {
    List<ColumnSchema> columns = new ArrayList<>(schemaPB.getColumnsCount());
    List<Integer> columnIds = new ArrayList<>(schemaPB.getColumnsCount());
    long timeToLive = Schema.defaultTTL;
    if (schemaPB.hasTableProperties() && schemaPB.getTableProperties().hasDefaultTimeToLive()) {
      timeToLive = schemaPB.getTableProperties().getDefaultTimeToLive();
    }
    for (Common.ColumnSchemaPB columnPb : schemaPB.getColumnsList()) {
      int id = columnPb.getId();
      if (id < 0) {
        throw new IllegalArgumentException("Illegal column ID " + id + " provided for column " +
                                           columnPb.getName());
      }
      columnIds.add(id);
      Type type = Type.getTypeForDataType(columnPb.getType().getMain());
      ColumnSchema.SortOrder sortOrder =
          ColumnSchema.SortOrder.findFromValue(columnPb.getSortingType());
      ColumnSchema column = new ColumnSchema.ColumnSchemaBuilder(columnPb.getName(), type)
          .id(id)
          .rangeKey(columnPb.getIsKey(), sortOrder)
          .hashKey(columnPb.getIsHashKey())
          .nullable(columnPb.getIsNullable())
          .build();
      columns.add(column);
    }
    return new Schema(columns, columnIds, timeToLive);
  }

  /**
   * Factory method for creating a {@code PartitionSchema} from a protobuf message.
   *
   * @param pb the partition schema protobuf message
   * @return a partition instance
   */
  static PartitionSchema pbToPartitionSchema(Common.PartitionSchemaPB pb, Schema schema) {
    List<Integer> rangeColumns = pbToIds(pb.getRangeSchema().getColumnsList());
    PartitionSchema.RangeSchema rangeSchema = new PartitionSchema.RangeSchema(rangeColumns);

    ImmutableList.Builder<PartitionSchema.HashBucketSchema> hashBucketSchemas =
        ImmutableList.builder();

    for (Common.PartitionSchemaPB.HashBucketSchemaPB hashBucketSchemaPB
        : pb.getHashBucketSchemasList()) {
      List<Integer> hashColumnIds = pbToIds(hashBucketSchemaPB.getColumnsList());

      PartitionSchema.HashBucketSchema hashBucketSchema =
          new PartitionSchema.HashBucketSchema(hashColumnIds,
                                               hashBucketSchemaPB.getNumBuckets(),
                                               hashBucketSchemaPB.getSeed());

      hashBucketSchemas.add(hashBucketSchema);
    }

    return new PartitionSchema(rangeSchema, hashBucketSchemas.build(), schema, pb.getHashSchema());
  }

  /**
   * Constructs a new {@code Partition} instance from the a protobuf message.
   * @param pb the protobuf message
   * @return the {@code Partition} corresponding to the message
   */
  static Partition pbToPartition(Common.PartitionPB pb) {
    return new Partition(pb.getPartitionKeyStart().toByteArray(),
                         pb.getPartitionKeyEnd().toByteArray(),
                         pb.getHashBucketsList());
  }

  /**
   * Deserializes a list of column identifier protobufs into a list of column IDs. This method
   * relies on the fact that the master will aways send a partition schema with column IDs, and not
   * column names (column names are only used when the client is sending the partition schema to
   * the master as part of the create table process).
   *
   * @param columnIdentifiers the column identifiers
   * @return the column IDs
   */
  private static List<Integer> pbToIds(
      List<Common.PartitionSchemaPB.ColumnIdentifierPB> columnIdentifiers) {
    ImmutableList.Builder<Integer> columnIds = ImmutableList.builder();
    for (Common.PartitionSchemaPB.ColumnIdentifierPB column : columnIdentifiers) {
      switch (column.getIdentifierCase()) {
        case ID:
          columnIds.add(column.getId());
          break;
        case NAME:
          throw new IllegalArgumentException(
              String.format("Expected column ID from master: %s", column));
        case IDENTIFIER_NOT_SET:
          throw new IllegalArgumentException("Unknown column: " + column);
      }
    }
    return columnIds.build();
  }

  private static byte[] objectToWireFormat(ColumnSchema col, Object value) {
    switch (col.getType()) {
      case BOOL:
        return Bytes.fromBoolean((Boolean) value);
      case INT8:
        return new byte[] {(Byte) value};
      case INT16:
        return Bytes.fromShort((Short) value);
      case INT32:
        return Bytes.fromInt((Integer) value);
      case INT64:
      case TIMESTAMP:
        return Bytes.fromLong((Long) value);
      case STRING:
        return ((String) value).getBytes(Charsets.UTF_8);
      case BINARY:
        return (byte[]) value;
      case FLOAT:
        return Bytes.fromFloat((Float) value);
      case DOUBLE:
        return Bytes.fromDouble((Double) value);
      default:
        throw new IllegalArgumentException("The column " + col.getName() + " is of type " + col
            .getType() + " which is unknown");
    }
  }

  private static Object byteStringToObject(Type type, ByteString value) {
    ByteBuffer buf = value.asReadOnlyByteBuffer();
    buf.order(ByteOrder.LITTLE_ENDIAN);
    switch (type) {
      case BOOL:
        return buf.get() != 0;
      case INT8:
        return buf.get();
      case INT16:
        return buf.getShort();
      case INT32:
        return buf.getInt();
      case INT64:
        return buf.getLong();
      case FLOAT:
        return buf.getFloat();
      case DOUBLE:
        return buf.getDouble();
      case STRING:
        return value.toStringUtf8();
      case BINARY:
        return value.toByteArray();
      default:
        throw new IllegalArgumentException("This type is unknown: " + type);
    }
  }

  /**
   * Convert a {@link com.google.common.net.HostAndPort} to {@link org.yb.Common.HostPortPB}
   * protobuf message for serialization.
   * @param hostAndPort The host and port object. Both host and port must be specified.
   * @return An initialized HostPortPB object.
   */
  public static Common.HostPortPB hostAndPortToPB(HostAndPort hostAndPort) {
    return Common.HostPortPB.newBuilder()
        .setHost(hostAndPort.getHost())
        .setPort(hostAndPort.getPort())
        .build();
  }

  /**
   * Convert a {@link org.yb.Common.HostPortPB} to {@link com.google.common.net.HostAndPort}.
   * @param hostPortPB The fully initialized HostPortPB object. Must have both host and port
   *                   specified.
   * @return An initialized initialized HostAndPort object.
   */
  public static HostAndPort hostAndPortFromPB(Common.HostPortPB hostPortPB) {
    return HostAndPort.fromParts(hostPortPB.getHost(), hostPortPB.getPort());
  }
}
