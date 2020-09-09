//--------------------------------------------------------------------------------------------------
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
//--------------------------------------------------------------------------------------------------

#include "yb/yql/cql/ql/ptree/process_context.h"
#include "yb/util/logging.h"

namespace yb {
namespace ql {

using std::endl;
using std::istream;
using std::min;
using std::string;

//--------------------------------------------------------------------------------------------------
// ProcessContextBase
//--------------------------------------------------------------------------------------------------

ProcessContextBase::ProcessContextBase() {
}

ProcessContextBase::~ProcessContextBase() {
}

//--------------------------------------------------------------------------------------------------

MCString* ProcessContextBase::error_msgs() {
  if (error_msgs_ == nullptr) {
    error_msgs_.reset(new MCString(PTempMem()));
  }
  return error_msgs_.get();
}

Status ProcessContextBase::GetStatus() {
  // Erroneous index is negative while successful index is non-negative.
  if (error_code_ < ErrorCode::SUCCESS) {
    return STATUS(QLError, error_msgs()->c_str(), Slice(), QLError(error_code_));
  }
  return Status::OK();
}

//--------------------------------------------------------------------------------------------------

void ProcessContextBase::Warn(const YBLocation& loc, const string& msg, ErrorCode error_code) {
  error_code_ = error_code;
  LOG(WARNING) << kErrorFontStart << "SQL Warning (" << loc << "): " << msg << kErrorFontEnd
               << endl;
}

//--------------------------------------------------------------------------------------------------

Status ProcessContextBase::Error(const YBLocation& loc,
                                 const char *m,
                                 ErrorCode error_code,
                                 const char* token) {
  error_code_ = error_code;

  // Form an error message.
  MCString msg(PTempMem());

  // Concatenate error code.
  msg += ErrorText(error_code);

  if (strcmp(m, "") != 0) {
    msg += ". ";
    // Concatenate error message.
    msg += m;
  }

  // Concantenate token.
  msg += "\n";
  if (token == nullptr) {
    // Try to mark the error token from the input statement in the error message.
    // This may not be possible if:
    //   1. Bison reports a wrong/invalid error location (e.g. ENG-2052).
    //   2. Theoretically, if input statement is empty.
    bool wrote_token = false;
    if (!stmt().empty()) {
      // Bison-reported line/column numbers start from 1, we use 0-based numbering here.
      const int err_begin_line = loc.BeginLine() - 1;
      const int err_begin_column = loc.BeginColumn() - 1;
      const int err_end_line = loc.EndLine() - 1;
      const int err_end_column = loc.EndColumn() - 1;

      // Error location values should have sane lower bound.
      // The reported values may exceed upper bound (ENG-2052) so we handle that in code below.
      DCHECK_GE(err_begin_line, 0);
      DCHECK_GE(err_begin_column, 0);
      DCHECK_GE(err_end_line, 0);
      DCHECK_GE(err_end_column, 0);

      int curr_line = 0;
      int curr_col = 0;
      const char *stmt_begin = stmt().c_str();
      const char *stmt_end = stmt_begin + stmt().length();
      const char *curr_char = stmt_begin;

      while (curr_char <= stmt_end) {
        if (curr_char == stmt_end || *curr_char == '\n') { // End of stmt/line char.
          msg += '\n';

          // If in error-token range, try writing line marking error location with '^'.
          if (curr_line >= err_begin_line && curr_line <= err_end_line) {
            const char *line_start = curr_char - curr_col; // Inclusive, first char of line.
            const char *line_end = curr_char - 1; // Inclusive, last char of line.

            // Line start and end should be within statement bounds.
            DCHECK_GE(line_start, stmt_begin);
            DCHECK_LT(line_start, stmt_end);
            DCHECK_GE(line_end, stmt_begin);
            DCHECK_LT(line_end, stmt_end);

            // Finding error-token start, left-trim spaces if this is first line of the error token.
            const char *error_start = line_start; // Inclusive, first char of error token.
            if (curr_line == err_begin_line) {
              error_start += err_begin_column;
            }
            if (!wrote_token) {
              while (error_start <= line_end && isspace(*error_start)) {
                error_start++;
              }
            }

            // Finding error-token end, right-trim spaces if this is last line of the error token.
            const char *error_end = line_end; // Inclusive, last char of error token.
            if (curr_line == err_end_line) {
              // The end-column location reported by Bison is generally exclusive (i.e. character
              // after the error token) so by default we subtract one from reported value.
              // ENG-2052: End-column value may be wrong/out-of-bounds so we cap value to line end.
              error_end = std::min(line_start + err_end_column - 1, line_end);

              while (error_end >= error_start && isspace(*error_end)) {
                error_end--;
              }
            }

            // If we found a valid token range write a marker line.
            if (error_end >= error_start) {
              msg.append(error_start - line_start, ' ');
              msg.append(error_end - error_start + 1, '^'); // +1 since both limits are inclusive.
              msg += '\n';
              wrote_token = true;
            }
          }

          curr_line++;
          curr_col = 0;
        } else {
          msg += *curr_char;
          curr_col++;
        }
        curr_char++;
      }
    }

    // If we couldn't mark the error token in the stmt we append the reported location directly.
    if (!wrote_token) {
      msg += "At location: (";
      loc.ToString<MCString>(&msg, false /* starting_location_only */);
      msg += ")\n";
    }
  } else {
    msg += token;
  }

  // Append this error message to the context.
  error_msgs()->append(msg);
  YB_LOG_EVERY_N_SECS(WARNING, 1) << "SQL Error: " << msg;
  return STATUS(QLError, msg.c_str(), Slice(), QLError(error_code_));
}

Status ProcessContextBase::Error(const YBLocation& loc,
                                 const std::string& msg,
                                 ErrorCode error_code,
                                 const char* token) {
  return Error(loc, msg.c_str(), error_code, token);
}

Status ProcessContextBase::Error(const YBLocation& loc, const std::string& msg, const char* token) {
  return Error(loc, msg, ErrorCode::SQL_STATEMENT_INVALID, token);
}

Status ProcessContextBase::Error(const YBLocation& loc, const char *msg, const char* token) {
  return Error(loc, msg, ErrorCode::SQL_STATEMENT_INVALID, token);
}

Status ProcessContextBase::Error(const YBLocation& loc,
                                 ErrorCode error_code,
                                 const char* token) {
  return Error(loc, "", error_code, token);
}

Status ProcessContextBase::Error(const TreeNode *tnode,
                                 const std::string& msg,
                                 ErrorCode error_code) {
  return Error(tnode->loc(), msg.c_str(), error_code);
}

Status ProcessContextBase::Error(const TreeNode *tnode,
                                 const char *msg,
                                 ErrorCode error_code) {
  return Error(tnode->loc(), msg, error_code);
}

Status ProcessContextBase::Error(const TreeNode *tnode,
                                 ErrorCode error_code) {
  return Error(tnode->loc(), error_code);
}

Status ProcessContextBase::Error(const TreeNode *tnode,
                                 const Status& s,
                                 ErrorCode error_code) {
  return Error(tnode->loc(), s.ToUserMessage().c_str(), error_code);
}

Status ProcessContextBase::Error(const TreeNode::SharedPtr& tnode,
                                 ErrorCode error_code) {
  return Error(tnode->loc(), error_code);
}

Status ProcessContextBase::Error(const TreeNode::SharedPtr& tnode,
                                 const std::string& msg,
                                 ErrorCode error_code) {
  return Error(tnode->loc(), msg.c_str(), error_code);
}

Status ProcessContextBase::Error(const TreeNode::SharedPtr& tnode,
                                 const char *msg,
                                 ErrorCode error_code) {
  return Error(tnode->loc(), msg, error_code);
}

Status ProcessContextBase::Error(const TreeNode::SharedPtr& tnode,
                                 const Status& s,
                                 ErrorCode error_code) {
  return Error(tnode->loc(), s.ToUserMessage().c_str(), error_code);
}

//--------------------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------------------
// ProcessContext
//--------------------------------------------------------------------------------------------------

ProcessContext::ProcessContext(ParseTree::UniPtr parse_tree)
    : parse_tree_(std::move(parse_tree)) {
}

ProcessContext::~ProcessContext() {
}

//--------------------------------------------------------------------------------------------------

void ProcessContext::SaveGeneratedParseTree(TreeNode::SharedPtr generated_parse_tree) {
  CHECK(parse_tree_.get() != nullptr) << "Context is not associated with a parse tree";
  parse_tree_->set_root(generated_parse_tree);
}

}  // namespace ql
}  // namespace yb
