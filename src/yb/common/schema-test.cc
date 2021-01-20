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

#include "yb/common/schema.h"

#include <unordered_map>
#include <vector>

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "yb/common/key_encoder.h"
#include "yb/common/row.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/util/hexdump.h"
#include "yb/util/stopwatch.h"
#include "yb/util/test_macros.h"

namespace yb {
namespace tablet {

using std::unordered_map;
using std::vector;
using strings::Substitute;

// Copy a row and its referenced data into the given Arena.
static Status CopyRowToArena(const Slice &row,
                             const Schema &schema,
                             Arena *dst_arena,
                             ContiguousRow *copied) {
  Slice row_data;

  // Copy the direct row data to arena
  if (!dst_arena->RelocateSlice(row, &row_data)) {
    return STATUS(IOError, "no space for row data in arena");
  }

  copied->Reset(row_data.mutable_data());
  RETURN_NOT_OK(RelocateIndirectDataToArena(copied, dst_arena));
  return Status::OK();
}



// Test basic functionality of Schema definition
TEST(TestSchema, TestSchema) {
  Schema empty_schema;
  ASSERT_GT(empty_schema.memory_footprint_excluding_this(), 0);

  ColumnSchema col1("key", STRING);
  ColumnSchema col2("uint32val", UINT32, true);
  ColumnSchema col3("int32val", INT32);

  vector<ColumnSchema> cols = { col1, col2, col3 };
  Schema schema(cols, 1);

  ASSERT_EQ(sizeof(Slice) + sizeof(uint32_t) + sizeof(int32_t),
            schema.byte_size());
  ASSERT_EQ(3, schema.num_columns());
  ASSERT_EQ(0, schema.column_offset(0));
  ASSERT_EQ(sizeof(Slice), schema.column_offset(1));
  ASSERT_GT(schema.memory_footprint_excluding_this(),
            empty_schema.memory_footprint_excluding_this());

  EXPECT_EQ("Schema [\n"
            "\tkey[string NOT NULL NOT A PARTITION KEY],\n"
            "\tuint32val[uint32 NULLABLE NOT A PARTITION KEY],\n"
            "\tint32val[int32 NOT NULL NOT A PARTITION KEY]\n"
            "]\nproperties: contain_counters: false is_transactional: false "
            "consistency_level: STRONG "
            "use_mangled_column_name: false "
            "is_ysql_catalog_table: false "
            "retain_delete_markers: false",
            schema.ToString());
  EXPECT_EQ("key[string NOT NULL NOT A PARTITION KEY]", schema.column(0).ToString());
  EXPECT_EQ("uint32 NULLABLE NOT A PARTITION KEY", schema.column(1).TypeToString());
}

TEST(TestSchema, TestSwap) {
  TableProperties properties1;
  properties1.SetDefaultTimeToLive(1000);
  Schema schema1({ ColumnSchema("col1", STRING),
                   ColumnSchema("col2", STRING),
                   ColumnSchema("col3", UINT32) },
                 2, properties1);
  TableProperties properties2;
  properties2.SetDefaultTimeToLive(2000);
  Schema schema2({ ColumnSchema("col3", UINT32),
                   ColumnSchema("col2", STRING) },
                 1, properties2);
  schema1.swap(schema2);
  ASSERT_EQ(2, schema1.num_columns());
  ASSERT_EQ(1, schema1.num_key_columns());
  ASSERT_EQ(3, schema2.num_columns());
  ASSERT_EQ(2, schema2.num_key_columns());
  ASSERT_EQ(2000, schema1.table_properties().DefaultTimeToLive());
  ASSERT_EQ(1000, schema2.table_properties().DefaultTimeToLive());
}

TEST(TestSchema, TestReset) {
  Schema schema;
  ASSERT_FALSE(schema.initialized());

  ASSERT_OK(schema.Reset({ ColumnSchema("col3", UINT32),
                           ColumnSchema("col2", STRING) },
                         1));
  ASSERT_TRUE(schema.initialized());

  // Swap the initialized schema with an uninitialized one.
  Schema schema2;
  schema2.swap(schema);
  ASSERT_FALSE(schema.initialized());
  ASSERT_TRUE(schema2.initialized());
}

// Test for KUDU-943, a bug where we suspected that Variant didn't behave
// correctly with empty strings.
TEST(TestSchema, TestEmptyVariant) {
  Slice empty_val("");
  Slice nonempty_val("test");

  Variant v(STRING, &nonempty_val);
  ASSERT_EQ("test", (static_cast<const Slice*>(v.value()))->ToString());
  v.Reset(STRING, &empty_val);
  ASSERT_EQ("", (static_cast<const Slice*>(v.value()))->ToString());
  v.Reset(STRING, &nonempty_val);
  ASSERT_EQ("test", (static_cast<const Slice*>(v.value()))->ToString());
}

TEST(TestSchema, TestProjectSubset) {
  Schema schema1({ ColumnSchema("col1", STRING),
                   ColumnSchema("col2", STRING),
                   ColumnSchema("col3", UINT32) },
                 1);

  Schema schema2({ ColumnSchema("col3", UINT32),
                   ColumnSchema("col2", STRING) },
                 0);

  RowProjector row_projector(&schema1, &schema2);
  ASSERT_OK(row_projector.Init());

  // Verify the mapping
  ASSERT_EQ(2, row_projector.base_cols_mapping().size());
  ASSERT_EQ(0, row_projector.adapter_cols_mapping().size());

  const vector<RowProjector::ProjectionIdxMapping>& mapping = row_projector.base_cols_mapping();
  ASSERT_EQ(mapping[0].first, 0);  // col3 schema2
  ASSERT_EQ(mapping[0].second, 2); // col3 schema1
  ASSERT_EQ(mapping[1].first, 1);  // col2 schema2
  ASSERT_EQ(mapping[1].second, 1); // col2 schema1
}

// Test projection when the type of the projected column
// doesn't match the original type.
TEST(TestSchema, TestProjectTypeMismatch) {
  Schema schema1({ ColumnSchema("key", STRING),
                   ColumnSchema("val", UINT32) },
                 1);
  Schema schema2({ ColumnSchema("val", STRING) }, 0);

  RowProjector row_projector(&schema1, &schema2);
  Status s = row_projector.Init();
  ASSERT_TRUE(s.IsInvalidArgument());
  ASSERT_STR_CONTAINS(s.message().ToString(), "must have type");
}

// Test projection when the some columns in the projection
// are not present in the base schema
TEST(TestSchema, TestProjectMissingColumn) {
  Schema schema1({ ColumnSchema("key", STRING), ColumnSchema("val", UINT32) }, 1);
  Schema schema2({ ColumnSchema("val", UINT32), ColumnSchema("non_present", STRING) }, 0);
  Schema schema3({ ColumnSchema("val", UINT32), ColumnSchema("non_present", UINT32, true) }, 0);

  RowProjector row_projector(&schema1, &schema2);
  Status s = row_projector.Init();
  ASSERT_TRUE(s.IsInvalidArgument());
  ASSERT_STR_CONTAINS(s.message().ToString(),
    "does not exist in the projection, and it does not have a nullable type");

  // Verify Default nullable column with no default value
  ASSERT_OK(row_projector.Reset(&schema1, &schema3));

  ASSERT_EQ(1, row_projector.base_cols_mapping().size());
  ASSERT_EQ(0, row_projector.adapter_cols_mapping().size());

  ASSERT_EQ(row_projector.base_cols_mapping()[0].first, 0);  // val schema2
  ASSERT_EQ(row_projector.base_cols_mapping()[0].second, 1); // val schema1
}

// Test projection mapping using IDs.
// This simulate a column rename ('val' -> 'val_renamed')
// and a new column added ('non_present')
TEST(TestSchema, TestProjectRename) {
  SchemaBuilder builder;
  ASSERT_OK(builder.AddKeyColumn("key", STRING));
  ASSERT_OK(builder.AddColumn("val", UINT32));
  Schema schema1 = builder.Build();

  builder.Reset(schema1);
  ASSERT_OK(builder.AddNullableColumn("non_present", UINT32));
  ASSERT_OK(builder.RenameColumn("val", "val_renamed"));
  Schema schema2 = builder.Build();

  RowProjector row_projector(&schema1, &schema2);
  ASSERT_OK(row_projector.Init());

  ASSERT_EQ(2, row_projector.base_cols_mapping().size());
  ASSERT_EQ(0, row_projector.adapter_cols_mapping().size());

  ASSERT_EQ(row_projector.base_cols_mapping()[0].first, 0);  // key schema2
  ASSERT_EQ(row_projector.base_cols_mapping()[0].second, 0); // key schema1

  ASSERT_EQ(row_projector.base_cols_mapping()[1].first, 1);  // val_renamed schema2
  ASSERT_EQ(row_projector.base_cols_mapping()[1].second, 1); // val schema1
}


// Test that the schema can be used to compare and stringify rows.
TEST(TestSchema, TestRowOperations) {
  Schema schema({ ColumnSchema("col1", STRING),
                  ColumnSchema("col2", STRING),
                  ColumnSchema("col3", UINT32),
                  ColumnSchema("col4", INT32) },
                1);

  Arena arena(1024, 256*1024);

  RowBuilder rb(schema);
  rb.AddString(string("row_a_1"));
  rb.AddString(string("row_a_2"));
  rb.AddUint32(3);
  rb.AddInt32(-3);
  ContiguousRow row_a(&schema);
  ASSERT_OK(CopyRowToArena(rb.data(), schema, &arena, &row_a));

  rb.Reset();
  rb.AddString(string("row_b_1"));
  rb.AddString(string("row_b_2"));
  rb.AddUint32(3);
  rb.AddInt32(-3);
  ContiguousRow row_b(&schema);
  ASSERT_OK(CopyRowToArena(rb.data(), schema, &arena, &row_b));

  ASSERT_GT(schema.Compare(row_b, row_a), 0);
  ASSERT_LT(schema.Compare(row_a, row_b), 0);

  ASSERT_EQ(string("(string col1=row_a_1, string col2=row_a_2, uint32 col3=3, int32 col4=-3)"),
            schema.DebugRow(row_a));
}

TEST(TestKeyEncoder, TestKeyEncoder) {
  faststring fs;
  const KeyEncoder<faststring>& encoder = GetKeyEncoder<faststring>(GetTypeInfo(STRING));

  typedef std::tuple<vector<Slice>, Slice> test_pair;

  vector<test_pair> pairs;

  // Simple key
  pairs.push_back(test_pair({ Slice("foo", 3) }, Slice("foo", 3)));

  // Simple compound key
  pairs.push_back(test_pair({ Slice("foo", 3), Slice("bar", 3) },
                            Slice("foo" "\x00\x00" "bar", 8)));

  // Compound key with a \x00 in it
  pairs.push_back(test_pair({ Slice("xxx\x00yyy", 7), Slice("bar", 3) },
                            Slice("xxx" "\x00\x01" "yyy" "\x00\x00" "bar", 13)));

  int i = 0;
  for (const test_pair &t : pairs) {
    const vector<Slice> &in = std::get<0>(t);
    Slice expected = std::get<1>(t);

    fs.clear();
    for (int col = 0; col < in.size(); col++) {
      encoder.Encode(&in[col], col == in.size() - 1, &fs);
    }

    ASSERT_EQ(0, expected.compare(Slice(fs)))
      << "Failed encoding example " << i << ".\n"
      << "Expected: " << HexDump(expected) << "\n"
      << "Got:      " << HexDump(Slice(fs));
    i++;
  }
}

TEST(TestSchema, TestDecodeKeys_CompoundStringKey) {
  Schema schema({ ColumnSchema("col1", STRING),
                  ColumnSchema("col2", STRING),
                  ColumnSchema("col3", STRING) },
                2);

  EXPECT_EQ("(string col1=foo, string col2=bar)",
            schema.DebugEncodedRowKey(Slice("foo\0\0bar", 8), Schema::START_KEY));
  EXPECT_EQ("(string col1=fo\\000o, string col2=bar)",
            schema.DebugEncodedRowKey(Slice("fo\x00\x01o\0\0""bar", 10), Schema::START_KEY));
  EXPECT_EQ("(string col1=fo\\000o, string col2=bar\\000xy)",
            schema.DebugEncodedRowKey(Slice("fo\x00\x01o\0\0""bar\0xy", 13), Schema::START_KEY));

  EXPECT_EQ("<start of table>",
            schema.DebugEncodedRowKey("", Schema::START_KEY));
  EXPECT_EQ("<end of table>",
            schema.DebugEncodedRowKey("", Schema::END_KEY));
}

// Test that appropriate statuses are returned when trying to decode an invalid
// encoded key.
TEST(TestSchema, TestDecodeKeys_InvalidKeys) {
  Schema schema({ ColumnSchema("col1", STRING),
                  ColumnSchema("col2", UINT32),
                  ColumnSchema("col3", STRING) },
                2);

  EXPECT_EQ("<invalid key: Invalid argument: Error decoding composite key component"
            " 'col1': Missing separator after composite key string component: foo>",
            schema.DebugEncodedRowKey(Slice("foo"), Schema::START_KEY));
  EXPECT_EQ("<invalid key: Invalid argument: Error decoding composite key component 'col2': "
            "key too short>",
            schema.DebugEncodedRowKey(Slice("foo\x00\x00", 5), Schema::START_KEY));
  EXPECT_EQ("<invalid key: Invalid argument: Error decoding composite key component 'col2': "
            "key too short: FFFF>",
            schema.DebugEncodedRowKey(Slice("foo\x00\x00\xff\xff", 7), Schema::START_KEY));
}

TEST(TestSchema, TestCreateProjection) {
  Schema schema({ ColumnSchema("col1", STRING),
                  ColumnSchema("col2", STRING),
                  ColumnSchema("col3", STRING),
                  ColumnSchema("col4", STRING),
                  ColumnSchema("col5", STRING) },
                2);
  Schema schema_with_ids = SchemaBuilder(schema).Build();
  Schema partial_schema;

  // By names, without IDs
  ASSERT_OK(schema.CreateProjectionByNames({ "col1", "col2", "col4" }, &partial_schema));
  EXPECT_EQ("Schema [\n"
            "\tcol1[string NOT NULL NOT A PARTITION KEY],\n"
            "\tcol2[string NOT NULL NOT A PARTITION KEY],\n"
            "\tcol4[string NOT NULL NOT A PARTITION KEY]\n"
            "]\nproperties: contain_counters: false is_transactional: false "
            "consistency_level: STRONG "
            "use_mangled_column_name: false "
            "is_ysql_catalog_table: false "
            "retain_delete_markers: false",
            partial_schema.ToString());

  // By names, with IDS
  ASSERT_OK(schema_with_ids.CreateProjectionByNames({ "col1", "col2", "col4" }, &partial_schema));
  EXPECT_EQ(Substitute("Schema [\n"
                       "\t$0:col1[string NOT NULL NOT A PARTITION KEY],\n"
                       "\t$1:col2[string NOT NULL NOT A PARTITION KEY],\n"
                       "\t$2:col4[string NOT NULL NOT A PARTITION KEY]\n"
                       "]\nproperties: contain_counters: false is_transactional: false "
                       "consistency_level: STRONG "
                       "use_mangled_column_name: false "
                       "is_ysql_catalog_table: false "
                       "retain_delete_markers: false",
                       schema_with_ids.column_id(0),
                       schema_with_ids.column_id(1),
                       schema_with_ids.column_id(3)),
            partial_schema.ToString());

  // By names, with missing names.
  Status s = schema.CreateProjectionByNames({ "foobar" }, &partial_schema);
  EXPECT_EQ("Not found: column not found: foobar", s.ToString(/* no file/line */ false));

  // By IDs
  ASSERT_OK(schema_with_ids.CreateProjectionByIdsIgnoreMissing({ schema_with_ids.column_id(0),
                                                                 schema_with_ids.column_id(1),
                                                                 ColumnId(1000), // missing column
                                                                 schema_with_ids.column_id(3) },
                                                               &partial_schema));
  EXPECT_EQ(Substitute("Schema [\n"
                       "\t$0:col1[string NOT NULL NOT A PARTITION KEY],\n"
                       "\t$1:col2[string NOT NULL NOT A PARTITION KEY],\n"
                       "\t$2:col4[string NOT NULL NOT A PARTITION KEY]\n"
                       "]\nproperties: contain_counters: false is_transactional: false "
                       "consistency_level: STRONG "
                       "use_mangled_column_name: false "
                       "is_ysql_catalog_table: false "
                       "retain_delete_markers: false",
                       schema_with_ids.column_id(0),
                       schema_with_ids.column_id(1),
                       schema_with_ids.column_id(3)),
            partial_schema.ToString());
}

TEST(TestSchema, TestCopyFrom) {
  TableProperties properties;
  properties.SetDefaultTimeToLive(1000);
  Schema schema1({ ColumnSchema("col1", STRING),
                     ColumnSchema("col2", STRING),
                     ColumnSchema("col3", UINT32) },
                 2, properties);
  Schema schema2;
  schema2.CopyFrom(schema1);
  ASSERT_EQ(3, schema2.num_columns());
  ASSERT_EQ(2, schema2.num_key_columns());
  ASSERT_EQ(1000, schema2.table_properties().DefaultTimeToLive());
}

TEST(TestSchema, TestSchemaBuilder) {
  TableProperties properties;
  properties.SetDefaultTimeToLive(1000);
  Schema schema1({ ColumnSchema("col1", STRING),
                     ColumnSchema("col2", STRING),
                     ColumnSchema("col3", UINT32) },
                 2, properties);
  SchemaBuilder builder(schema1);
  Schema schema2 = builder.Build();
  ASSERT_TRUE(schema1.Equals(schema2));
}

TEST(TestSchema, TestTableProperties) {
  TableProperties properties;
  ASSERT_FALSE(properties.HasDefaultTimeToLive());

  properties.SetDefaultTimeToLive(1000);
  ASSERT_TRUE(properties.HasDefaultTimeToLive());
  ASSERT_EQ(1000, properties.DefaultTimeToLive());

  TableProperties properties1;
  properties1.SetDefaultTimeToLive(1000);
  ASSERT_EQ(properties, properties1);

  TablePropertiesPB pb;
  properties.ToTablePropertiesPB(&pb);
  ASSERT_TRUE(pb.has_default_time_to_live());
  ASSERT_EQ(1000, pb.default_time_to_live());

  auto properties2 = TableProperties::FromTablePropertiesPB(pb);
  ASSERT_TRUE(properties2.HasDefaultTimeToLive());
  ASSERT_EQ(1000, properties2.DefaultTimeToLive());

  properties.Reset();
  pb.Clear();
  ASSERT_FALSE(properties.HasDefaultTimeToLive());
  properties.ToTablePropertiesPB(&pb);
  ASSERT_FALSE(pb.has_default_time_to_live());
  auto properties3 = TableProperties::FromTablePropertiesPB(pb);
  ASSERT_FALSE(properties3.HasDefaultTimeToLive());
}

#ifdef NDEBUG
TEST(TestKeyEncoder, BenchmarkSimpleKey) {
  faststring fs;
  Schema schema({ ColumnSchema("col1", STRING) }, 1);

  RowBuilder rb(schema);
  rb.AddString(Slice("hello world"));
  ConstContiguousRow row(&rb.schema(), rb.data());

  LOG_TIMING(INFO, "Encoding") {
    for (int i = 0; i < 10000000; i++) {
      schema.EncodeComparableKey(row, &fs);
    }
  }
}
#endif

} // namespace tablet
} // namespace yb
