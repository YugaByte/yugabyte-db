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

#ifndef YB_COMMON_COMMON_FWD_H
#define YB_COMMON_COMMON_FWD_H

#include <memory>

namespace yb {

class ClockBase;
class HybridTime;
class IndexInfo;
class IndexMap;
class PartitionSchema;

class Schema;
typedef std::shared_ptr<Schema> SchemaPtr;

class PgsqlBCallPB;
class PgsqlConditionPB;
class PgsqlExpressionPB;
class PgsqlRSRowDescPB;
class PgsqlWriteRequestPB;

class QLExprExecutor;
typedef std::shared_ptr<QLExprExecutor> QLExprExecutorPtr;

class QLJsonColumnOperationsPB;
class QLJsonOperationPB;
class QLRSColDescPB;
class QLRSRowDescPB;
class QLTableRow;
class QLType;
class QLValue;
class QLValuePB;
class TableProperties;

struct ColumnId;
struct OpId;
struct QLTableColumn;

enum class PgSystemAttrNum : int;

namespace common {

class Jsonb;

} // namespace common

} // namespace yb

#endif // YB_COMMON_COMMON_FWD_H
