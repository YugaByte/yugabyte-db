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
//--------------------------------------------------------------------------------------------------

#ifndef YB_YQL_PGGATE_PG_EXPR_H_
#define YB_YQL_PGGATE_PG_EXPR_H_

#include "yb/client/client.h"
#include "yb/yql/pggate/util/pg_doc_data.h"
#include "yb/yql/pggate/util/pg_tuple.h"
#include "yb/util/bfpg/tserver_opcodes.h"

namespace yb {
namespace pggate {

class PgDml;
class PgExpr {
 public:
  enum class Opcode {
    PG_EXPR_CONSTANT,
    PG_EXPR_COLREF,
    PG_EXPR_VARIABLE,

    // The logical expression for defining the conditions when we support WHERE clause.
    PG_EXPR_NOT,
    PG_EXPR_EQ,
    PG_EXPR_NE,
    PG_EXPR_GE,
    PG_EXPR_GT,
    PG_EXPR_LE,
    PG_EXPR_LT,

    // Aggregate functions.
    PG_EXPR_AVG,
    PG_EXPR_SUM,
    PG_EXPR_COUNT,
    PG_EXPR_MAX,
    PG_EXPR_MIN,

    // Serialized YSQL/PG Expr node.
    PG_EXPR_EVAL_EXPR_CALL,

    PG_EXPR_GENERATE_ROWID,
  };

  // Public types.
  typedef std::shared_ptr<PgExpr> SharedPtr;
  typedef std::shared_ptr<const PgExpr> SharedPtrConst;

  typedef std::unique_ptr<PgExpr> UniPtr;
  typedef std::unique_ptr<const PgExpr> UniPtrConst;

  // Constructor.
  explicit PgExpr(Opcode opcode, const YBCPgTypeEntity *type_entity);
  explicit PgExpr(Opcode opcode, const YBCPgTypeEntity *type_entity, const PgTypeAttrs *type_attrs);
  explicit PgExpr(const char *opname, const YBCPgTypeEntity *type_entity);
  virtual ~PgExpr();

  // Prepare expression when constructing a statement.
  virtual CHECKED_STATUS PrepareForRead(PgDml *pg_stmt, PgsqlExpressionPB *expr_pb);

  // Convert this expression structure to PB format.
  virtual CHECKED_STATUS Eval(PgDml *pg_stmt, PgsqlExpressionPB *expr_pb);
  virtual CHECKED_STATUS Eval(PgDml *pg_stmt, QLValuePB *result);
  virtual CHECKED_STATUS Eval(QLValuePB *result);

  // Access methods.
  Opcode opcode() const {
    return opcode_;
  }
  bool is_constant() const {
    return opcode_ == Opcode::PG_EXPR_CONSTANT;
  }
  bool is_colref() const {
    return opcode_ == Opcode::PG_EXPR_COLREF;
  }
  bool is_aggregate() const {
    // Only return true for pushdown supported aggregates.
    return (opcode_ == Opcode::PG_EXPR_SUM ||
            opcode_ == Opcode::PG_EXPR_COUNT ||
            opcode_ == Opcode::PG_EXPR_MAX ||
            opcode_ == Opcode::PG_EXPR_MIN);
  }
  virtual bool is_ybbasetid() const {
    return false;
  }

  // Read the result from input buffer (yb_cursor) that was computed by and sent from DocDB.
  // Write the result to output buffer (pg_cursor) in Postgres format.
  CHECKED_STATUS ResultToPg(Slice *yb_cursor, Slice *pg_cursor);

  // Function translate_data_() reads the received data from DocDB and writes it to Postgres buffer
  // using to_datum().
  // - DocDB supports a number of datatypes, and we would need to provide one translate function for
  //   each datatype to read them correctly. The translate_data() function pointer must be setup
  //   correctly during the compilation of a statement.
  // - For each postgres data type, to_datum() function pointer must be setup properly during
  //   the compilation of a statement.
  void TranslateData(Slice *yb_cursor, const PgWireDataHeader& header, int index,
                     PgTuple *pg_tuple) const {
    CHECK(translate_data_) << "Data format translation is not provided";
    translate_data_(yb_cursor, header, index, type_entity_, &type_attrs_, pg_tuple);
  }

  // Implementation for "translate_data()" for each supported datatype.
  // Translates DocDB-numeric datatypes.
  template<typename data_type>
  static void TranslateNumber(Slice *yb_cursor, const PgWireDataHeader& header, int index,
                              const YBCPgTypeEntity *type_entity, const PgTypeAttrs *type_attrs,
                              PgTuple *pg_tuple) {
    if (header.is_null()) {
      return pg_tuple->WriteNull(index, header);
    }
    DCHECK(type_entity) << "Type entity not provided";
    DCHECK(type_entity->yb_to_datum) << "Type entity converter not provided";

    data_type result = 0;
    size_t read_size = PgDocData::ReadNumber(yb_cursor, &result);
    yb_cursor->remove_prefix(read_size);
    pg_tuple->WriteDatum(index, type_entity->yb_to_datum(&result, read_size, type_attrs));
  }

  // Translates DocDB-char-based datatypes.
  static void TranslateText(Slice *yb_cursor, const PgWireDataHeader& header, int index,
                            const YBCPgTypeEntity *type_entity, const PgTypeAttrs *type_attrs,
                            PgTuple *pg_tuple);

  // Translates DocDB-binary-based datatypes.
  static void TranslateBinary(Slice *yb_cursor, const PgWireDataHeader& header, int index,
                              const YBCPgTypeEntity *type_entity, const PgTypeAttrs *type_attrs,
                              PgTuple *pg_tuple);

  // Translate DocDB-decimal datatype.
  static void TranslateDecimal(Slice *yb_cursor, const PgWireDataHeader& header, int index,
                               const YBCPgTypeEntity *type_entity, const PgTypeAttrs *type_attrs,
                               PgTuple *pg_tuple);

  // Translate system column.
  template<typename data_type>
  static void TranslateSysCol(Slice *yb_cursor, const PgWireDataHeader& header, data_type *value) {
    *value = 0;
    if (header.is_null()) {
      // 0 is an invalid OID.
      return;
    }
    size_t read_size = PgDocData::ReadNumber(yb_cursor, value);
    yb_cursor->remove_prefix(read_size);
  }

  static void TranslateSysCol(Slice *yb_cursor, const PgWireDataHeader& header,
                              PgTuple *pg_tuple, uint8_t **value);
  static void TranslateCtid(Slice *yb_cursor, const PgWireDataHeader& header, int index,
                            const YBCPgTypeEntity *type_entity, const PgTypeAttrs *type_attrs,
                            PgTuple *pg_tuple);
  static void TranslateOid(Slice *yb_cursor, const PgWireDataHeader& header, int index,
                           const YBCPgTypeEntity *type_entity, const PgTypeAttrs *type_attrs,
                           PgTuple *pg_tuple);
  static void TranslateXmin(Slice *yb_cursor, const PgWireDataHeader& header, int index,
                            const YBCPgTypeEntity *type_entity, const PgTypeAttrs *type_attrs,
                            PgTuple *pg_tuple);
  static void TranslateCmin(Slice *yb_cursor, const PgWireDataHeader& header, int index,
                            const YBCPgTypeEntity *type_entity, const PgTypeAttrs *type_attrs,
                            PgTuple *pg_tuple);
  static void TranslateXmax(Slice *yb_cursor, const PgWireDataHeader& header, int index,
                            const YBCPgTypeEntity *type_entity, const PgTypeAttrs *type_attrs,
                            PgTuple *pg_tuple);
  static void TranslateCmax(Slice *yb_cursor, const PgWireDataHeader& header, int index,
                            const YBCPgTypeEntity *type_entity, const PgTypeAttrs *type_attrs,
                            PgTuple *pg_tuple);
  static void TranslateTableoid(Slice *yb_cursor, const PgWireDataHeader& header, int index,
                                const YBCPgTypeEntity *type_entity, const PgTypeAttrs *type_attrs,
                                PgTuple *pg_tuple);
  static void TranslateYBCtid(Slice *yb_cursor, const PgWireDataHeader& header, int index,
                              const YBCPgTypeEntity *type_entity, const PgTypeAttrs *type_attrs,
                              PgTuple *pg_tuple);
  static void TranslateYBBasectid(Slice *yb_cursor, const PgWireDataHeader& header, int index,
                                  const YBCPgTypeEntity *type_entity, const PgTypeAttrs *type_attrs,
                                  PgTuple *pg_tuple);

  // Get expression type.
  InternalType internal_type() const;

  // Find opcode.
  static CHECKED_STATUS CheckOperatorName(const char *name);
  static Opcode NameToOpcode(const char *name);
  static bfpg::TSOpcode PGOpcodeToTSOpcode(const PgExpr::Opcode opcode);
  static bfpg::TSOpcode OperandTypeToSumTSOpcode(InternalType type);

 protected:
  void InitializeTranslateData();

  // Data members.
  Opcode opcode_;
  const PgTypeEntity *type_entity_;
  const PgTypeAttrs type_attrs_;
  std::function<void(Slice *, const PgWireDataHeader&, int, const YBCPgTypeEntity *,
                     const PgTypeAttrs *, PgTuple *)> translate_data_;
};

class PgConstant : public PgExpr {
 public:
  // Public types.
  typedef std::shared_ptr<PgConstant> SharedPtr;
  typedef std::shared_ptr<const PgConstant> SharedPtrConst;

  typedef std::unique_ptr<PgConstant> UniPtr;
  typedef std::unique_ptr<const PgConstant> UniPtrConst;

  // Constructor.
  explicit PgConstant(const YBCPgTypeEntity *type_entity, uint64_t datum, bool is_null,
      PgExpr::Opcode opcode = PgExpr::Opcode::PG_EXPR_CONSTANT);
  explicit PgConstant(const YBCPgTypeEntity *type_entity, PgDatumKind datum_kind,
      PgExpr::Opcode opcode = PgExpr::Opcode::PG_EXPR_CONSTANT);

  // Destructor.
  virtual ~PgConstant();

  // Update numeric.
  void UpdateConstant(int8_t value, bool is_null);
  void UpdateConstant(int16_t value, bool is_null);
  void UpdateConstant(int32_t value, bool is_null);
  void UpdateConstant(int64_t value, bool is_null);
  void UpdateConstant(float value, bool is_null);
  void UpdateConstant(double value, bool is_null);

  // Update text.
  void UpdateConstant(const char *value, bool is_null);
  void UpdateConstant(const void *value, size_t bytes, bool is_null);

  // Expression to PB.
  CHECKED_STATUS Eval(PgDml *pg_stmt, PgsqlExpressionPB *expr_pb) override;
  CHECKED_STATUS Eval(PgDml *pg_stmt, QLValuePB *result) override;
  CHECKED_STATUS Eval(QLValuePB *result) override;

  // Read binary value.
  const string &binary_value() {
    return ql_value_.binary_value();
  }

 private:
  QLValuePB ql_value_;
};

class PgColumnRef : public PgExpr {
 public:
  // Public types.
  typedef std::shared_ptr<PgColumnRef> SharedPtr;
  typedef std::shared_ptr<const PgColumnRef> SharedPtrConst;

  typedef std::unique_ptr<PgColumnRef> UniPtr;
  typedef std::unique_ptr<const PgColumnRef> UniPtrConst;

  explicit PgColumnRef(int attr_num,
                       const PgTypeEntity *type_entity,
                       const PgTypeAttrs *type_attrs);
  virtual ~PgColumnRef();

  // Setup ColumnRef expression when constructing statement.
  CHECKED_STATUS PrepareForRead(PgDml *pg_stmt, PgsqlExpressionPB *expr_pb) override;

  int attr_num() const {
    return attr_num_;
  }

  bool is_ybbasetid() const override;

 private:
  int attr_num_;
};

class PgOperator : public PgExpr {
 public:
  // Public types.
  typedef std::shared_ptr<PgOperator> SharedPtr;
  typedef std::shared_ptr<const PgOperator> SharedPtrConst;

  typedef std::unique_ptr<PgOperator> UniPtr;
  typedef std::unique_ptr<const PgOperator> UniPtrConst;

  // Constructor.
  explicit PgOperator(const char *name, const YBCPgTypeEntity *type_entity);
  virtual ~PgOperator();

  // Append arguments.
  void AppendArg(PgExpr *arg);

  // Setup operator expression when constructing statement.
  virtual CHECKED_STATUS PrepareForRead(PgDml *pg_stmt, PgsqlExpressionPB *expr_pb);

 private:
  const string opname_;
  std::vector<PgExpr*> args_;
};

}  // namespace pggate
}  // namespace yb

#endif // YB_YQL_PGGATE_PG_EXPR_H_
