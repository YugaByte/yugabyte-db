# Copyright (c) YugaByte, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
# in compliance with the License.  You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software distributed under the License
# is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
# or implied.  See the License for the specific language governing permissions and limitations
# under the License.
#

add_library(ql_exec
            eval_bcall.cc
            eval_const.cc
            eval_expr.cc
            eval_logic.cc
            eval_op.cc
            eval_col.cc
            eval_where.cc
            eval_misc.cc
            eval_aggr.cc
            eval_json.cc
            exec_context.cc
            executor.cc)

target_link_libraries(ql_exec
                      ql_parser
                      ql_audit
                      yb_client
                      yb_util)
add_dependencies(ql_exec ql_parser ql_audit)
