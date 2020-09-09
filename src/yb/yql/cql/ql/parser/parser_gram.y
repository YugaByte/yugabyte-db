%{
//--------------------------------------------------------------------------------------------------
// Portions Copyright (c) YugaByte, Inc.
// Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
// Portions Copyright (c) 1994, Regents of the University of California
//
// POSTGRESQL BISON rules/actions
//
// NOTES
//    CAPITALS are used to represent terminal symbols.
//    non-capitals are used to represent non-terminals.
//
//    In general, nothing in this file should initiate database accesses
//    nor depend on changeable state (such as SET variables).  If you do
//    database accesses, your code will fail when we have aborted the
//    current transaction and are just parsing commands to find the next
//    ROLLBACK or COMMIT.  If you make use of SET variables, then you
//    will do the wrong thing in multi-query strings like this:
//      SET SQL_inheritance TO off; SELECT * FROM foo;
//    because the entire string is parsed by gram.y before the SET gets
//    executed.  Anything that depends on the database or changeable state
//    should be handled during parse analysis so that it happens at the
//    right time not the wrong time.  The handling of SQL_inheritance is
//    a good example.
//--------------------------------------------------------------------------------------------------
%}

//--------------------------------------------------------------------------------------------------
// NOTE: All parsing rules are copies of PostgreQL's code. Currently, we left all processing code
// out. After parse tree and nodes are defined, they should be created while processing the rules.
//--------------------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------------------
// BISON options.
// The parsing context.
%skeleton "lalr1.cc" /* -*- C++ -*- */
%require "3.0.2"

// Expect 0 shift/reduce conflicts.
%expect 0

// Debugging options. These should be deleted after coding is completed.
%define parse.trace
%define parse.error verbose
// Because of a bug in BISON 3.2, we have to turn off assertion for now.
// %define parse.assert

// BISON options.
%defines                                                                  // Generate header files.

// Define class for YACC.
%define api.namespace {yb::ql}
%define parser_class_name {GramProcessor}

// First parameter for yylex and yyparse.
%parse-param { Parser *parser_ }

// Define token.
%define api.value.type variant
%define api.token.constructor
%define api.token.prefix {TOK_}

//--------------------------------------------------------------------------------------------------
// The following code block goes into YACC generated *.hh header file.
%code requires {
#include <stdbool.h>

#include "yb/common/common.pb.h"
#include "yb/gutil/macros.h"

#include "yb/yql/cql/ql/ptree/parse_tree.h"
#include "yb/yql/cql/ql/ptree/tree_node.h"
#include "yb/yql/cql/ql/ptree/list_node.h"
#include "yb/yql/cql/ql/parser/parser_inactive_nodes.h"
#include "yb/yql/cql/ql/ptree/pt_create_keyspace.h"
#include "yb/yql/cql/ql/ptree/pt_use_keyspace.h"
#include "yb/yql/cql/ql/ptree/pt_alter_keyspace.h"
#include "yb/yql/cql/ql/ptree/pt_alter_table.h"
#include "yb/yql/cql/ql/ptree/pt_create_table.h"
#include "yb/yql/cql/ql/ptree/pt_create_type.h"
#include "yb/yql/cql/ql/ptree/pt_create_index.h"
#include "yb/yql/cql/ql/ptree/pt_create_role.h"
#include "yb/yql/cql/ql/ptree/pt_alter_role.h"
#include "yb/yql/cql/ql/ptree/pt_grant_revoke.h"
#include "yb/yql/cql/ql/ptree/pt_truncate.h"
#include "yb/yql/cql/ql/ptree/pt_drop.h"
#include "yb/yql/cql/ql/ptree/pt_type.h"
#include "yb/yql/cql/ql/ptree/pt_name.h"
#include "yb/yql/cql/ql/ptree/pt_expr.h"
#include "yb/yql/cql/ql/ptree/pt_bcall.h"
#include "yb/yql/cql/ql/ptree/pt_select.h"
#include "yb/yql/cql/ql/ptree/pt_insert.h"
#include "yb/yql/cql/ql/ptree/pt_insert_json_clause.h"
#include "yb/yql/cql/ql/ptree/pt_insert_values_clause.h"
#include "yb/yql/cql/ql/ptree/pt_explain.h"
#include "yb/yql/cql/ql/ptree/pt_delete.h"
#include "yb/yql/cql/ql/ptree/pt_update.h"
#include "yb/yql/cql/ql/ptree/pt_transaction.h"

DECLARE_bool(cql_raise_index_where_clause_error);

namespace yb {
namespace ql {

class Parser;

// Parsing types.
// Use shared_ptr here because YACC does not know how to use move() instead of "=" for pointers.
typedef int64_t                        PInt64;
typedef bool                           PBool;
typedef char                           PChar;
typedef const char                    *KeywordType;
typedef MCSharedPtr<MCString>          PString;

typedef TreeNode::SharedPtr            PTreeNode;
typedef PTListNode::SharedPtr          PListNode;
typedef PTExpr::SharedPtr              PExpr;
typedef PTRoleOption::SharedPtr        PRoleOption;
typedef PTRef::SharedPtr               PRef;
typedef PTExprListNode::SharedPtr      PExprListNode;
typedef PTRoleOptionListNode::SharedPtr     PRoleOptionListNode;
typedef PTConstInt::SharedPtr          PConstInt;
typedef PTCollectionExpr::SharedPtr    PCollectionExpr;
typedef PTCollection::SharedPtr        PCollection;
typedef PTInsertValuesClause::SharedPtr     PInsertValuesClause;
typedef PTInsertJsonClause::SharedPtr       PInsertJsonClause;
typedef PTSelectStmt::SharedPtr        PSelectStmt;
typedef PTTableRef::SharedPtr          PTableRef;
typedef PTTableRefListNode::SharedPtr  PTableRefListNode;
typedef PTOrderBy::SharedPtr           POrderBy;
typedef PTOrderByListNode::SharedPtr   POrderByListNode;
typedef PTAssign::SharedPtr            PAssign;
typedef PTKeyspaceProperty::SharedPtr     PKeyspaceProperty;
typedef PTKeyspacePropertyListNode::SharedPtr     PKeyspacePropertyListNode;
typedef PTKeyspacePropertyMap::SharedPtr  PKeyspacePropertyMap;
typedef PTDmlUsingClause::SharedPtr     PDmlUsingClause;
typedef PTDmlUsingClauseElement::SharedPtr     PDmlUsingClauseElement;
typedef PTTableProperty::SharedPtr     PTableProperty;
typedef PTTablePropertyListNode::SharedPtr     PTablePropertyListNode;
typedef PTTablePropertyMap::SharedPtr  PTablePropertyMap;

typedef PTTypeField::SharedPtr         PTypeField;
typedef PTTypeFieldListNode::SharedPtr PTypeFieldListNode;

typedef PTAssignListNode::SharedPtr    PAssignListNode;

typedef PTName::SharedPtr              PName;
typedef PTIndexColumn::SharedPtr       PIndexColumn;
typedef PTQualifiedName::SharedPtr     PQualifiedName;
typedef PTQualifiedNameListNode::SharedPtr PQualifiedNameListNode;

typedef PTBaseType::SharedPtr          PType;
typedef PTCharBaseType::SharedPtr      PCharBaseType;
typedef MCSharedPtr<MCVector<PExpr>>   PExprVector;

// Inactive parsing node types.
typedef UndefTreeNode::SharedPtr       UndefType;
typedef void                          *UndefListType;
typedef JoinType                       jtype;
typedef DropBehavior                   dbehavior;
typedef ObjectType                     objtype;
typedef FunctionParameterMode          fun_param_mode;

} // namespace ql.
} // namespace yb.

}

//--------------------------------------------------------------------------------------------------
// The code in the rest of this file go into YACC generated *.cc source file.
%code {
#include <stdio.h>
#include <string.h>

#include <gflags/gflags.h>

#include "yb/yql/cql/ql/parser/parser.h"
#include "yb/yql/cql/ql/parser/scanner_util.h"
#include "yb/util/stol_utils.h"

using namespace std;
using namespace yb::ql;

#undef yylex
#define yylex parser_->Scan

#define PTREE_MEM parser_->PTreeMem()
#define PTREE_LOC(loc) Location::MakeShared(PTREE_MEM, loc)
#define MAKE_NODE(loc, node, ...) node::MakeShared(PTREE_MEM, PTREE_LOC(loc), ##__VA_ARGS__)
// This allows nested MAKE_NODE macros since it doesn't have ##__VA_ARGS__ and requires atleast
// one variable argument.
#define NESTED_MAKE_NODE(loc, node, ...)  MAKE_NODE(loc, node, __VA_ARGS__)

#define PARSER_ERROR(loc, code) parser_->Error(loc, ErrorCode:##:code)
#define PARSER_ERROR_MSG(loc, code, msg) parser_->Error(loc, msg, ErrorCode:##:code)

#define PARSER_INVALID(loc) parser_->Error(loc, ErrorCode::SQL_STATEMENT_INVALID)
#define PARSER_UNSUPPORTED(loc) parser_->Error(loc, ErrorCode::FEATURE_NOT_SUPPORTED)
#define PARSER_NOCODE(loc) parser_->Error(loc, ErrorCode::FEATURE_NOT_YET_IMPLEMENTED)

#define PARSER_CQL_INVALID(loc) parser_->Error(loc, ErrorCode::CQL_STATEMENT_INVALID)
#define PARSER_CQL_INVALID_MSG(loc, msg) parser_->Error(loc, msg, ErrorCode::CQL_STATEMENT_INVALID)
}

//--------------------------------------------------------------------------------------------------
// Active tree node declarations (%type).
// The rule names are currently used by QL.
%type <PListNode>         // Statement lists.
                          stmtblock stmtmulti dml_list

%type <PTreeNode>         // Statement as tree node.
                          stmt

                          // Create table.
                          CreateStmt schema_stmt TableElement TableConstraint
                          columnDef columnElem ColConstraint ColConstraintElem
                          ConstraintElem ConstraintAttr

                          // Create role.
                          CreateRoleStmt

                          // Alter role.
                          AlterRoleStmt

                          // Truncate table.
                          TruncateStmt

                          // Drop.
                          DropStmt

                          // Grant.
                          GrantStmt GrantRoleStmt

                          // Revoke.
                          RevokeStmt RevokeRoleStmt

                          // Select.
                          distinct_clause opt_all_clause
                          group_by_item for_locking_clause opt_for_locking_clause
                          opt_window_clause

                          // Insert.
                          InsertStmt returning_clause
                          opt_on_conflict opt_conf_expr

                          // Delete.
                          DeleteStmt

                          // Update.
                          UpdateStmt
                          set_target_list

                          // Insert/update/delete DML.
                          dml

                          // Start transaction / commit.
                          TransactionStmt

                          // Alter table.
                          AlterTableStmt
                          addColumnDef dropColumn alterProperty
                          renameColumn alterColumnType

                          // Keyspace related statements.
                          CreateSchemaStmt UseSchemaStmt AlterSchemaStmt

                          // User-defined types.
                          CreateTypeStmt

                          // Index.
                          IndexStmt

                          //Explain
                          ExplainStmt ExplainableStmt

%type <PCollection>       // Select can be either statement or expression of collection types.
                          SelectStmt select_no_parens select_with_parens select_clause

%type <PSelectStmt>       simple_select

%type <PInsertValuesClause>     values_clause

%type <PInsertJsonClause>       json_clause

%type <PExpr>             // Expression clause. These expressions are used in a specific context.
                          if_clause opt_if_clause
                          where_clause opt_where_clause
                          where_or_current_clause opt_where_or_current_clause
                          limit_clause select_limit_value
                          offset_clause select_offset_value

%type <PListNode>         // Clauses as list of tree nodes.
                          group_clause group_by_list having_clause into_clause

                          // Create table clauses.
                          OptTableElementList TableElementList
                          ColQualList NestedColumnList columnList index_column_list

                          // Alter table commands.
                          addColumnDefList dropColumnList alterPropertyList
                          renameColumnList alterColumnTypeList
                          alter_table_op alter_table_ops

                          // Create index clauses.
                          index_params opt_include_clause

%type <PExpr>             // Expressions.
                          a_expr b_expr ctext_expr c_expr AexprConst bindvar
                          collection_expr target_el in_expr
                          func_expr func_application func_arg_expr
                          inactive_a_expr inactive_c_expr

%type <PRoleOption>       RoleOption

%type <PRef>              columnref

%type <PCollectionExpr>   // An expression for CQL collections:
                          //  - Map/Set/List/Tuple/Frozen/User-Defined Types.
                          map_elems map_expr set_elems set_expr list_elems list_expr
                          tuple_elems tuple_expr

%type <PExprListNode>     // A list of expressions.
                          target_list opt_target_list
                          ctext_row ctext_expr_list func_arg_list col_arg_list json_ref
                          json_ref_single_arrow

%type <PExprVector>       // A vector of expressions.
                          opt_select_limit_offset select_limit_offset

%type <PRoleOptionListNode>   RoleOptionList optRoleOptionList

%type <PDmlUsingClause>   // Using clause for DML statements.
                          opt_using_ttl_timestamp_clause using_ttl_timestamp_clause
                          recursive_ttl_timestamp_clause

%type <PDmlUsingClauseElement>   // Using clause element for DML statements.
                                 ttl_timestamp_clause

%type <PType>             // Datatype nodes.
                          Typename SimpleTypename ParametricTypename Numeric

%type <PTableRef>         // Name node that represents table.
                          table_ref relation_expr_opt_alias

%type <POrderByListNode>  // Sortby List for orderby clause
                          sortby_list opt_sort_clause sort_clause

%type <POrderBy>          // Sortby for orderby clause
                          sortby

%type <PTableRefListNode> // List of names that represent tables.
                          from_clause from_list

%type <PCharBaseType>     character Character ConstCharacter
                          CharacterWithLength CharacterWithoutLength

%type <PAssign>           set_clause single_set_clause
%type <PAssignListNode>   set_clause_list

%type <objtype>           drop_type cql_drop_type ql_drop_type

%type <PKeyspaceProperty>    keyspace_property_map_list_element
%type <PKeyspacePropertyMap> keyspace_property_map_list keyspace_property_map
%type <PKeyspacePropertyListNode> opt_keyspace_options keyspace_property keyspace_properties

%type <PTableProperty>    column_ordering property_map_list_element
%type <PTablePropertyMap> property_map_list property_map
%type <PTablePropertyListNode>   opt_table_options table_property table_properties orderingList
                          opt_index_options

%type <PTypeField>         TypeField
%type <PTypeFieldListNode> TypeFieldList

// Name nodes.
%type <PName>             indirection_el
%type <PIndexColumn>      index_column

%type <PQualifiedNameListNode>  insert_column_list any_name_list relation_expr_list

%type <PQualifiedName>    qualified_name indirection relation_expr
                          insert_target insert_column_item opt_indirection
                          set_target any_name attrs opt_opfamily opclass_purpose
                          opt_collate opt_class udt_name

%type <PString>           // Identifier name.
                          ColId role_name permission permissions
                          alias_clause opt_alias_clause

// Precision for datatype FLOAT in declarations for columns or any other entities.
%type <PInt64>            opt_float

%type <KeywordType>       unreserved_keyword type_func_name_keyword
%type <KeywordType>       col_name_keyword reserved_keyword

%type <PBool>             boolean opt_else_clause opt_returns_clause opt_json_clause_default_null

//--------------------------------------------------------------------------------------------------
// Inactive tree node declarations (%type).
// The rule names are currently not used.

%type <KeywordType>       iso_level opt_encoding opt_boolean_or_string row_security_cmd
                          RowSecurityDefaultForCmd all_Op MathOp extract_arg

%type <PChar>             enable_trigger

%type <PString>           createdb_opt_name RoleId opt_type foreign_server_version
                          opt_foreign_server_version OptSchemaName copy_file_name
                          database_name access_method_clause access_method attr_name name
                          cursor_name file_name index_name opt_index_name
                          cluster_index_specification generic_option_name opt_charset
                          Sconst comment_text notify_payload ColLabel var_name func_name
                          type_function_name param_name NonReservedWord NonReservedWord_or_Sconst
                          ExistingIndex OptTableSpace OptConsTableSpace opt_provider
                          security_label opt_existing_window_name property_name

%type <PBool>             opt_if_not_exists xml_whitespace_option constraints_set_mode opt_varying
                          opt_timezone opt_no_inherit opt_ordinality opt_instead opt_unique
                          opt_concurrently opt_verbose opt_full opt_freeze opt_default opt_recheck
                          copy_from opt_program all_or_distinct opt_trusted opt_restart_seqs
                          opt_or_replace opt_grant_grant_option /* opt_grant_admin_option */
                          opt_nowait opt_if_exists opt_with_data opt_allow_filtering
                          TriggerForSpec TriggerForType

%type <PInt64>            TableLikeOptionList TableLikeOption key_actions key_delete key_match
                          key_update key_action ConstraintAttributeSpec ConstraintAttributeElem
                          opt_check_option document_or_content reindex_target_type
                          reindex_target_multitable reindex_option_list reindex_option_elem Iconst
                          SignedIconst sub_type opt_column event cursor_options opt_hold
                          opt_set_data row_or_rows first_or_next OptTemp OptNoLog
                          for_locking_strength defacl_privilege_target import_qualification_type
                          opt_lock lock_type cast_context vacuum_option_list vacuum_option_elem
                          opt_nowait_or_skip TriggerActionTime add_drop opt_asc_desc opt_nulls_order

%type <PType>             ConstTypename ConstDatetime ConstInterval
                          Bit ConstBit BitWithLength BitWithoutLength
                          UserDefinedType

%type <jtype>             join_type
%type <dbehavior>         opt_drop_behavior
%type <objtype>           comment_type security_label_type
%type <fun_param_mode>    arg_class
// This should be of type <oncommit>.
%type <PInt64>            OnCommitOption

%type <UndefType>         inactive_stmt inactive_schema_stmt
                          AlterEventTrigStmt AlterDatabaseStmt AlterDatabaseSetStmt AlterDomainStmt
                          AlterEnumStmt AlterFdwStmt AlterForeignServerStmt AlterObjectSchemaStmt
                          AlterOwnerStmt AlterSeqStmt AlterSystemStmt InactiveAlterTableStmt
                          AlterTblSpcStmt AlterExtensionStmt AlterExtensionContentsStmt
                          AlterForeignTableStmt AlterCompositeTypeStmt AlterUserStmt
                          AlterUserSetStmt
                          AlterPolicyStmt AlterDefaultPrivilegesStmt
                          DefACLAction AnalyzeStmt ClosePortalStmt ClusterStmt CommentStmt
                          ConstraintsSetStmt CopyStmt CreateAsStmt CreateCastStmt
                          CreateDomainStmt CreateExtensionStmt CreateOpClassStmt
                          CreateOpFamilyStmt AlterOpFamilyStmt CreatePLangStmt
                          CreateSeqStmt CreateTableSpaceStmt
                          CreateFdwStmt CreateForeignServerStmt CreateForeignTableStmt
                          CreateAssertStmt CreateTransformStmt CreateTrigStmt CreateEventTrigStmt
                          CreateUserStmt CreatePolicyStmt
                          CreatedbStmt DeclareCursorStmt DefineStmt DiscardStmt DoStmt
                          DropOpClassStmt DropOpFamilyStmt DropPLangStmt
                          DropAssertStmt DropTrigStmt DropRuleStmt DropCastStmt
                          DropPolicyStmt DropUserStmt DropdbStmt DropTableSpaceStmt DropFdwStmt
                          DropTransformStmt
                          DropForeignServerStmt FetchStmt
                          ImportForeignSchemaStmt
                          ListenStmt LoadStmt LockStmt NotifyStmt PreparableStmt
                          CreateFunctionStmt AlterFunctionStmt ReindexStmt RemoveAggrStmt
                          RemoveFuncStmt RemoveOperStmt RenameStmt
                          RuleActionStmt RuleActionStmtOrEmpty RuleStmt
                          SecLabelStmt
                          UnlistenStmt VacuumStmt
                          VariableResetStmt VariableSetStmt VariableShowStmt
                          ViewStmt CheckPointStmt CreateConversionStmt
                          DeallocateStmt PrepareStmt ExecuteStmt
                          DropOwnedStmt ReassignOwnedStmt
                          AlterTSConfigurationStmt AlterTSDictionaryStmt
                          CreateMatViewStmt RefreshMatViewStmt
                          alter_column_default opclass_item opclass_drop alter_using
                          alter_table_cmd alter_type_cmd opt_collate_clause replica_identity
                          createdb_opt_item copy_opt_item transaction_mode_item
                          create_extension_opt_item alter_extension_opt_item
                          CreateOptRoleElem AlterOptRoleElem
                          TriggerFuncArg
                          TriggerWhen
                          event_trigger_when_item
                          OptConstrFromTable
                          RowSecurityOptionalWithCheck RowSecurityOptionalExpr
                          grantee
                          privilege
                          privilege_target
                          function_with_argtypes
                          DefACLOption
                          import_qualification
                          empty_grouping_set rollup_clause cube_clause
                          grouping_sets_clause
                          fdw_option
                          OptTempTableName
                          create_as_target create_mv_target
                          createfunc_opt_item common_func_opt_item dostmt_opt_item
                          func_arg func_arg_with_default table_func_column aggr_arg
                          func_return func_type
                          for_locking_item
                          join_outer join_qual
                          overlay_placing substr_from substr_for
                          opt_binary opt_oids copy_delimiter
                          fetch_args
                          opt_select_fetch_first_value
                          SeqOptElem
                          generic_set set_rest set_rest_more generic_reset reset_rest
                          SetResetClause FunctionSetResetClause
                          TypedTableElement TableFuncElement
                          columnOptions
                          def_elem reloption_elem old_aggr_elem
                          def_arg
                          func_table
                          ExclusionWhereClause
                          NumericOnly
                          index_elem
                          joined_table
                          tablesample_clause opt_repeatable_clause
                          generic_option_arg
                          generic_option_elem alter_generic_option_elem
                          copy_generic_opt_arg copy_generic_opt_arg_list_item
                          copy_generic_opt_elem
                          var_value zone_value
                          RoleSpec opt_granted_by
                          TableLikeClause
                          OptTableSpaceOwner
                          xml_attribute_el
                          xml_root_version opt_xml_root_standalone
                          xmlexists_argument
                          func_expr_common_subexpr
                          func_expr_windowless
                          filter_clause
                          window_definition over_clause window_specification
                          opt_frame_clause frame_extent frame_bound

%type <UndefListType>     alter_table_cmds alter_type_cmds createdb_opt_list createdb_opt_items
                          copy_opt_list transaction_mode_list create_extension_opt_list
                          alter_extension_opt_list OptRoleList AlterOptRoleList OptSchemaEltList
                          TriggerEvents TriggerOneEvent event_trigger_when_list
                          event_trigger_value_list handler_name qual_Op qual_all_Op
                          subquery_Op opt_inline_handler opt_validator validator_clause
                          RowSecurityDefaultToRole RowSecurityOptionalToRole
                          grantee_list privileges privilege_list function_with_argtypes_list
                          OptInherit definition
                          OptTypedTableElementList TypedTableElementList reloptions opt_reloptions
                          opt_definition func_args
                          func_args_list func_args_with_defaults func_args_with_defaults_list
                          aggr_args aggr_args_list func_as createfunc_opt_list alterfunc_opt_list
                          old_aggr_definition old_aggr_list oper_argtypes RuleActionList
                          RuleActionMulti opt_column_list opt_name_list
                          name_list role_list
                          opt_array_bounds qualified_name_list
                          type_name_list any_operator expr_list
                          multiple_set_clause def_list
                          reloption_list TriggerFuncArgs
                          opclass_item_list opclass_drop_list
                          transaction_mode_list_or_empty
                          TableFuncElementList opt_type_modifiers prep_type_clause
                          execute_param_clause opt_enum_val_list enum_val_list
                          table_func_column_list create_generic_options
                          alter_generic_options dostmt_opt_list
                          transform_element_list transform_type_list opt_fdw_options
                          fdw_options for_locking_items
                          locked_rels_list extract_list overlay_list position_list substr_list
                          trim_list opt_interval interval_second OptSeqOptList SeqOptList
                          rowsfrom_item rowsfrom_list opt_col_def_list ExclusionConstraintList
                          ExclusionConstraintElem row explicit_row implicit_row
                          type_list NumericOnly_list
                          func_alias_clause generic_option_list alter_generic_option_list
                          copy_generic_opt_list copy_generic_opt_arg_list
                          copy_options constraints_set_list xml_attribute_list
                          xml_attributes within_group_clause
                          window_definition_list opt_partition_clause DefACLOptionList var_list

//--------------------------------------------------------------------------------------------------
// Token definitions (%token).
// Keyword: If you want to make any keyword changes, update the keyword table in
//   src/include/parser/kwlist.h
// and add new keywords to the appropriate one of the reserved-or-not-so-reserved keyword lists.
// Search for "Keyword category lists".

// Declarations for ordinary keywords in alphabetical order.
%token <KeywordType>      ABORT_P ABSOLUTE_P ACCESS ACTION ADD_P ADMIN AFTER  AGGREGATE ALL ALLOW
                          ALSO ALTER ALWAYS ANALYSE ANALYZE AND ANY ARRAY AS ASC ASSERTION
                          ASSIGNMENT ASYMMETRIC AT ATTRIBUTE AUTHORIZATION AUTHORIZE

                          BACKWARD BEFORE BEGIN_P BETWEEN BIGINT BINARY BIT BLOB BOOLEAN_P BOTH BY

                          CACHE CALLED CASCADE CASCADED CASE CAST CATALOG_P CHAIN CHAR_P
                          CHARACTER CHARACTERISTICS CHECK CHECKPOINT CLASS CLOSE CLUSTER CLUSTERING
                          COALESCE COLLATE COLLATION COLUMN COMMENT COMMENTS COMMIT COMMITTED
                          COMPACT CONCURRENTLY CONFIGURATION CONFLICT CONNECTION CONSTRAINT
                          CONSTRAINTS CONTENT_P CONTINUE_P CONVERSION_P COPY COST COUNTER COVERING
                          CREATE CROSS CSV CUBE CURRENT_P CURRENT_CATALOG CURRENT_DATE CURRENT_ROLE
                          CURRENT_SCHEMA CURRENT_TIME CURRENT_TIMESTAMP CURRENT_USER CURSOR CYCLE

                          DATA_P DATE DATABASE DAY_P DEALLOCATE DEC DECIMAL_P DECLARE DEFAULT
                          DEFAULTS DEFERRABLE DEFERRED DEFINER DELETE_P
                          DELIMITER DELIMITERS DESC DESCRIBE DICTIONARY DISABLE_P DISCARD
                          DISTINCT DO DOCUMENT_P DOMAIN_P DOUBLE_P DROP

                          EACH ELSE ENABLE_P ENCODING ENCRYPTED END_P ENUM_P ERROR ESCAPE EVENT
                          EXCEPT EXCLUDE EXCLUDING EXCLUSIVE EXECUTE EXISTS EXPLAIN EXTENSION
                          EXTERNAL EXTRACT

                          FALSE_P FAMILY FETCH FILTER FILTERING FIRST_P FLOAT_P FOLLOWING FOR FORCE
                          FOREIGN FORWARD FREEZE FROM FROZEN FULL FUNCTION FUNCTIONS

                          GLOBAL GRANT GRANTED GREATEST GROUP_P GROUPING

                          HANDLER HAVING HEADER_P HOLD HOUR_P

                          IDENTITY_P IF_P ILIKE IMMEDIATE IMMUTABLE IMPLICIT_P IMPORT_P IN_P
                          INCLUDE INCLUDING INCREMENT INDEX INDEXES INET INFINITY INHERIT INHERITS
                          INITIALLY INLINE_P INNER_P INOUT INPUT_P INSENSITIVE INSERT INSTEAD
                          INT_P INTEGER INTERSECT INTERVAL INTO INVOKER IS ISNULL ISOLATION

                          JOIN JSON JSONB

                          KEY KEYSPACE KEYSPACES

                          LABEL LANGUAGE LARGE_P LAST_P LATERAL_P LEADING LEAKPROOF LEAST LEFT
                          LEVEL LIKE LIMIT LIST LISTEN LOAD LOCAL LOCALTIME LOCALTIMESTAMP LOCATION
                          LOCK_P LOCKED LOGGED LOGIN

                          MAP MAPPING MATCH MATERIALIZED MAXVALUE MINUTE_P MINVALUE MODE MODIFY
                          MONTH_P MOVE

                          NAME_P NAMES NAN NATIONAL NATURAL NCHAR NEXT NO NONE NOT NOTHING NOTIFY
                          NOTNULL NOWAIT NULL_P NULLIF NULLS_P NUMERIC

                          OBJECT_P OF OFF OFFSET OIDS ON ONLY OPERATOR OPTION OPTIONS OR ORDER
                          ORDINALITY OUT_P OUTER_P OVER OVERLAPS OVERLAY OWNED OWNER

                          PARSER PARTIAL PARTITION PASSING PASSWORD PERMISSION PERMISSIONS PLACING
                          PLANS POLICY POSITION PRECEDING PRECISION PRESERVE PREPARE PREPARED
                          PRIMARY PRIOR PRIVILEGES PROCEDURAL PROCEDURE PROGRAM

                          QUOTE

                          RANGE READ REAL REASSIGN RECHECK RECURSIVE REF REFRESH
                          REINDEX RELATIVE_P RELEASE RENAME REPEATABLE REPLACE REPLICA RESET
                          RESTART RESTRICT RETURNING RETURNS REVOKE RIGHT ROLE ROLES ROLLBACK ROLLUP
                          ROW ROWS RULE

                          SAVEPOINT SCHEMA SCHEME SCROLL SEARCH SECOND_P SECURITY SELECT SEQUENCE
                          SEQUENCES SERIALIZABLE SERVER SESSION SESSION_USER SET SETS SETOF

                          SHARE SHOW SIMILAR SIMPLE SKIP SMALLINT SNAPSHOT SOME SQL_P STABLE
                          STANDALONE_P START STATEMENT STATIC STATISTICS STATUS STDIN STDOUT STORAGE
                          STRICT_P STRIP_P SUBSTRING SUPERUSER SYMMETRIC SYSID SYSTEM_P

                          TABLE TABLES TABLESAMPLE TABLESPACE TEMP TEMPLATE TEMPORARY TEXT_P
                          THEN TIME TIMESTAMP TIMEUUID TINYINT TO TOKEN TRAILING TRANSACTION
                          TRANSFORM TREAT TRIGGER TRIM TRUE_P TRUNCATE TRUSTED TTL TUPLE
                          TYPE_P TYPES_P PARTITION_HASH

                          UNBOUNDED UNCOMMITTED UNENCRYPTED UNION UNIQUE UNKNOWN UNLISTEN
                          UNLOGGED UNSET UNTIL UPDATE USE USER USING UUID

                          VACUUM VALID VALIDATE VALIDATOR VALUE_P VALUES VARCHAR VARIADIC VARINT
                          VARYING VERBOSE VERSION_P VIEW VIEWS VOLATILE

                          WHEN WHERE WHITESPACE_P WINDOW WITH WITHIN WITHOUT WORK WRAPPER WRITE

                          XML_P XMLATTRIBUTES XMLCONCAT XMLELEMENT XMLEXISTS XMLFOREST
                          XMLPARSE XMLPI XMLROOT XMLSERIALIZE

                          YEAR_P YES_P

                          ZONE

// Identifer.
%token <PString>          IDENT

// Bind parameters: "$1", "$2", "$3", etc.
%token <PInt64>           PARAM

// Float constants.
%token <PString>          FCONST SCONST BCONST XCONST Op

// UUID constant.
%token <PString>          UCONST

// Integer constants.
%token <PString>          ICONST

// Character constants.
%token <PChar>            CCONST

// Multichar operators: "::", "..", ":=", "=>", "<=", ">=", "<>", "!=", "->", "->>".
%token                    TYPECAST DOT_DOT COLON_EQUALS EQUALS_GREATER LESS_EQUALS
                          GREATER_EQUALS NOT_EQUALS SINGLE_ARROW DOUBLE_ARROW

// Special ops. The grammar treat them as keywords, but they are not in the kwlist.h list and so can
// never be entered directly.  The filter in parser.c creates these tokens when required (based on
// looking one token ahead). NOT_LA exists so that productions such as NOT LIKE can be given the
// same precedence as LIKE; otherwise they'd effectively have the same precedence as NOT, at least
// with respect to their left-hand subexpression. NULLS_LA and WITH_LA are needed to make the
// grammar LALR(1). (Look Ahead Left to Right parser.)
//
// All *_LA tokens can be inserted into the grammer stack only manually - it's done in
// the LexProcessor::Scan() method in the scanner.cc module.
// OFFSET_LA and GROUP_LA were added to support using of these keywords as regular column names.
// - OFFSET_LA is used to support OFFSET clause in SELECT statement
// - GROUP_LA is used to support GROUP BY clause in SELECT statement
%token                    NOT_LA NULLS_LA WITH_LA OFFSET_LA GROUP_LA

%token                    SCAN_ERROR "incomprehensible_character_pattern"
%token END                0 "end_of_file"

//--------------------------------------------------------------------------------------------------
// Precedence: lowest to highest.
%nonassoc   SET                                                      // see relation_expr_opt_alias.
%left       UNION EXCEPT
%left       INTERSECT
%left       OR
%left       AND
%right      NOT
%nonassoc   IS ISNULL NOTNULL                                // IS sets precedence for IS NULL, etc.
%nonassoc   '<' '>' '=' LESS_EQUALS GREATER_EQUALS NOT_EQUALS
%nonassoc   BETWEEN IN_P LIKE ILIKE SIMILAR NOT_LA
%nonassoc   ESCAPE                                  // ESCAPE must be just above LIKE/ILIKE/SIMILAR.
%left       POSTFIXOP                                                 // dummy for postfix Op rules.

// To support target_el without AS, we must give IDENT an explicit priority
// between POSTFIXOP and Op.  We can safely assign the same priority to
// various unreserved keywords as needed to resolve ambiguities (this can't
// have any bad effects since obviously the keywords will still behave the
// same as if they weren't keywords).  We need to do this for PARTITION,
// RANGE, ROWS to support opt_existing_window_name; and for RANGE, ROWS
// so that they can follow a_expr without creating postfix-operator problems;
// and for NULL so that it can follow b_expr in ColQualList without creating
// postfix-operator problems.
//
// To support CUBE and ROLLUP in GROUP BY without reserving them, we give them
// an explicit priority lower than '(', so that a rule with CUBE '(' will shift
// rather than reducing a conflicting rule that takes CUBE as a function name.
// Using the same precedence as IDENT seems right for the reasons given above.
//
// The frame_bound productions UNBOUNDED PRECEDING and UNBOUNDED FOLLOWING
// are even messier: since UNBOUNDED is an unreserved keyword (per spec!),
// there is no principled way to distinguish these from the productions
// a_expr PRECEDING/FOLLOWING.  We hack this up by giving UNBOUNDED slightly
// lower precedence than PRECEDING and FOLLOWING.  At present this doesn't
// appear to cause UNBOUNDED to be treated differently from other unreserved
// keywords anywhere else in the grammar, but it's definitely risky.  We can
// blame any funny behavior of UNBOUNDED on the SQL standard, though.
%nonassoc UNBOUNDED                                  // ideally should have same precedence as IDENT
%nonassoc IDENT NULL_P PARTITION RANGE ROWS PRECEDING FOLLOWING CUBE ROLLUP
%left     Op OPERATOR                             // multi-character ops and user-defined operators
%left     '+' '-'
%left     '*' '/' '%'
%left     '^'

// Unary Operators.
%left     AT                                                     // sets precedence for AT TIME ZONE
%left     COLLATE
%right    UMINUS
%left     '[' ']'
%left     '(' ')'
%left     TYPECAST
%left     '.'
%nonassoc ',' '?'
%right    ':'

// These might seem to be low-precedence, but actually they are not part
// of the arithmetic hierarchy at all in their use as JOIN operators.
// We make them high-precedence to support their use as function names.
// They wouldn't be given a precedence at all, were it not that we need
// left-associativity among the JOIN rules themselves.
%left     JOIN CROSS LEFT FULL RIGHT INNER_P NATURAL

// kluge to keep xml_whitespace_option from causing shift/reduce conflicts.
%right    PRESERVE STRIP_P
%right    IF_P

//--------------------------------------------------------------------------------------------------
// Logging.
%printer { yyoutput << $$; } <*>;

//--------------------------------------------------------------------------------------------------
// Scanning cursor location: Keep tracking of current text location.
%locations

%%
%start stmtblock;

//--------------------------------------------------------------------------------------------------
// ACTIVE PARSING RULES.
// QL currently uses these PostgreQL rules.
//--------------------------------------------------------------------------------------------------
// The target production for the whole parse.
stmtblock:
  stmtmulti {
    $$ = $1;
    parser_->SaveGeneratedParseTree($$);
  }
  | BEGIN_P TRANSACTION
      dml_list ';'
    END_P TRANSACTION ';' {
    $3->Prepend(MAKE_NODE(@1, PTStartTransaction));
    $3->Append(MAKE_NODE(@5, PTCommit));
    $$ = $3;
    parser_->SaveGeneratedParseTree($$);
  }
;

// The thrashing around here is to discard "empty" statements...
stmtmulti:
  stmt {
    if ($1 == nullptr) {
      $$ = nullptr;
    } else {
      $$ = MAKE_NODE(@1, PTListNode, $1);
    }
  }
  | stmtmulti ';' stmt {
    if ($3 == nullptr) {
      $$ = $1;
    } else if ($1 == nullptr) {
      $$ = MAKE_NODE(@1, PTListNode, $3);
    } else {
      $1->Append($3);
      $$ = $1;
    }
  }
;

//--------------------------------------------------------------------------------------------------
// DML statement list
//--------------------------------------------------------------------------------------------------

dml_list:
  dml {
    $$ = MAKE_NODE(@1, PTListNode, $1);
  }
  |
  dml_list ';' dml {
    $1->Append($3);
    $$ = $1;
  }
;

dml:
  InsertStmt {
    if ($1 != nullptr) {
      parser_->SetBindVariables(static_cast<PTDmlStmt*>($1.get()));
    }
    $$ = $1;
  }
  | UpdateStmt {
    if ($1 != nullptr) {
      parser_->SetBindVariables(static_cast<PTDmlStmt*>($1.get()));
    }
    $$ = $1;
  }
  | DeleteStmt {
    if ($1 != nullptr) {
      parser_->SetBindVariables(static_cast<PTDmlStmt*>($1.get()));
    }
    $$ = $1;
  }
  | ExplainStmt {
      if ($1 != nullptr) {
        parser_->SetBindVariables(static_cast<PTDmlStmt*>($1.get()));
      }
      $$ = $1;
  }
;

stmt:
  /*EMPTY*/ {
    $$ = nullptr;
  }
  | CreateSchemaStmt {
    $$ = $1;
  }
  | UseSchemaStmt {
    $$ = $1;
  }
  | AlterSchemaStmt {
    $$ = $1;
  }
  | CreateTypeStmt {
    $$ = $1;
  }
  | CreateStmt {
    $$ = $1;
  }
  | CreateRoleStmt {
    $$ = $1;
  }
  | AlterRoleStmt {
    $$ = $1;
  }
  | GrantStmt {
    $$ = $1;
  }
  | GrantRoleStmt {
      $$ = $1;
  }
  | RevokeStmt {
     $$ = $1;
  }
  | RevokeRoleStmt {
      $$ = $1;
  }
  | TruncateStmt {
    $$ = $1;
  }
  | DropStmt {
    $$ = $1;
  }
  | AlterTableStmt {
    $$ = $1;
  }
  | IndexStmt {
    $$ = $1;
  }
  | SelectStmt { // SelectStmt rule is used to define a tuple (collection of data),
                 // so it might be either SELECT statement or VALUES clause.
    if ($1 != nullptr) {
      if ($1->IsDml()) {
        parser_->SetBindVariables(static_cast<PTDmlStmt*>($1.get()));
      } else { // PTInsertValuesClause, etc.
        PARSER_UNSUPPORTED(@1);
      }
    }
    $$ = $1;
  }
  | ExplainStmt {
      $$ = $1;
  }
  | InsertStmt {
    if ($1 != nullptr) {
      parser_->SetBindVariables(static_cast<PTDmlStmt*>($1.get()));
    }
    $$ = $1;
  }
  | DeleteStmt {
    if ($1 != nullptr) {
      parser_->SetBindVariables(static_cast<PTDmlStmt*>($1.get()));
    }
    $$ = $1;
  }
  | UpdateStmt {
    if ($1 != nullptr) {
      parser_->SetBindVariables(static_cast<PTDmlStmt*>($1.get()));
    }
    $$ = $1;
  }
  | TransactionStmt {
    $$ = $1;
  }
  | inactive_stmt {
    // Report error that the syntax is not yet supported.
    PARSER_UNSUPPORTED(@1);
  }
;

schema_stmt:
  CreateStmt {
    $$ = $1;
  }
  | inactive_schema_stmt {
    // Report error that the syntax is not yet supported.
    PARSER_UNSUPPORTED(@1);
  }
;

//--------------------------------------------------------------------------------------------------
// CREATE Type statement.
// Syntax:
//   CREATE TYPE [ IF NOT EXISTS ] keyspace_name.type_name (
//       field_name field_type,
//       field_name field_type,
//       ...
//   )
//--------------------------------------------------------------------------------------------------

CreateTypeStmt:
  CREATE TYPE_P qualified_name '(' TypeFieldList ')' {
     $$ = MAKE_NODE(@1, PTCreateType, $3, $5, false /* create_if_not_exists */);
  }
  | CREATE TYPE_P IF_P NOT_LA EXISTS qualified_name '(' TypeFieldList ')' {
     $$ = MAKE_NODE(@1, PTCreateType, $6, $8, true /* create_if_not_exists */);
  }
  | CREATE TYPE_P qualified_name AS ENUM_P '(' opt_enum_val_list ')' {
      PARSER_UNSUPPORTED(@1);
  }
  | CREATE TYPE_P qualified_name AS RANGE definition {
      PARSER_UNSUPPORTED(@1);
  }
;

TypeFieldList:
  TypeField {
    $$ = MAKE_NODE(@1, PTTypeFieldListNode, $1);
  }
  | TypeFieldList ',' TypeField {
    $1->Append($3);
    $$ = $1;
  }
;

TypeField:
  ColId Typename {
    $$ = MAKE_NODE(@1, PTTypeField, $1, $2);
  }
;

//--------------------------------------------------------------------------------------------------
// CREATE KEYSPACE statement.
// Syntax:
//   CREATE KEYSPACE | SCHEMA [ IF NOT EXISTS ] keyspace_name [ WITH REPLICATION = map
//                                                              AND DURABLE_WRITES =  true | false ]
//--------------------------------------------------------------------------------------------------

CreateSchemaStmt:
  CREATE KEYSPACE ColId opt_keyspace_options OptSchemaEltList {
    $$ = MAKE_NODE(@1, PTCreateKeyspace, $3, false, $4);
  }
  | CREATE SCHEMA ColId opt_keyspace_options OptSchemaEltList {
    $$ = MAKE_NODE(@1, PTCreateKeyspace, $3, false, $4);
  }
  | CREATE KEYSPACE IF_P NOT_LA EXISTS ColId opt_keyspace_options OptSchemaEltList {
    $$ = MAKE_NODE(@1, PTCreateKeyspace, $6, true, $7);
  }
  | CREATE SCHEMA IF_P NOT_LA EXISTS ColId opt_keyspace_options OptSchemaEltList {
    $$ = MAKE_NODE(@1, PTCreateKeyspace, $6, true, $7);
  }
  | CREATE KEYSPACE OptSchemaName AUTHORIZATION RoleSpec opt_keyspace_options OptSchemaEltList {
    PARSER_UNSUPPORTED(@4);
    $$ = nullptr;
  }
  | CREATE SCHEMA OptSchemaName AUTHORIZATION RoleSpec opt_keyspace_options OptSchemaEltList {
    PARSER_UNSUPPORTED(@4);
    $$ = nullptr;
  }
  | CREATE KEYSPACE IF_P NOT_LA EXISTS OptSchemaName AUTHORIZATION RoleSpec
  opt_keyspace_options OptSchemaEltList {
    PARSER_UNSUPPORTED(@7);
    $$ = nullptr;
  }
  | CREATE SCHEMA IF_P NOT_LA EXISTS OptSchemaName AUTHORIZATION RoleSpec
  opt_keyspace_options OptSchemaEltList {
    PARSER_UNSUPPORTED(@7);
    $$ = nullptr;
  }
;

OptSchemaName:
  ColId                     { $$ = $1; }
  | /* EMPTY */             { }
;

OptSchemaEltList:
  OptSchemaEltList schema_stmt {
    PARSER_UNSUPPORTED(@1);
  }
  | /* EMPTY */ {
  }
;

opt_keyspace_options:
  /*EMPTY*/ {
    $$ = nullptr;
  }
  | WITH keyspace_properties {
    $$ = $2;
  }
;

keyspace_properties:
  keyspace_property {
    $$ = $1;
  }
  | keyspace_properties AND keyspace_property {
    $1->AppendList($3);
    $$ = $1;
  }
;

keyspace_property:
  property_name '=' TRUE_P {
    PTConstBool::SharedPtr pt_constbool = MAKE_NODE(@3, PTConstBool, true);
    PTKeyspaceProperty::SharedPtr pt_keyspace_property =
        MAKE_NODE(@1, PTKeyspaceProperty, $1, pt_constbool);
    $$ = MAKE_NODE(@1, PTKeyspacePropertyListNode, pt_keyspace_property);
  }
  | property_name '=' FALSE_P {
    PTConstBool::SharedPtr pt_constbool = MAKE_NODE(@3, PTConstBool, false);
    PTKeyspaceProperty::SharedPtr pt_keyspace_property =
        MAKE_NODE(@1, PTKeyspaceProperty, $1, pt_constbool);
    $$ = MAKE_NODE(@1, PTKeyspacePropertyListNode, pt_keyspace_property);
  }
  | property_name '=' Sconst {
    PTConstText::SharedPtr pt_consttext = MAKE_NODE(@3, PTConstText, $3);
    PTKeyspaceProperty::SharedPtr pt_keyspace_property =
        MAKE_NODE(@1, PTKeyspaceProperty, $1, pt_consttext);
    $$ = MAKE_NODE(@1, PTKeyspacePropertyListNode, pt_keyspace_property);
  }
  | property_name '=' keyspace_property_map {
    $3->SetPropertyName($1);
    $$ = MAKE_NODE(@1, PTKeyspacePropertyListNode, $3);
  }
;

keyspace_property_map:
  '{' keyspace_property_map_list '}' {
    $$ = $2;
  }
;

keyspace_property_map_list:
  keyspace_property_map_list_element {
    $$ = MAKE_NODE(@1, PTKeyspacePropertyMap);
    $$->AppendMapElement($1);
  }
  | keyspace_property_map_list ',' keyspace_property_map_list_element {
    $1->AppendMapElement($3);
    $$ = $1;
  }
;

keyspace_property_map_list_element:
  Sconst ':' ICONST {
    PTConstVarInt::SharedPtr pt_constvarint = MAKE_NODE(@3, PTConstVarInt, $3);
    $$ = MAKE_NODE(@1, PTKeyspaceProperty, $1, pt_constvarint);
  }
  | Sconst ':' FCONST {
    PTConstDecimal::SharedPtr pt_constdecimal = MAKE_NODE(@3, PTConstDecimal, $3);
    $$ = MAKE_NODE(@1, PTKeyspaceProperty, $1, pt_constdecimal);
  }
  | Sconst ':' TRUE_P {
    PTConstBool::SharedPtr pt_constbool = MAKE_NODE(@3, PTConstBool, true);
    $$ = MAKE_NODE(@1, PTKeyspaceProperty, $1, pt_constbool);
  }
  | Sconst ':' FALSE_P {
    PTConstBool::SharedPtr pt_constbool = MAKE_NODE(@3, PTConstBool, false);
    $$ = MAKE_NODE(@1, PTKeyspaceProperty, $1, pt_constbool);  }
  | Sconst ':' Sconst {
    PTConstText::SharedPtr pt_consttext = MAKE_NODE(@3, PTConstText, $3);
    $$ = MAKE_NODE(@1, PTKeyspaceProperty, $1, pt_consttext);  }
;

//--------------------------------------------------------------------------------------------------
// USE KEYSPACE statement.
// Syntax:
//   USE keyspace_name
//--------------------------------------------------------------------------------------------------

UseSchemaStmt:
  USE ColId {
    $$ = MAKE_NODE(@1, PTUseKeyspace, $2);
  }
;

//--------------------------------------------------------------------------------------------------
// ALTER KEYSPACE statement.
// Syntax:
//   ALTER KEYSPACE | SCHEMA keyspace_name [ WITH REPLICATION = map
//                                           AND DURABLE_WRITES =  true | false ]
//--------------------------------------------------------------------------------------------------

AlterSchemaStmt:
  ALTER KEYSPACE ColId opt_keyspace_options OptSchemaEltList {
    $$ = MAKE_NODE(@1, PTAlterKeyspace, $3, $4);
  }
  | ALTER SCHEMA ColId opt_keyspace_options OptSchemaEltList {
    $$ = MAKE_NODE(@1, PTAlterKeyspace, $3, $4);
  }
;

//--------------------------------------------------------------------------------------------------
// CREATE TABLE statement.
// Syntax:
//   CREATE [ TEMPORARY ] TABLE [ IF NOT EXISTS ] qualified_name
//     ( column_definition [, column_definition ] )
//     [ Unused PostgreQL Options ]
//--------------------------------------------------------------------------------------------------

CreateStmt:
  CREATE OptTemp TABLE qualified_name '(' OptTableElementList ')'
  OptInherit opt_table_options OnCommitOption OptTableSpace {
    $$ = MAKE_NODE(@1, PTCreateTable, $4, $6, false, $9);
  }
  | CREATE OptTemp TABLE IF_P NOT_LA EXISTS qualified_name '(' OptTableElementList ')'
  OptInherit opt_table_options OnCommitOption OptTableSpace {
    $$ = MAKE_NODE(@1, PTCreateTable, $7, $9, true, $12);
  }
  | CREATE OptTemp TABLE qualified_name OF any_name
  OptTypedTableElementList opt_table_options OnCommitOption OptTableSpace {
    PARSER_UNSUPPORTED(@5);
    $$ = nullptr;
  }
  | CREATE OptTemp TABLE IF_P NOT_LA EXISTS qualified_name OF any_name
  OptTypedTableElementList opt_table_options OnCommitOption OptTableSpace {
    PARSER_UNSUPPORTED(@8);
    $$ = nullptr;
  }
;

OptTableElementList:
  /*EMPTY*/ {
    PARSER_ERROR(@0, INVALID_COLUMN_DEFINITION);
    $$ = nullptr;
  }
  | TableElementList {
    $$ = $1;
  }
;

TableElementList:
  TableElement {
    $$ = MAKE_NODE(@1, PTListNode, $1);
  }
  | TableElementList ',' TableElement {
    $1->Append($3);
    $$ = $1;
  }
;

TableElement:
  columnDef {
    $$ = $1;
  }
  | TableConstraint {
    $$ = $1;
  }
  | TableLikeClause {
    PARSER_UNSUPPORTED(@1);
  }
;

columnDef:
  ColId Typename create_generic_options ColQualList {
    $$ = MAKE_NODE(@1, PTColumnDefinition, $1, $2, $4);
  }
  | ColId Typename STATIC create_generic_options ColQualList {
    PTStatic::SharedPtr static_option = MAKE_NODE(@3, PTStatic);
    if ($5 == nullptr) {
      $5 = MAKE_NODE(@5, PTListNode, static_option);
    } else {
      $5->Append(static_option);
    }
    $$ = MAKE_NODE(@1, PTColumnDefinition, $1, $2, $5);
  }
;

ColQualList:
  /*EMPTY*/ {
    $$ = nullptr;
  }
  | ColQualList ColConstraint {
    if ($1 == nullptr) {
      $$ = MAKE_NODE(@1, PTListNode, $2);
    } else {
      $1->Append($2);
      $$ = $1;
    }
  }
;

ColConstraint:
  ColConstraintElem {
    $$ = $1;
  }
  | ConstraintAttr {
    PARSER_UNSUPPORTED(@1);
  }
  | COLLATE any_name {
    PARSER_UNSUPPORTED(@1);
  }
  | CONSTRAINT name ColConstraintElem {
    PARSER_UNSUPPORTED(@1);
  }
;

// DEFAULT NULL is already the default for Postgres.
// But define it here and carry it forward into the system
// to make it explicit.
// - thomas 1998-09-13
//
// WITH NULL and NULL are not SQL-standard syntax elements,
// so leave them out. Use DEFAULT NULL to explicitly indicate
// that a column may have that value. WITH NULL leads to
// shift/reduce conflicts with WITH TIME ZONE anyway.
// - thomas 1999-01-08
//
// DEFAULT expression must be b_expr not a_expr to prevent shift/reduce
// conflict on NOT (since NOT might start a subsequent NOT NULL constraint,
// or be part of a_expr NOT LIKE or similar constructs).
ColConstraintElem:
  PRIMARY KEY opt_definition OptConsTableSpace {
    $$ = MAKE_NODE(@1, PTPrimaryKey);
  }
  | NOT NULL_P {
    PARSER_UNSUPPORTED(@1);
  }
  | NULL_P {
    PARSER_UNSUPPORTED(@1);
  }
  | UNIQUE opt_definition OptConsTableSpace {
    PARSER_UNSUPPORTED(@1);
  }
  | CHECK '(' a_expr ')' opt_no_inherit {
    PARSER_UNSUPPORTED(@1);
  }
  | DEFAULT b_expr {
    PARSER_UNSUPPORTED(@1);
  }
;

// ConstraintAttr represents constraint attributes, which we parse as if
// they were independent constraint clauses, in order to avoid shift/reduce
// conflicts (since NOT might start either an independent NOT NULL clause
// or an attribute).  parse_utilcmd.c is responsible for attaching the
// attribute information to the preceding "real" constraint node, and for
// complaining if attribute clauses appear in the wrong place or wrong
// combinations.
//
// See also ConstraintAttributeSpec, which can be used in places where
// there is no parsing conflict.  (Note: currently, NOT VALID and NO INHERIT
// are allowed clauses in ConstraintAttributeSpec, but not here.  Someday we
// might need to allow them here too, but for the moment it doesn't seem
// useful in the statements that use ConstraintAttr.)
ConstraintAttr:
  DEFERRABLE {
  }
  | NOT DEFERRABLE {
  }
  | INITIALLY DEFERRED {
  }
  | INITIALLY IMMEDIATE {
  }
;

// ConstraintElem specifies constraint syntax which is not embedded into
// a column definition. ColConstraintElem specifies the embedded form.
// - thomas 1997-12-03
TableConstraint:
  CONSTRAINT name ConstraintElem {
    PARSER_UNSUPPORTED(@1);
  }
  | ConstraintElem {
    $$ = $1;
  }
;

ConstraintElem:
  PRIMARY KEY '(' NestedColumnList ')' opt_definition OptConsTableSpace ConstraintAttributeSpec {
    $$ = MAKE_NODE(@1, PTPrimaryKey, $4);
  }
  | PRIMARY KEY ExistingIndex ConstraintAttributeSpec {
    PARSER_UNSUPPORTED(@3);
  }
  | CHECK '(' a_expr ')' ConstraintAttributeSpec {
    PARSER_UNSUPPORTED(@1);
  }
  | UNIQUE '(' columnList ')' opt_definition OptConsTableSpace ConstraintAttributeSpec {
    PARSER_UNSUPPORTED(@1);
  }
  | UNIQUE ExistingIndex ConstraintAttributeSpec {
    PARSER_UNSUPPORTED(@1);
  }
  | EXCLUDE access_method_clause '(' ExclusionConstraintList ')'
  opt_definition OptConsTableSpace ExclusionWhereClause ConstraintAttributeSpec {
    PARSER_UNSUPPORTED(@1);
  }
;

opt_no_inherit:
  NO INHERIT                    {  $$ = true; }
  | /* EMPTY */                 {  $$ = false; }
    ;

opt_column_list:
  '(' columnList ')'            { $$ = nullptr; }
  | /*EMPTY*/ {
  }
;

NestedColumnList:
  index_column {
    $$ = MAKE_NODE(@1, PTListNode, $1);
  }
  | NestedColumnList ',' index_column {
    $1->Append($3);
    $$ = $1;
  }
  | '(' NestedColumnList ')' {
    $$ = MAKE_NODE(@1, PTListNode, $2);
  }
  | '(' NestedColumnList ')' ',' index_column {
    $$ = MAKE_NODE(@1, PTListNode, $2);
    $$->Append($5);
  }
;

// This can be "a_expr", but as of now, we only allow column_ref and json attributes.
index_column:
  columnref {
    // A columnref expression refers to a previously defined column.
    if (!$1->name()->IsSimpleName()) {
      PARSER_ERROR_MSG(@0, INVALID_ARGUMENTS, "Cannot use qualified name in this context");
    }
    $$ = MAKE_NODE(@1, PTIndexColumn, $1->name()->column_name(), $1);
  }
  | columnref json_ref {
    // Declare an index column here as generic expressions are not mapped to any pre-defined column.
    PTExpr::SharedPtr expr = MAKE_NODE(@1, PTJsonColumnWithOperators, $1->name(), $2);
    $$ = MAKE_NODE(@1, PTIndexColumn, parser_->MakeString(expr->QLName().c_str()), expr);
  }
;

index_column_list:
  index_column {
    $$ = MAKE_NODE(@1, PTListNode, $1);
  }
  | index_column_list ',' index_column {
    $1->Append($3);
    $$ = $1;
  }
;

columnList:
  columnElem {
    $$ = MAKE_NODE(@1, PTListNode, $1);
  }
  | columnList ',' columnElem {
    $1->Append($3);
    $$ = $1;
  }
;

columnElem:
  ColId {
    $$ = MAKE_NODE(@1, PTName, $1);
  }
;

key_match:
  MATCH FULL {
    $$ = FKCONSTR_MATCH_FULL;
  }
  | MATCH PARTIAL {
    $$ = FKCONSTR_MATCH_PARTIAL;
  }
  | MATCH SIMPLE {
    $$ = FKCONSTR_MATCH_SIMPLE;
  }
  | /*EMPTY*/ {
    $$ = FKCONSTR_MATCH_SIMPLE;
  }
;

ExclusionConstraintList:
  ExclusionConstraintElem {
    // $$ = list_make1($1);
  }
  | ExclusionConstraintList ',' ExclusionConstraintElem {
    // $$ = lappend($1, $3);
  }
;

ExclusionConstraintElem:
  index_elem WITH any_operator {
  }
  // allow OPERATOR() decoration for the benefit of ruleutils.c.
  | index_elem WITH OPERATOR '(' any_operator ')' {
  }
;

ExclusionWhereClause:
  WHERE '(' a_expr ')'          { $$ = nullptr; }
  | /*EMPTY*/                   { $$ = nullptr; }
;

// We combine the update and delete actions into one value temporarily
// for simplicity of parsing, and then break them down again in the
// calling production.  update is in the left 8 bits, delete in the right.
// Note that NOACTION is the default.
key_actions:
  key_update {
    $$ = ($1 << 8) | (FKCONSTR_ACTION_NOACTION & 0xFF);
  }
  | key_delete {
    $$ = (FKCONSTR_ACTION_NOACTION << 8) | ($1 & 0xFF);
  }
  | key_update key_delete {
    $$ = ($1 << 8) | ($2 & 0xFF);
  }
  | key_delete key_update {
    $$ = ($2 << 8) | ($1 & 0xFF);
  }
  | /*EMPTY*/ {
    $$ = (FKCONSTR_ACTION_NOACTION << 8) | (FKCONSTR_ACTION_NOACTION & 0xFF);
  }
;

key_update:
  ON UPDATE key_action {
    $$ = $3;
  }
;

key_delete:
  ON DELETE_P key_action {
    $$ = $3;
  }
;

key_action:
  NO ACTION         { $$ = FKCONSTR_ACTION_NOACTION; }
  | RESTRICT        { $$ = FKCONSTR_ACTION_RESTRICT; }
  | CASCADE         { $$ = FKCONSTR_ACTION_CASCADE; }
  | SET NULL_P      { $$ = FKCONSTR_ACTION_SETNULL; }
  | SET DEFAULT     { $$ = FKCONSTR_ACTION_SETDEFAULT; }
;

TableLikeClause:
  LIKE qualified_name TableLikeOptionList {
  }
;

TableLikeOptionList:
  TableLikeOptionList INCLUDING TableLikeOption   { $$ = $1 | $3; }
  | TableLikeOptionList EXCLUDING TableLikeOption { $$ = $1 & ~$3; }
  | /* EMPTY */                                   { $$ = 0; }
;

TableLikeOption:
  DEFAULTS        { $$ = CREATE_TABLE_LIKE_DEFAULTS; }
  | CONSTRAINTS   { $$ = CREATE_TABLE_LIKE_CONSTRAINTS; }
  | INDEXES       { $$ = CREATE_TABLE_LIKE_INDEXES; }
  | STORAGE       { $$ = CREATE_TABLE_LIKE_STORAGE; }
  | COMMENTS      { $$ = CREATE_TABLE_LIKE_COMMENTS; }
  | ALL           { $$ = CREATE_TABLE_LIKE_ALL; }
;

// Redundancy here is needed to avoid shift/reduce conflicts,
// since TEMP is not a reserved word.  See also OptTempTableName.
//
// NOTE: we accept both GLOBAL and LOCAL options.  They currently do nothing,
// but future versions might consider GLOBAL to request SQL-spec-compliant
// temp table behavior, so warn about that.  Since we have no modules the
// LOCAL keyword is really meaningless; furthermore, some other products
// implement LOCAL as meaning the same as our default temp table behavior,
// so we'll probably continue to treat LOCAL as a noise word.
OptTemp:
  /*EMPTY*/             { }
  | TEMPORARY           { PARSER_UNSUPPORTED(@1); }
  | TEMP                { PARSER_UNSUPPORTED(@1); }
  | LOCAL TEMPORARY     { PARSER_UNSUPPORTED(@1); }
  | LOCAL TEMP          { PARSER_UNSUPPORTED(@1); }
  | GLOBAL TEMPORARY    { PARSER_UNSUPPORTED(@1); }
  | GLOBAL TEMP         { PARSER_UNSUPPORTED(@1); }
  | UNLOGGED            { PARSER_UNSUPPORTED(@1); }
;

OptInherit:
  /*EMPTY*/ {
  }
  | INHERITS '(' qualified_name_list ')'  {
    PARSER_UNSUPPORTED(@1);
  }
;

opt_table_options:
  /*EMPTY*/ {
    $$ = nullptr;
  }
  | WITH table_properties {
    $$ = $2;
  }
  | WITH reloptions {
    PARSER_UNSUPPORTED(@1);
  }
  | WITH OIDS {
    PARSER_UNSUPPORTED(@1);
  }
  | WITHOUT OIDS {
    PARSER_UNSUPPORTED(@1);
  }
;

table_properties:
  table_property {
    $$ = $1;
  }
  | table_properties AND table_property {
    $1->AppendList($3);
    $$ = $1;
  }
;

table_property:
  property_name '=' ICONST {
    PTConstVarInt::SharedPtr pt_constvarint = MAKE_NODE(@3, PTConstVarInt, $3);
    PTTableProperty::SharedPtr pt_table_property = MAKE_NODE(@1, PTTableProperty, $1,
                                                             pt_constvarint);
    $$ = MAKE_NODE(@1, PTTablePropertyListNode, pt_table_property);
  }
  | property_name '=' FCONST {
    PTConstDecimal::SharedPtr pt_constdecimal = MAKE_NODE(@3, PTConstDecimal, $3);
    PTTableProperty::SharedPtr pt_table_property = MAKE_NODE(@1, PTTableProperty, $1,
                                                             pt_constdecimal);
    $$ = MAKE_NODE(@1, PTTablePropertyListNode, pt_table_property);
  }
  | property_name '=' TRUE_P {
    PTConstBool::SharedPtr pt_constbool = MAKE_NODE(@3, PTConstBool, true);
    PTTableProperty::SharedPtr pt_table_property = MAKE_NODE(@1, PTTableProperty, $1, pt_constbool);
    $$ = MAKE_NODE(@1, PTTablePropertyListNode, pt_table_property);
  }
  | property_name '=' FALSE_P {
    PTConstBool::SharedPtr pt_constbool = MAKE_NODE(@3, PTConstBool, false);
    PTTableProperty::SharedPtr pt_table_property = MAKE_NODE(@1, PTTableProperty, $1, pt_constbool);
    $$ = MAKE_NODE(@1, PTTablePropertyListNode, pt_table_property);
  }
  | property_name '=' Sconst {
    PTConstText::SharedPtr pt_consttext = MAKE_NODE(@3, PTConstText, $3);
    PTTableProperty::SharedPtr pt_table_property = MAKE_NODE(@1, PTTableProperty, $1, pt_consttext);
    $$ = MAKE_NODE(@1, PTTablePropertyListNode, pt_table_property);
  }
  | property_name '=' property_map {
    $3->SetPropertyName($1);
    $$ = MAKE_NODE(@1, PTTablePropertyListNode, $3);
  }
  | CLUSTERING ORDER BY '(' orderingList ')' {
    $$ = $5;
  }
  | COMPACT STORAGE {
    $$ = MAKE_NODE(@1, PTTablePropertyListNode);
  }
  | PARTITION SCHEME OF qualified_name {
    PTTableProperty::SharedPtr pt_table_property = MAKE_NODE(@4, PTTableProperty, $4);
    $$ = MAKE_NODE(@1, PTTablePropertyListNode, pt_table_property);
  }
;

property_map:
  '{' property_map_list '}' {
    $$ = $2;
  }
;

property_map_list:
  property_map_list_element {
    $$ = MAKE_NODE(@1, PTTablePropertyMap);
    $$->AppendMapElement($1);
  }
  | property_map_list ',' property_map_list_element {
    $1->AppendMapElement($3);
    $$ = $1;
  }
;

property_map_list_element:
  Sconst ':' ICONST {
    PTConstVarInt::SharedPtr pt_constvarint = MAKE_NODE(@3, PTConstVarInt, $3);
    $$ = MAKE_NODE(@1, PTTableProperty, $1, pt_constvarint);
  }
  | Sconst ':' FCONST {
    PTConstDecimal::SharedPtr pt_constdecimal = MAKE_NODE(@3, PTConstDecimal, $3);
    $$ = MAKE_NODE(@1, PTTableProperty, $1, pt_constdecimal);
  }
  | Sconst ':' TRUE_P {
    PTConstBool::SharedPtr pt_constbool = MAKE_NODE(@3, PTConstBool, true);
    $$ = MAKE_NODE(@1, PTTableProperty, $1, pt_constbool);
  }
  | Sconst ':' FALSE_P {
    PTConstBool::SharedPtr pt_constbool = MAKE_NODE(@3, PTConstBool, false);
    $$ = MAKE_NODE(@1, PTTableProperty, $1, pt_constbool);  }
  | Sconst ':' Sconst {
    PTConstText::SharedPtr pt_consttext = MAKE_NODE(@3, PTConstText, $3);
    $$ = MAKE_NODE(@1, PTTableProperty, $1, pt_consttext);  }
;

orderingList:
  column_ordering {
    $$ = MAKE_NODE(@1, PTTablePropertyListNode, $1);
  }
  | orderingList ',' column_ordering {
    $1->Append($3);
    $$ = $1;
  }
;

column_ordering:
  ColId opt_asc_desc {
    PTQualifiedName::SharedPtr name_node = MAKE_NODE(@1, PTQualifiedName, $1);
    PTExpr::SharedPtr expr = MAKE_NODE(@1, PTRef, name_node);
    $$ = MAKE_NODE(@1, PTTableProperty, expr, PTOrderBy::Direction($2));
  }
  | ColId json_ref opt_asc_desc {
    PTQualifiedName::SharedPtr name_node = MAKE_NODE(@1, PTQualifiedName, $1);
    PTExpr::SharedPtr expr = MAKE_NODE(@1, PTJsonColumnWithOperators, name_node, $2);
    $$ = MAKE_NODE(@1, PTTableProperty, expr, PTOrderBy::Direction($3));
  }
;

reloptions:
  '(' reloption_list ')' {
    PARSER_UNSUPPORTED(@2);
  }
;

opt_reloptions:
  /* EMPTY */ {
    $$ = nullptr;
  }
  | WITH reloptions {
    PARSER_UNSUPPORTED(@2);
  }
;

reloption_list:
  reloption_elem {
    PARSER_UNSUPPORTED(@1);
  }
  | reloption_list ',' reloption_elem {
    PARSER_UNSUPPORTED(@1);
  }
;

// This should match def_elem and also allow qualified names.
reloption_elem:
  ColLabel '=' def_arg {
  }
  | ColLabel {
  }
  | ColLabel '.' ColLabel '=' def_arg {
  }
  | ColLabel '.' ColLabel {
  }
;

OnCommitOption:
  /*EMPTY*/ {
    $$ = OnCommitAction::ONCOMMIT_NOOP;
  }
  | ON COMMIT DROP {
    PARSER_UNSUPPORTED(@1);
    $$ = OnCommitAction::ONCOMMIT_DROP;
  }
  | ON COMMIT DELETE_P ROWS {
    PARSER_UNSUPPORTED(@1);
    $$ = OnCommitAction::ONCOMMIT_DELETE_ROWS;
  }
  | ON COMMIT PRESERVE ROWS {
    PARSER_UNSUPPORTED(@1);
    $$ = OnCommitAction::ONCOMMIT_PRESERVE_ROWS;
  }
;

OptTableSpace:
  /*EMPTY*/ {
    $$ = nullptr;
  }
  | TABLESPACE name {
    PARSER_UNSUPPORTED(@1);
  }
;

OptConsTableSpace:
  /*EMPTY*/ {
    $$ = nullptr;
  }
  | USING INDEX TABLESPACE name {
    PARSER_UNSUPPORTED(@1);
  }
;

ExistingIndex:
  USING INDEX index_name {
    PARSER_UNSUPPORTED(@1);
  }
;

OptTypedTableElementList:
  /*EMPTY*/ {
  }
  | '(' TypedTableElementList ')' {
    $$ = $2;
  }
;

TypedTableElementList:
  TypedTableElement {
  }
  | TypedTableElementList ',' TypedTableElement {
  }
;

TypedTableElement:
  columnOptions {
  }
  | TableConstraint {
  }
;

columnOptions:
  ColId WITH OPTIONS ColQualList {
  }
;

/*****************************************************************************
 *
 *    QUERY:
 *
 *    DROP itemtype [ IF EXISTS ] itemname [, itemname ...]
 *           [ RESTRICT | CASCADE ]
 *
 *****************************************************************************/

DropStmt:
  DROP drop_type IF_P EXISTS any_name_list opt_drop_behavior {
    $$ = MAKE_NODE(@1, PTDropStmt, $2, $5, true);
  }
  | DROP drop_type any_name_list opt_drop_behavior {
    $$ = MAKE_NODE(@1, PTDropStmt, $2, $3, false);
  }
  | DROP ROLE IF_P EXISTS role_name {
    PTQualifiedName::SharedPtr name_node = MAKE_NODE(@1, PTQualifiedName, $5);
    PTQualifiedNameListNode::SharedPtr list_node = MAKE_NODE(
      @1, PTQualifiedNameListNode, name_node);
    $$ = MAKE_NODE(@1, PTDropStmt, OBJECT_ROLE, list_node, true);
  }
  | DROP ROLE role_name {
    PTQualifiedName::SharedPtr name_node = MAKE_NODE(@1, PTQualifiedName, $3);
    PTQualifiedNameListNode::SharedPtr list_node = MAKE_NODE(
      @1, PTQualifiedNameListNode, name_node);
    $$ = MAKE_NODE(@1, PTDropStmt, OBJECT_ROLE, list_node, false);
  }
  | DROP DOMAIN_P type_name_list opt_drop_behavior {
    PARSER_CQL_INVALID_MSG(@2, "DROP DOMAIN statement not supported");
  }
  | DROP DOMAIN_P IF_P EXISTS type_name_list opt_drop_behavior {
    PARSER_CQL_INVALID_MSG(@2, "DROP DOMAIN IF EXISTS statement not supported");
  }
  | DROP INDEX CONCURRENTLY any_name_list opt_drop_behavior {
    PARSER_CQL_INVALID_MSG(@2, "DROP INDEX CONCURRENTLY statement not supported");
  }
  | DROP INDEX CONCURRENTLY IF_P EXISTS any_name_list opt_drop_behavior {
    PARSER_CQL_INVALID_MSG(@2, "DROP INDEX CONCURRENTLY IF EXISTS statement not supported");
  }
;

drop_type:
  cql_drop_type {
    $$ = $1;
  }
  | ql_drop_type {
    PARSER_CQL_INVALID(@1);
  }
;

cql_drop_type:
  TABLE                           { $$ = OBJECT_TABLE; }
  | SCHEMA                        { $$ = OBJECT_SCHEMA; }
  | KEYSPACE                      { $$ = OBJECT_SCHEMA; }
  | TYPE_P                        { $$ = OBJECT_TYPE; }
  | INDEX                         { $$ = OBJECT_INDEX; }
;

ql_drop_type:
  SEQUENCE                        { $$ = OBJECT_SEQUENCE; }
  | VIEW                          { $$ = OBJECT_VIEW; }
  | MATERIALIZED VIEW             { $$ = OBJECT_MATVIEW; }
  | FOREIGN TABLE                 { $$ = OBJECT_FOREIGN_TABLE; }
  | EVENT TRIGGER                 { $$ = OBJECT_EVENT_TRIGGER; }
  | COLLATION                     { $$ = OBJECT_COLLATION; }
  | CONVERSION_P                  { $$ = OBJECT_CONVERSION; }
  | EXTENSION                     { $$ = OBJECT_EXTENSION; }
  | TEXT_P SEARCH PARSER          { $$ = OBJECT_TSPARSER; }
  | TEXT_P SEARCH DICTIONARY      { $$ = OBJECT_TSDICTIONARY; }
  | TEXT_P SEARCH TEMPLATE        { $$ = OBJECT_TSTEMPLATE; }
  | TEXT_P SEARCH CONFIGURATION   { $$ = OBJECT_TSCONFIGURATION; }
;

any_name_list:
  any_name {
    $$ = MAKE_NODE(@1, PTQualifiedNameListNode, $1);
  }
  | any_name_list ',' any_name {
    $1->Append($3);
    $$ = $1;
  }
;

any_name:
  ColId {
    $$ = MAKE_NODE(@1, PTQualifiedName, $1);
  }
  | ColId attrs {
    PTName::SharedPtr name_node = MAKE_NODE(@1, PTName, $1);
    $2->Prepend(name_node);
    $$ = $2;
  }
;

attrs:
  '.' attr_name {
    $$ = MAKE_NODE(@2, PTQualifiedName, $2);
  }
  | attrs '.' attr_name {
    PTName::SharedPtr name_node = MAKE_NODE(@3, PTName, $3);
    $1->Append(name_node);
    $$ = $1;
  }
;

type_name_list:
  Typename {
  }
  | type_name_list ',' Typename {
  }
;

//--------------------------------------------------------------------------------------------------
// ALTER TABLE statement.
//--------------------------------------------------------------------------------------------------

/* This structure is according to Apache Cassandra 3.0 CQL specs */
AlterTableStmt:
  ALTER TABLE relation_expr alter_table_ops {
    $$ = MAKE_NODE(@1, PTAlterTable, $3, $4);
  }
  | InactiveAlterTableStmt {
    PARSER_UNSUPPORTED(@1);
  }
;

alter_table_ops:
  alter_table_op {
    $$ = $1;
  }
  | alter_table_ops alter_table_op {
    $1->Splice($2);
    $$ = $1;
  }
;

alter_table_op:
  ADD_P addColumnDefList {
    $$ = $2;
  }
  | DROP dropColumnList {
    $$ = $2;
  }
  | WITH alterPropertyList {
    $$ = $2;
  }
  | RENAME renameColumnList {
    $$ = $2;
  }
  | ALTER alterColumnTypeList {
    $$ = $2;
  }
;

addColumnDefList:
  addColumnDef {
    $$ = MAKE_NODE(@1, PTListNode, $1);
  }
  | addColumnDefList ',' addColumnDef {
    $1->Append($3);
    $$ = $1;
  }
;

addColumnDef:
  ColId Typename {
    $$ = MAKE_NODE(@1, PTAlterColumnDefinition, nullptr, $1, $2, ALTER_ADD);
  }
;

dropColumnList:
  dropColumn {
    $$ = MAKE_NODE(@1, PTListNode, $1);
  }
  | dropColumnList ',' dropColumn {
    $1->Append($3);
    $$ = $1;
  }
;

dropColumn:
  qualified_name {
    $$ = MAKE_NODE(@1, PTAlterColumnDefinition, $1, nullptr, nullptr, ALTER_DROP);
  }
;

renameColumnList:
  renameColumn {
    $$ = MAKE_NODE(@1, PTListNode, $1);
  }
  | renameColumnList ',' renameColumn {
    $1->Append($3);
    $$ = $1;
  }
;

renameColumn:
  qualified_name TO ColId {
    $$ = MAKE_NODE(@1, PTAlterColumnDefinition, $1, $3, nullptr, ALTER_RENAME);
  }
;

alterColumnTypeList:
  alterColumnType {
    $$ = MAKE_NODE(@1, PTListNode, $1);
  }
  | alterColumnTypeList ',' alterColumnType {
    $1->Append($3);
    $$ = $1;
  }
;

alterColumnType:
  qualified_name TYPE_P Typename {
    PARSER_UNSUPPORTED(@3);
  }
;

alterPropertyList:
  alterProperty {
    $$ = MAKE_NODE(@1, PTListNode, $1);
  }
  | alterPropertyList AND alterProperty {
    $1->Append($3);
    $$ = $1;
  }
;

alterProperty:
  property_name '=' AexprConst {
    $$ = MAKE_NODE(@1, PTTableProperty, $1, $3);
  }
;

//--------------------------------------------------------------------------------------------------
// SELECT statements.
//--------------------------------------------------------------------------------------------------

// A complete SELECT statement looks like this.
//
// The rule returns either a single SelectStmt node or a tree of them,
// representing a set-operation tree.
//
// There is an ambiguity when a sub-SELECT is within an a_expr and there
// are excess parentheses: do the parentheses belong to the sub-SELECT or
// to the surrounding a_expr?  We don't really care, but bison wants to know.
// To resolve the ambiguity, we are careful to define the grammar so that
// the decision is staved off as long as possible: as long as we can keep
// absorbing parentheses into the sub-SELECT, we will do so, and only when
// it's no longer possible to do that will we decide that parens belong to
// the expression.  For example, in "SELECT (((SELECT 2)) + 3)" the extra
// parentheses are treated as part of the sub-select.  The necessity of doing
// it that way is shown by "SELECT (((SELECT 2)) UNION SELECT 2)".  Had we
// parsed "((SELECT 2))" as an a_expr, it'd be too late to go back to the
// SELECT viewpoint when we see the UNION.
//
// This approach is implemented by defining a nonterminal select_with_parens,
// which represents a SELECT with at least one outer layer of parentheses,
// and being careful to use select_with_parens, never '(' SelectStmt ')',
// in the expression grammar.  We will then have shift-reduce conflicts
// which we can resolve in favor of always treating '(' <select> ')' as
// a select_with_parens.  To resolve the conflicts, the productions that
// conflict with the select_with_parens productions are manually given
// precedences lower than the precedence of ')', thereby ensuring that we
// shift ')' (and then reduce to select_with_parens) rather than trying to
// reduce the inner <select> nonterminal to something else.  We use UMINUS
// precedence for this, which is a fairly arbitrary choice.
//
// To be able to define select_with_parens itself without ambiguity, we need
// a nonterminal select_no_parens that represents a SELECT structure with no
// outermost parentheses.  This is a little bit tedious, but it works.
//
// In non-expression contexts, we use SelectStmt which can represent a SELECT
// with or without outer parentheses.
SelectStmt:
  select_no_parens        %prec UMINUS {
    $$ = $1;
  }
  | select_with_parens    %prec UMINUS {
    PARSER_UNSUPPORTED(@1);
  }
;

select_with_parens:
  '(' select_no_parens ')' {
    $$ = $2;
  }
  | '(' select_with_parens ')' {
    $$ = $2;
  }
;

// This rule parses the equivalent of the standard's <query expression>.
// The duplicative productions are annoying, but hard to get rid of without
// creating shift/reduce conflicts.
//
//  The locking clause (FOR UPDATE etc) may be before or after LIMIT/OFFSET.
//  In <=7.2.X, LIMIT/OFFSET had to be after FOR UPDATE
//  We now support both orderings, but prefer LIMIT/OFFSET before the locking
// clause.
//  2002-08-28 bjm
select_no_parens:
  values_clause {
    $$ = $1;
  }
  | simple_select opt_allow_filtering {
    $$ = $1;
  }
  | simple_select sort_clause opt_allow_filtering {
    $1->SetOrderByClause($2);
    $$ = $1;
  }
  | simple_select opt_sort_clause select_limit_offset opt_for_locking_clause opt_allow_filtering {
    $1->SetOrderByClause($2);
    $1->SetLimitClause($3->at(0));
    $1->SetOffsetClause($3->at(1));
    $$ = $1;
  }
  | simple_select opt_sort_clause for_locking_clause opt_select_limit_offset opt_allow_filtering {
    PARSER_UNSUPPORTED(@3);
  }
;

select_clause:
  simple_select {
    $$ = $1;
  }
  | select_with_parens {
    PARSER_UNSUPPORTED(@1);
  }
;

// This rule parses SELECT statements that can appear within set operations,
// including UNION, INTERSECT and EXCEPT.  '(' and ')' can be used to specify
// the ordering of the set operations.  Without '(' and ')' we want the
// operations to be ordered per the precedence specs at the head of this file.
//
// As with select_no_parens, simple_select cannot have outer parentheses,
// but can have parenthesized subclauses.
//
// Note that sort clauses cannot be included at this level --- SQL requires
//    SELECT foo UNION SELECT bar ORDER BY baz
// to be parsed as
//    (SELECT foo UNION SELECT bar) ORDER BY baz
// not
//    SELECT foo UNION (SELECT bar ORDER BY baz)
// Likewise for WITH, FOR UPDATE and LIMIT.  Therefore, those clauses are
// described as part of the select_no_parens production, not simple_select.
// This does not limit functionality, because you can reintroduce these
// clauses inside parentheses.
//
// NOTE: only the leftmost component SelectStmt should have INTO.
// However, this is not checked by the grammar; parse analysis must check it.
simple_select:
  SELECT opt_all_clause target_list into_clause from_clause opt_where_clause opt_if_clause
  group_clause having_clause opt_window_clause {
    $$ = MAKE_NODE(@1, PTSelectStmt, false, $3, $5, $6, $7, $8, $9, nullptr, nullptr, nullptr);
  }
  | SELECT distinct_clause target_list into_clause from_clause opt_where_clause opt_if_clause
  group_clause having_clause opt_window_clause {
    $$ = MAKE_NODE(@1, PTSelectStmt, true, $3, $5, $6, $7, $8, $9, nullptr, nullptr, nullptr);
  }
  | TABLE relation_expr {
    PARSER_UNSUPPORTED(@1);
  }
  | select_clause UNION all_or_distinct select_clause {
    PARSER_UNSUPPORTED(@2);
  }
  | select_clause INTERSECT all_or_distinct select_clause {
    PARSER_UNSUPPORTED(@2);
  }
  | select_clause EXCEPT all_or_distinct select_clause {
    PARSER_UNSUPPORTED(@2);
  }
;

values_clause:
  VALUES ctext_row {
    $$ = MAKE_NODE(@1, PTInsertValuesClause, $2);
  }
  | values_clause ',' ctext_row {
    PARSER_NOCODE(@2);
    $1->Append($3);
    $$ = $1;
  }
;

// Sconst parsing is delayed
json_clause:
  JSON Sconst opt_json_clause_default_null {
    PTConstText::SharedPtr json_expr = MAKE_NODE(@2, PTConstText, $2);
    $$ = MAKE_NODE(@1, PTInsertJsonClause, json_expr, $3);
  }
  | JSON bindvar opt_json_clause_default_null {
    if ($2 != nullptr) {
      parser_->AddBindVariable(static_cast<PTBindVar*>($2.get()));
    }
    $$ = MAKE_NODE(@1, PTInsertJsonClause, $2, $3);
  }
;

opt_json_clause_default_null:
  /* EMPTY */ {
    $$ = true;
  }
  | DEFAULT NULL_P {
    $$ = true;
  }
  | DEFAULT UNSET {
    $$ = false;
  }
;

into_clause:
  /* EMPTY */ {
    $$ = nullptr;
  }
  | INTO OptTempTableName {
    PARSER_UNSUPPORTED(@1);
    $$ = nullptr;
  }
;

// Redundancy here is needed to avoid shift/reduce conflicts,
// since TEMP is not a reserved word.  See also OptTemp.
OptTempTableName:
  TEMPORARY opt_table qualified_name {
  }
  | TEMP opt_table qualified_name {
  }
  | LOCAL TEMPORARY opt_table qualified_name {
  }
  | LOCAL TEMP opt_table qualified_name {
  }
  | GLOBAL TEMPORARY opt_table qualified_name {
  }
  | GLOBAL TEMP opt_table qualified_name {
  }
  | UNLOGGED opt_table qualified_name {
  }
  | TABLE qualified_name {
  }
  | qualified_name {
  }
;

opt_table:
  /* EMPTY */ {
  }
  | TABLE {
  }
;

all_or_distinct:
  /* EMPTY */ {
    $$ = false;
  }
  | ALL {
    $$ = true;
  }
  | DISTINCT {
    $$ = false;
  }
;

// We use (NIL) as a placeholder to indicate that all target expressions
// should be placed in the DISTINCT list during parsetree analysis.
distinct_clause:
  DISTINCT {
  }
  | DISTINCT ON '(' expr_list ')' {
    PARSER_UNSUPPORTED(@2);
  }
;

opt_all_clause:
  /* EMPTY */ {
  }
  | ALL {
    PARSER_UNSUPPORTED(@1);
  }
;

opt_sort_clause:
  /* EMPTY */ {
    $$ = nullptr;
  }
  | sort_clause {
    $$ = $1;
  }
;

sort_clause:
  ORDER BY sortby_list {
    $$ = $3;
  }
;

sortby_list:
  sortby {
    $$ = MAKE_NODE(@1, PTOrderByListNode, $1);
  }
  | sortby_list ',' sortby {
    $1->Append($3);
    $$ = $1;
  }
;

sortby:
  a_expr opt_asc_desc opt_nulls_order {
    $$ = MAKE_NODE(@1, PTOrderBy, $1, PTOrderBy::Direction($2), PTOrderBy::NullPlacement($3));
  }
  | a_expr USING qual_all_Op opt_nulls_order {
    PARSER_UNSUPPORTED(@2);
    $$ = nullptr;
  }
;

// SELECT target list.
opt_target_list:
  /* EMPTY */ {
    $$ = nullptr;
  }
  | target_list {
    $$ = $1;
  }
;

target_list:
  target_el {
    $$ = MAKE_NODE(@1, PTExprListNode, $1);
  }
  | target_list ',' target_el {
    $1->Append($3);
    $$ = $1;
  }
;

target_el:
  a_expr AS ColLabel {
    $$ = MAKE_NODE(@1, PTExprAlias, $1, $3);
  }
  | a_expr {
    $$ = $1;
  }
  | '*' {
    $$ = MAKE_NODE(@1, PTAllColumns);
  }
  // We support omitting AS only for column labels that aren't
  // any known keyword.  There is an ambiguity against postfix
  // operators: is "a ! b" an infix expression, or a postfix
  // expression and a column label?  We prefer to resolve this
  // as an infix expression, which we accomplish by assigning
  // IDENT a precedence higher than POSTFIXOP.
  | a_expr IDENT {
    $$ = MAKE_NODE(@1, PTExprAlias, $1, $2);
  }
;

// SELECT ALLOW FILTERING.
opt_allow_filtering:
  ALLOW FILTERING {
    $$ = true;
  }
  | /*EMPTY*/     {
    $$ = false;
  }
;

// SELECT LIMIT AND/OR OFFSET.
select_limit_offset:
  limit_clause {
    $$ = MCMakeShared<MCVector<PExpr>>(PTREE_MEM, 2);
    $$->at(0) = $1;
    $$->at(1) = nullptr;
  }
  | offset_clause {
    $$ = MCMakeShared<MCVector<PExpr>>(PTREE_MEM, 2);
    $$->at(0) = nullptr;
    $$->at(1) = $1;
  }
  | limit_clause offset_clause {
    $$ = MCMakeShared<MCVector<PExpr>>(PTREE_MEM, 2);
    $$->at(0) = $1;
    $$->at(1) = $2;
  }
  | offset_clause limit_clause {
    $$ = MCMakeShared<MCVector<PExpr>>(PTREE_MEM, 2);
    $$->at(0) = $2;
    $$->at(1) = $1;
  }
;

opt_select_limit_offset:
  /* EMPTY */ {
    $$ = nullptr;
  }
  | select_limit_offset {
    $$ = $1;
  }
;

limit_clause:
  LIMIT select_limit_value {
    $$ = $2;
  }
  | LIMIT select_limit_value ',' select_offset_value {
    PARSER_UNSUPPORTED(@3);
    $$ = nullptr;
  }
  // SQL:2008 syntax
  | FETCH first_or_next opt_select_fetch_first_value row_or_rows ONLY {
    $$ = nullptr;
  }
;

offset_clause:
  OFFSET_LA select_offset_value {
    $$ = $2;
  }
;

select_limit_value:
  ICONST {
    $$ = MAKE_NODE(@1, PTConstVarInt, $1);
  }
  | bindvar {
    parser_->AddBindVariable(static_cast<PTBindVar*>($1.get()));
    $$ = $1;
  }
  | ALL {
    PARSER_UNSUPPORTED(@1);
    $$ = nullptr;
  }
;

select_offset_value:
  ICONST {
    $$ = MAKE_NODE(@1, PTConstVarInt, $1);
  }
  | bindvar {
    parser_->AddBindVariable(static_cast<PTBindVar*>($1.get()));
    $$ = $1;
  }
;

// Allowing full expressions without parentheses causes various parsing
// problems with the trailing ROW/ROWS key words.  SQL only calls for
// constants, so we allow the rest only with parentheses.  If omitted,
// default to 1.
opt_select_fetch_first_value:
  /* EMPTY */ {
  }
  | SignedIconst {
  }
  | '(' a_expr ')' {
  }
;

// noise words.
row_or_rows:
  ROW {
  }
  | ROWS {
  }
;

first_or_next:
  FIRST_P {
    $$ = 0;
  }
  | NEXT {
    $$ = 1;
  }
;

// This syntax for group_clause tries to follow the spec quite closely.
// However, the spec allows only column references, not expressions,
// which introduces an ambiguity between implicit row constructors
// (a,b) and lists of column references.
//
// We handle this by using the a_expr production for what the spec calls
// <ordinary grouping set>, which in the spec represents either one column
// reference or a parenthesized list of column references. Then, we check the
// top node of the a_expr to see if it's an implicit RowExpr, and if so, just
// grab and use the list, discarding the node. (this is done in parse analysis,
// not here)
//
// (we abuse the row_format field of RowExpr to distinguish implicit and
// explicit row constructors; it's debatable if anyone sanely wants to use them
// in a group clause, but if they have a reason to, we make it possible.)
//
// Each item in the group_clause list is either an expression tree or a
// GroupingSet node of some type.
group_clause:
  /*EMPTY*/ {
    $$ = nullptr;
  }
  | GROUP_LA BY group_by_list {
    $$ = $3;
  }
;

group_by_list:
  group_by_item {
    $$ = MAKE_NODE(@1, PTListNode, $1);
  }
  | group_by_list ',' group_by_item {
    $1->Append($3);
    $$ = $1;
  }
;

group_by_item:
  a_expr {
    $$ = $1;
  }
  | empty_grouping_set {
    PARSER_UNSUPPORTED(@1);
  }
  | cube_clause {
    PARSER_UNSUPPORTED(@1);
  }
  | rollup_clause {
    PARSER_UNSUPPORTED(@1);
  }
  | grouping_sets_clause {
    PARSER_UNSUPPORTED(@1);
  }
;

empty_grouping_set:
  '(' ')' {
  }
;

// These hacks rely on setting precedence of CUBE and ROLLUP below that of '(',
// so that they shift in these rules rather than reducing the conflicting
// unreserved_keyword rule.
rollup_clause:
  ROLLUP '(' expr_list ')' {
  }
;

cube_clause:
  CUBE '(' expr_list ')' {
  }
;

grouping_sets_clause:
  GROUPING SETS '(' group_by_list ')' {
  }
;

// HAVING.
having_clause:
  /* EMPTY */ {
    $$ = nullptr;
  }
  | HAVING a_expr {
    PARSER_UNSUPPORTED(@1);
  }
;

// LOCKING.
opt_for_locking_clause:
  /* EMPTY */ {
    $$ = nullptr;
  }
  | for_locking_clause {
    PARSER_UNSUPPORTED(@1);
  }
;

for_locking_clause:
  for_locking_items {
  }
  | FOR READ ONLY {
  }
;

for_locking_items:
  for_locking_item {
  }
  | for_locking_items for_locking_item  {
  }
;

for_locking_item:
  for_locking_strength locked_rels_list opt_nowait_or_skip {
  }
;

for_locking_strength:
  FOR UPDATE                  { }
  | FOR NO KEY UPDATE         { }
  | FOR SHARE                 { }
  | FOR KEY SHARE             { }
;

locked_rels_list:
  /* EMPTY */ {
  }
  | OF qualified_name_list {
  }
;

//--------------------------------------------------------------------------------------------------
// INSERT statement.
//--------------------------------------------------------------------------------------------------

InsertStmt:
  INSERT INTO insert_target '(' insert_column_list ')' values_clause opt_using_ttl_timestamp_clause
  opt_returns_clause
  {
    $$ = MAKE_NODE(@2, PTInsertStmt, $3, $5, $7, nullptr, false, $8, $9);
  }
  | INSERT INTO insert_target '(' insert_column_list ')' values_clause if_clause opt_else_clause
  opt_using_ttl_timestamp_clause opt_returns_clause
  {
    $$ = MAKE_NODE(@2, PTInsertStmt, $3, $5, $7, $8, $9, $10, $11);
  }
  | INSERT INTO insert_target json_clause opt_on_conflict opt_using_ttl_timestamp_clause
  opt_returns_clause
  {
    $$ = MAKE_NODE(@2, PTInsertStmt, $3, nullptr, $4, nullptr, false, $6, $7);
  }
  | INSERT INTO insert_target DEFAULT VALUES opt_on_conflict returning_clause {
    PARSER_CQL_INVALID_MSG(@4, "DEFAULT VALUES feature is not supported");
  }
  | INSERT INTO insert_target values_clause opt_on_conflict returning_clause {
    PARSER_CQL_INVALID_MSG(@4, "Missing list of target columns");
  }
;

opt_returns_clause:
  /* EMPTY */ {
    $$ = false;
  }
  | RETURNS STATUS AS ROW {
    $$ = true;
  }
;

// Can't easily make AS optional here, because VALUES in insert_rest would
// have a shift/reduce conflict with VALUES as an optional alias.  We could
// easily allow unreserved_keywords as optional aliases, but that'd be an odd
// divergence from other places.  So just require AS for now.
insert_target:
  qualified_name {
    $$ = $1;
  }
  | qualified_name AS ColId {
    PARSER_UNSUPPORTED(@2);
  }
;

insert_column_list:
  insert_column_item {
    $$ = MAKE_NODE(@1, PTQualifiedNameListNode, $1);
  }
  | insert_column_list ',' insert_column_item {
    $1->Append($3);
    $$ = $1;
  }
;

insert_column_item:
  ColId opt_indirection {
    if ($2 == nullptr) {
      $$ = MAKE_NODE(@1, PTQualifiedName, $1);
    } else {
      PTName::SharedPtr name_node = MAKE_NODE(@1, PTName, $1);
      $2->Prepend(name_node);
      $$ = $2;
    }
  }
;

opt_indirection:
  /*EMPTY*/ {
    $$ = nullptr;
  }
  | opt_indirection indirection_el {
    if ($1 == nullptr) {
      $$ = MAKE_NODE(@1, PTQualifiedName, $2);
    } else {
      $1->Append($2);
      $$ = $1;
    }
  }
;

opt_on_conflict:
  /*EMPTY*/ {
  }
  | ON CONFLICT opt_conf_expr DO UPDATE SET set_clause_list opt_where_clause {
    PARSER_UNSUPPORTED(@1);
  }
  | ON CONFLICT opt_conf_expr DO NOTHING {
    PARSER_UNSUPPORTED(@1);
  }
;

opt_using_ttl_timestamp_clause:
  /*EMPTY*/ {
    $$ = nullptr;
  }
  | using_ttl_timestamp_clause {
    $$ = $1;
  }
;

using_ttl_timestamp_clause:
  USING recursive_ttl_timestamp_clause {
    $$ = $2;
  }
;

recursive_ttl_timestamp_clause:
  ttl_timestamp_clause {
    $$ = MAKE_NODE(@1, PTDmlUsingClause);
    $$->Append($1);
  }
  |
  recursive_ttl_timestamp_clause AND ttl_timestamp_clause {
    $1->Append($3);
    $$ = $1;
  }
;

ttl_timestamp_clause:
  TTL c_expr {
    $$ = MAKE_NODE(@1, PTDmlUsingClauseElement, parser_->MakeString($1), $2);
  }
  |
  TIMESTAMP b_expr {
    $$ = MAKE_NODE(@1, PTDmlUsingClauseElement, parser_->MakeString($1), $2);
  }
;

opt_conf_expr:
  /*EMPTY*/ {
  }
  | '(' index_params ')' opt_where_clause {
  }
  | ON CONSTRAINT name {
  }
;

returning_clause:
  /* EMPTY */ {
  }
  | RETURNING target_list {
    PARSER_CQL_INVALID_MSG(@1, "RETURNING clause is not supported");
  }
;

//--------------------------------------------------------------------------------------------------
// DELETE STATEMENTS
//--------------------------------------------------------------------------------------------------

DeleteStmt:
DELETE_P opt_target_list FROM relation_expr_opt_alias opt_using_ttl_timestamp_clause
  opt_where_or_current_clause opt_returns_clause {
    $$ = MAKE_NODE(@1, PTDeleteStmt, $2, $4, $5, $6, nullptr, false, $7);
  }
  | DELETE_P opt_target_list FROM relation_expr_opt_alias
  opt_using_ttl_timestamp_clause where_or_current_clause if_clause opt_else_clause
  opt_returns_clause {
    $$ = MAKE_NODE(@1, PTDeleteStmt, $2, $4, $5, $6, $7, $8, $9);
  }
;

//--------------------------------------------------------------------------------------------------
// UPDATE statement.
//--------------------------------------------------------------------------------------------------

UpdateStmt:
  UPDATE relation_expr_opt_alias opt_using_ttl_timestamp_clause SET set_clause_list
  opt_where_or_current_clause opt_returns_clause {
    $$ = MAKE_NODE(@1, PTUpdateStmt, $2, $5, $6, nullptr, false, $3, $7);
  }
  | UPDATE relation_expr_opt_alias opt_using_ttl_timestamp_clause SET
  set_clause_list opt_where_or_current_clause if_clause opt_else_clause opt_returns_clause {
    $$ = MAKE_NODE(@1, PTUpdateStmt, $2, $5, $6, $7, $8, $3, $9);
  }
;

set_clause_list:
  set_clause {
    $$ = MAKE_NODE(@1, PTAssignListNode, $1);
  }
  | set_clause_list ',' set_clause {
    $1->Append($3);
    $$ = $1;
  }
;

set_clause:
  single_set_clause {
    $$ = $1;
  }
  | multiple_set_clause {
    PARSER_UNSUPPORTED(@1);
  }
;

single_set_clause:
  set_target '=' ctext_expr {
    $$ = MAKE_NODE(@1, PTAssign, $1, $3);
  }
  | set_target col_arg_list '=' ctext_expr {
    $$ = MAKE_NODE(@1, PTAssign, $1, $4, $2);
  }
  | set_target json_ref_single_arrow '=' ctext_expr {
    $$ = MAKE_NODE(@1, PTAssign, $1, $4, nullptr, $2);
  }
;

col_arg_list:
  '[' c_expr ']' {
    $$ = MAKE_NODE(@1, PTExprListNode);
    $$->Append($2);
  }
  | col_arg_list '[' c_expr ']' {
    $1->Append($3);
    $$ = $1;
  }
;

// Ideally, we'd accept any row-valued a_expr as RHS of a multiple_set_clause.
// However, per SQL spec the row-constructor case must allow DEFAULT as a row
// member, and it's pretty unclear how to do that (unless perhaps we allow
// DEFAULT in any a_expr and let parse analysis sort it out later?).  For the
// moment, the planner/executor only support a subquery as a multiassignment
// source anyhow, so we need only accept ctext_row and subqueries here.
multiple_set_clause:
  '(' set_target_list ')' '=' ctext_row {
  }
  | '(' set_target_list ')' '=' select_with_parens {
  }
;

set_target:
  ColId opt_indirection {
    if ($2 == nullptr) {
      $$ = MAKE_NODE(@1, PTQualifiedName, $1);
    } else {
      PTName::SharedPtr name_node = MAKE_NODE(@1, PTName, $1);
      $2->Prepend(name_node);
      $$ = $2;
    }
  }
;

set_target_list:
  set_target {
  }
  | set_target_list ',' set_target {
  }
;

//--------------------------------------------------------------------------------------------------
//  clauses common to all Optimizable Stmts:
//    from_clause   - allow list of both JOIN expressions and table names
//    where_clause  - qualifications for joins or restrictions
//--------------------------------------------------------------------------------------------------

from_clause:
  /* EMPTY */ {
    PARSER_UNSUPPORTED(@0);
    $$ = nullptr;
  }
  | FROM from_list {
    $$ = $2;
  }
;

from_list:
  table_ref {
    $$ = MAKE_NODE(@1, PTTableRefListNode, $1);
  }
  | from_list ',' table_ref {
    $1->Append($3);
    $$ = $1;
  }
;

// table_ref is where an alias clause can be attached.
table_ref:
  relation_expr opt_alias_clause {
    $$ = MAKE_NODE(@1, PTTableRef, $1, $2);
  }
  | relation_expr opt_alias_clause tablesample_clause {
    PARSER_UNSUPPORTED(@3);
  }
  | func_table func_alias_clause {
    PARSER_UNSUPPORTED(@1);
  }
  | LATERAL_P func_table func_alias_clause {
    PARSER_UNSUPPORTED(@1);
  }
  | select_with_parens opt_alias_clause {
    PARSER_UNSUPPORTED(@1);
  }
  | LATERAL_P select_with_parens opt_alias_clause {
    PARSER_UNSUPPORTED(@1);
  }
  | joined_table {
    PARSER_UNSUPPORTED(@1);
  }
  | '(' joined_table ')' alias_clause {
    PARSER_UNSUPPORTED(@1);
  }
;

// It may seem silly to separate joined_table from table_ref, but there is
// method in SQL's madness: if you don't do it this way you get reduce-
// reduce conflicts, because it's not clear to the parser generator whether
// to expect alias_clause after ')' or not.  For the same reason we must
// treat 'JOIN' and 'join_type JOIN' separately, rather than allowing
// join_type to expand to empty; if we try it, the parser generator can't
// figure out when to reduce an empty join_type right after table_ref.
//
// Note that a CROSS JOIN is the same as an unqualified
// INNER JOIN, and an INNER JOIN/ON has the same shape
// but a qualification expression to limit membership.
// A NATURAL JOIN implicitly matches column names between
// tables and the shape is determined by which columns are
// in common. We'll collect columns during the later transformations.

joined_table:
  '(' joined_table ')' {
  }
  | table_ref CROSS JOIN table_ref {
  }
  | table_ref join_type JOIN table_ref join_qual {
  }
  | table_ref JOIN table_ref join_qual {
  }
  | table_ref NATURAL join_type JOIN table_ref {
  }
  | table_ref NATURAL JOIN table_ref {
  }
;

alias_clause:
  AS ColId {
    $$ = $2;
  }
  | AS ColId '(' name_list ')' {
    PARSER_UNSUPPORTED(@1);
  }
  | ColId '(' name_list ')' {
    PARSER_UNSUPPORTED(@1);
  }
  | ColId {
    PARSER_UNSUPPORTED(@1);
  }
;

opt_alias_clause:
  /* EMPTY */ {
    $$ = nullptr;
  }
  | alias_clause {
    $$ = $1;
  }
;

// func_alias_clause can include both an Alias and a coldeflist, so we make it
// return a 2-element list that gets disassembled by calling production.
func_alias_clause:
  /* EMPTY */ {
  }
  | alias_clause {
  }
  | AS '(' TableFuncElementList ')' {
  }
  | AS ColId '(' TableFuncElementList ')' {
  }
  | ColId '(' TableFuncElementList ')' {
  }
;

join_type:
  FULL join_outer             { $$ = JOIN_FULL; }
  | LEFT join_outer           { $$ = JOIN_LEFT; }
  | RIGHT join_outer          { $$ = JOIN_RIGHT; }
  | INNER_P                   { $$ = JOIN_INNER; }
;

/* OUTER is just noise... */
join_outer:
  /* EMPTY */ {
    $$ = nullptr;
  }
  | OUTER_P {
    $$ = nullptr;
  }
;

// JOIN qualification clauses
// Possibilities are:
//  USING ( column list ) allows only unqualified column names,
//              which must match between tables.
//  ON expr allows more general qualifications.
//
// We return USING as a List node, while an ON-expr will not be a List.

join_qual:
  USING '(' name_list ')' {
  }
  | ON a_expr {
  }
;

relation_expr:
  qualified_name {
    $$ = $1;
  }
  | qualified_name '*' {
    PARSER_UNSUPPORTED(@2);
  }
  | ONLY qualified_name {
    PARSER_UNSUPPORTED(@1);
  }
  | ONLY '(' qualified_name ')' {
    PARSER_UNSUPPORTED(@1);
  }
;

relation_expr_list:
  relation_expr {
    $$ = MAKE_NODE(@1, PTQualifiedNameListNode, $1);
  }
  | relation_expr_list ',' relation_expr {
    $1->Append($3);
    $$ = $1;
  }
;

// Given "UPDATE foo set set ...", we have to decide without looking any
// further ahead whether the first "set" is an alias or the UPDATE's SET
// keyword.  Since "set" is allowed as a column name both interpretations
// are feasible.  We resolve the shift/reduce conflict by giving the first
// relation_expr_opt_alias production a higher precedence than the SET token
// has, causing the parser to prefer to reduce, in effect assuming that the
// SET is not an alias.
relation_expr_opt_alias:
  relation_expr %prec UMINUS  {
    $$ = MAKE_NODE(@1, PTTableRef, $1, nullptr);
  }
  | relation_expr AS ColId {
    $$ = MAKE_NODE(@1, PTTableRef, $1, $3);
  }
  | relation_expr ColId {
    PARSER_UNSUPPORTED(@2);
  }
;

// TABLESAMPLE decoration in a FROM item
tablesample_clause:
  TABLESAMPLE func_name '(' expr_list ')' opt_repeatable_clause {
  }
;

opt_repeatable_clause:
  REPEATABLE '(' a_expr ')' {
  }
  | /*EMPTY*/ {
    $$ = nullptr;
  }
;

// func_table represents a function invocation in a FROM list. It can be
// a plain function call, like "foo(...)", or a ROWS FROM expression with
// one or more function calls, "ROWS FROM (foo(...), bar(...))",
// optionally with WITH ORDINALITY attached.
// In the ROWS FROM syntax, a column definition list can be given for each
// function, for example:
//     ROWS FROM (foo() AS (foo_res_a text, foo_res_b text),
//                bar() AS (bar_res_a text, bar_res_b text))
// It's also possible to attach a column definition list to the RangeFunction
// as a whole, but that's handled by the table_ref production.
func_table:
  func_expr_windowless opt_ordinality {
  }
  | ROWS FROM '(' rowsfrom_list ')' opt_ordinality {
  }
;

rowsfrom_item:
  func_expr_windowless opt_col_def_list {
  }
;

rowsfrom_list:
  rowsfrom_item {
  }
  | rowsfrom_list ',' rowsfrom_item {
  }
;

opt_col_def_list:
  AS '(' TableFuncElementList ')' {
  }
  | /*EMPTY*/ {
  }
;

opt_else_clause:
  ELSE ERROR                    { $$ = true; }
  | /*EMPTY*/                   { $$ = false; }
;

opt_ordinality:
  WITH_LA ORDINALITY            { $$ = true; }
  | /*EMPTY*/                   { $$ = false; }
;

opt_where_clause:
  /*EMPTY*/                     { $$ = nullptr; }
  | where_clause                { $$ = $1; }
;

where_clause:
  WHERE a_expr                  { $$ = $2; }
;

if_clause:
  IF_P a_expr                   { $$ = $2; }
;

opt_if_clause:
  /*EMPTY*/                     { $$ = nullptr; }
  | if_clause                   { $$ = $1; }
;

/* variant for UPDATE and DELETE */
opt_where_or_current_clause:
  /*EMPTY*/                     { $$ = nullptr; }
  | where_or_current_clause     { $$ = $1; }
;

where_or_current_clause:
  WHERE a_expr {
    $$ = $2;
  }
  | WHERE CURRENT_P OF cursor_name {
    PARSER_UNSUPPORTED(@2);
  }
;

TableFuncElementList:
  TableFuncElement {
  }
  | TableFuncElementList ',' TableFuncElement {
  }
;

TableFuncElement:
  ColId Typename opt_collate_clause {
  }
;

//--------------------------------------------------------------------------------------------------
//  expression grammar
//--------------------------------------------------------------------------------------------------

// General expressions
// This is the heart of the expression syntax.
//
// We have two expression types: a_expr is the unrestricted kind, and
// b_expr is a subset that must be used in some places to avoid shift/reduce
// conflicts.  For example, we can't do BETWEEN as "BETWEEN a_expr AND a_expr"
// because that use of AND conflicts with AND as a boolean operator.  So,
// b_expr is used in BETWEEN and we remove boolean keywords from b_expr.
//
// Note that '(' a_expr ')' is a b_expr, so an unrestricted expression can
// always be used by surrounding it with parens.
//
// c_expr is all the productions that are common to a_expr and b_expr;
// it's factored out just to eliminate redundant coding.
//
// Be careful of productions involving more than one terminal token.
// By default, bison will assign such productions the precedence of their
// last terminal, but in nearly all cases you want it to be the precedence
// of the first terminal instead; otherwise you will not get the behavior
// you expect!  So we use %prec annotations freely to set precedences.
a_expr:
  c_expr {
    $$ = $1;
  }

  // These operators must be called out explicitly in order to make use
  // of bison's automatic operator-precedence handling.  All other
  // operator names are handled by the generic productions using "Op",
  // below; and all those operators will have the same precedence.
  //
  // If you add more explicitly-known operators, be sure to add them
  // also to b_expr and to the MathOp list below.
  | '+' a_expr                                                 %prec UMINUS {
    $$ = $2;
  }
  | '-' a_expr                                                 %prec UMINUS {
    $$ = MAKE_NODE(@1, PTOperator1, ExprOperator::kUMinus, QL_OP_NOOP, $2);
  }
  | columnref '[' a_expr ']' {
    PTExprListNode::SharedPtr args = MAKE_NODE(@1, PTExprListNode, $3);
    $$ = MAKE_NODE(@1, PTSubscriptedColumn, $1->name(), args);
  }
  | columnref json_ref {
    $$ = MAKE_NODE(@1, PTJsonColumnWithOperators, $1->name(), $2);
  }
  // Logical expression.
  | NOT a_expr {
    $$ = MAKE_NODE(@1, PTLogic1, ExprOperator::kLogic1, QL_OP_NOT, $2);
  }
  | a_expr IS TRUE_P                                           %prec IS {
    $$ = MAKE_NODE(@1, PTLogic1, ExprOperator::kLogic1, QL_OP_IS_TRUE, $1);
  }
  | a_expr IS NOT TRUE_P                                       %prec IS {
    $$ = MAKE_NODE(@1, PTLogic1, ExprOperator::kLogic1, QL_OP_IS_FALSE, $1);
  }
  | a_expr IS FALSE_P                                          %prec IS {
    $$ = MAKE_NODE(@1, PTLogic1, ExprOperator::kLogic1, QL_OP_IS_FALSE, $1);
  }
  | a_expr IS NOT FALSE_P                                      %prec IS {
    $$ = MAKE_NODE(@1, PTLogic1, ExprOperator::kLogic1, QL_OP_IS_TRUE, $1);
  }
  | a_expr AND a_expr {
    $$ = MAKE_NODE(@1, PTLogic2, ExprOperator::kLogic2, QL_OP_AND, $1, $3);
  }
  | a_expr OR a_expr {
    $$ = MAKE_NODE(@1, PTLogic2, ExprOperator::kLogic2, QL_OP_OR, $1, $3);
  }

  // Relations that have no operand.
  | EXISTS {
    $$ = MAKE_NODE(@1, PTRelation0, ExprOperator::kRelation0, QL_OP_EXISTS);
  }
  | NOT_LA EXISTS {
    $$ = MAKE_NODE(@1, PTRelation0, ExprOperator::kRelation0, QL_OP_NOT_EXISTS);
  }

  // Relations that have one operand.
  | a_expr IS NULL_P                                           %prec IS {
    $$ = MAKE_NODE(@1, PTRelation1, ExprOperator::kRelation1, QL_OP_IS_NULL, $1);
  }
  | a_expr ISNULL {
    $$ = MAKE_NODE(@1, PTRelation1, ExprOperator::kRelation1, QL_OP_IS_NULL, $1);
  }
  | a_expr IS NOT NULL_P                                       %prec IS {
    $$ = MAKE_NODE(@1, PTRelation1, ExprOperator::kRelation1, QL_OP_IS_NOT_NULL, $1);
  }
  | a_expr NOTNULL {
    $$ = MAKE_NODE(@1, PTRelation1, ExprOperator::kRelation1, QL_OP_IS_NOT_NULL, $1);
  }

  // Relations that have two operands.
  | a_expr '=' a_expr {
    $$ = MAKE_NODE(@1, PTRelation2, ExprOperator::kRelation2, QL_OP_EQUAL, $1, $3);
  }
  | a_expr '<' a_expr {
    $$ = MAKE_NODE(@1, PTRelation2, ExprOperator::kRelation2, QL_OP_LESS_THAN, $1, $3);
  }
  | a_expr '>' a_expr {
    $$ = MAKE_NODE(@1, PTRelation2, ExprOperator::kRelation2, QL_OP_GREATER_THAN, $1, $3);
  }
  | a_expr LESS_EQUALS a_expr {
    $$ = MAKE_NODE(@1, PTRelation2, ExprOperator::kRelation2, QL_OP_LESS_THAN_EQUAL, $1, $3);
  }
  | a_expr GREATER_EQUALS a_expr {
    $$ = MAKE_NODE(@1, PTRelation2, ExprOperator::kRelation2, QL_OP_GREATER_THAN_EQUAL, $1, $3);
  }
  | a_expr NOT_EQUALS a_expr {
    $$ = MAKE_NODE(@1, PTRelation2, ExprOperator::kRelation2, QL_OP_NOT_EQUAL, $1, $3);
  }
  | a_expr LIKE a_expr {
    PARSER_CQL_INVALID(@2);
  }
  | a_expr NOT_LA LIKE a_expr                                  %prec NOT_LA {
    PARSER_CQL_INVALID(@3);
  }

  // Relations that have 3 operands.
  | a_expr BETWEEN opt_asymmetric b_expr AND a_expr            %prec BETWEEN {
    $$ = MAKE_NODE(@1, PTRelation3, ExprOperator::kRelation3, QL_OP_BETWEEN, $1, $4, $6);
  }
  | a_expr NOT_LA BETWEEN opt_asymmetric b_expr AND a_expr     %prec NOT_LA {
    $$ = MAKE_NODE(@1, PTRelation3, ExprOperator::kRelation3, QL_OP_NOT_BETWEEN, $1, $5, $7);
  }

  // Predicates that have variable number of operands.
  | a_expr IN_P in_expr {
    $$ = MAKE_NODE(@1, PTRelation2, ExprOperator::kRelation2, QL_OP_IN, $1, $3);
  }
  | a_expr NOT_LA IN_P in_expr                                 %prec NOT_LA {
    $$ = MAKE_NODE(@1, PTRelation2, ExprOperator::kRelation2, QL_OP_NOT_IN, $1, $4);
  }
  | collection_expr {
    $$ = $1;
  }
  | a_expr '+' a_expr {
    PTExprListNode::SharedPtr args = MAKE_NODE(@1, PTExprListNode, $1);
    args->Append($3);
    auto name = parser_->MakeString("+");
    $$ = MAKE_NODE(@2, PTBcall, name, args);
  }
  | a_expr '-' a_expr {
    PTExprListNode::SharedPtr args = MAKE_NODE(@1, PTExprListNode, $1);
    args->Append($3);
    auto name = parser_->MakeString("-");
    $$ = MAKE_NODE(@2, PTBcall, name, args);
  }
  | inactive_a_expr {
    PARSER_CQL_INVALID(@1);
  }
;

inactive_a_expr:
  a_expr TYPECAST Typename {
  }
  | a_expr COLLATE any_name {
  }
  | a_expr AT TIME ZONE a_expr                                 %prec AT {
  }
  // These operators must be called out explicitly in order to make use
  // of bison's automatic operator-precedence handling.  All other
  // operator names are handled by the generic productions using "Op",
  // below; and all those operators will have the same precedence.
  //
  // If you add more explicitly-known operators, be sure to add them
  // also to b_expr and to the MathOp list below.
  | a_expr '*' a_expr {
    PARSER_CQL_INVALID(@2);
  }
  | a_expr '/' a_expr {
    PARSER_CQL_INVALID(@2);
  }
  | a_expr '%' a_expr {
    PARSER_CQL_INVALID(@2);
  }
  | a_expr '^' a_expr {
    PARSER_CQL_INVALID(@2);
  }
  | a_expr qual_Op a_expr                                      %prec Op {
  }
  | qual_Op a_expr                                             %prec Op {
  }
  | a_expr qual_Op                                             %prec POSTFIXOP {
  }
  | a_expr LIKE a_expr ESCAPE a_expr                           %prec LIKE {
  }
  | a_expr NOT_LA LIKE a_expr ESCAPE a_expr                    %prec NOT_LA {
  }
  | a_expr ILIKE a_expr {
  }
  | a_expr ILIKE a_expr ESCAPE a_expr                          %prec ILIKE {
  }
  | a_expr NOT_LA ILIKE a_expr                                 %prec NOT_LA {
  }
  | a_expr NOT_LA ILIKE a_expr ESCAPE a_expr                   %prec NOT_LA {
  }
  | a_expr SIMILAR TO a_expr                                   %prec SIMILAR {
  }
  | a_expr SIMILAR TO a_expr ESCAPE a_expr                     %prec SIMILAR {
  }
  | a_expr NOT_LA SIMILAR TO a_expr                            %prec NOT_LA {
  }
  | a_expr NOT_LA SIMILAR TO a_expr ESCAPE a_expr              %prec NOT_LA {
  }
  | row OVERLAPS row {
  }
  | a_expr IS UNKNOWN                                          %prec IS {
  }
  | a_expr IS DISTINCT FROM a_expr                             %prec IS {
  }
  | a_expr IS NOT DISTINCT FROM a_expr                         %prec IS {
  }
  | a_expr IS OF '(' type_list ')'                             %prec IS {
  }
  | a_expr IS NOT OF '(' type_list ')'                         %prec IS {
  }
  | a_expr BETWEEN SYMMETRIC b_expr AND a_expr                 %prec BETWEEN {
  }
  | a_expr NOT_LA BETWEEN SYMMETRIC b_expr AND a_expr          %prec NOT_LA {
  }
  | a_expr subquery_Op sub_type select_with_parens             %prec Op {
  }
  | a_expr subquery_Op sub_type '(' a_expr ')'                 %prec Op {
  }
  | UNIQUE select_with_parens {
  }
  | a_expr IS DOCUMENT_P                                       %prec IS {
  }
  | a_expr IS NOT DOCUMENT_P                                   %prec IS {
  }
;

// Restricted expressions
//
// b_expr is a subset of the complete expression syntax defined by a_expr.
//
// Presently, AND, NOT, IS, and IN are the a_expr keywords that would
// cause trouble in the places where b_expr is used.  For simplicity, we
// just eliminate all the boolean-keyword-operator productions from b_expr.
b_expr:
  c_expr {
    $$ = $1;
  }
  | b_expr TYPECAST Typename {
  }
  | '+' b_expr                                                 %prec UMINUS {
    $$ = $2;
  }
  | '-' b_expr                                                 %prec UMINUS {
    $$ = MAKE_NODE(@1, PTOperator1, ExprOperator::kUMinus, QL_OP_NOOP, $2);
  }
  | b_expr '+' b_expr {
  }
  | b_expr '-' b_expr {
  }
  | b_expr '*' b_expr {
  }
  | b_expr '/' b_expr {
  }
  | b_expr '%' b_expr {
  }
  | b_expr '^' b_expr {
  }
  | b_expr '<' b_expr {
  }
  | b_expr '>' b_expr {
  }
  | b_expr '=' b_expr {
  }
  | b_expr LESS_EQUALS b_expr {
  }
  | b_expr GREATER_EQUALS b_expr {
  }
  | b_expr NOT_EQUALS b_expr {
  }
  | b_expr qual_Op b_expr                                      %prec Op {
  }
  | qual_Op b_expr                                             %prec Op {
  }
  | b_expr qual_Op                                             %prec POSTFIXOP {
  }
  | b_expr IS DISTINCT FROM b_expr                             %prec IS {
  }
  | b_expr IS NOT DISTINCT FROM b_expr                         %prec IS {
  }
  | b_expr IS OF '(' type_list ')'                             %prec IS {
  }
  | b_expr IS NOT OF '(' type_list ')'                         %prec IS {
  }
  | b_expr IS DOCUMENT_P                                       %prec IS {
  }
  | b_expr IS NOT DOCUMENT_P                                   %prec IS {
  }
;

// Productions that can be used in both a_expr and b_expr.
//
// Note: productions that refer recursively to a_expr or b_expr mostly
// cannot appear here.  However, it's OK to refer to a_exprs that occur
// inside parentheses, such as function arguments; that cannot introduce
// ambiguity to the b_expr syntax.
c_expr:
  columnref {
    $$ = $1;
  }
  | bindvar {
    if ($1 != nullptr) {
      parser_->AddBindVariable(static_cast<PTBindVar*>($1.get()));
    }
    $$ = $1;
  }
  | AexprConst {
    $$ = $1;
  }
  | '(' a_expr ')' opt_indirection {
    if ($4) {
      PARSER_UNSUPPORTED(@1);
    } else {
      $$ = $2;
    }
  }
  | func_expr {
    $$ = $1;
  }
  | inactive_c_expr {
    PARSER_UNSUPPORTED(@1);
  }
;

inactive_c_expr:
  PARAM opt_indirection {
  }
  | select_with_parens      %prec UMINUS {
  }
  | select_with_parens indirection {
  }
  | EXISTS select_with_parens {
  }
  | ARRAY select_with_parens {
  }
  | explicit_row {
  }
  | implicit_row {
  }
  | GROUPING '(' expr_list ')' {
  }
;

// func_expr and its cousin func_expr_windowless are split out from c_expr just
// so that we have classifications for "everything that is a function call or
// looks like one".  This isn't very important, but it saves us having to
// document which variants are legal in places like "FROM function()" or the
// backwards-compatible functional-index syntax for CREATE INDEX.
// (Note that many of the special SQL functions wouldn't actually make any
// sense as functional index entries, but we ignore that consideration here.)
func_expr:
  func_application within_group_clause filter_clause over_clause {
    // All optional clause are not used, for which we raise error at their definitions.
    $$ = $1;
  }
  | func_expr_common_subexpr {
    PARSER_UNSUPPORTED(@1);
    $$ = nullptr;
  }
;

func_application:
  func_name '(' ')' {
    PTExprListNode::SharedPtr args = MAKE_NODE(@1, PTExprListNode);
    $$ = MAKE_NODE(@1, PTBcall, $1, args);
  }
  | func_name '(' func_arg_list opt_sort_clause ')' {
    if ($4 != nullptr) {
      PARSER_UNSUPPORTED(@1);
    }
    $$ = MAKE_NODE(@1, PTBcall, $1, $3);
  }
  // special treatment for token and partition_hash because it is a reserved keyword and produces
  // a dedicated C++ class.
  | TOKEN '(' ')' {
    PTExprListNode::SharedPtr args = MAKE_NODE(@1, PTExprListNode);
    auto name = parser_->MakeString($1);
    $$ = MAKE_NODE(@1, PTToken, name, args);
  }
  | TOKEN '(' func_arg_list opt_sort_clause ')' {
    if ($4 != nullptr) {
      PARSER_UNSUPPORTED(@1);
    }
    auto name = parser_->MakeString($1);
    $$ = MAKE_NODE(@1, PTToken, name, $3);
  }
  | PARTITION_HASH '(' ')' {
    PTExprListNode::SharedPtr args = MAKE_NODE(@1, PTExprListNode);
    auto name = parser_->MakeString($1);
    $$ = MAKE_NODE(@1, PTPartitionHash, name, args);
  }
  | PARTITION_HASH '(' func_arg_list opt_sort_clause ')' {
    if ($4 != nullptr) {
      PARSER_UNSUPPORTED(@1);
    }
    auto name = parser_->MakeString($1);
    $$ = MAKE_NODE(@1, PTPartitionHash, name, $3);
  }
  | CAST '(' a_expr AS Typename ')' {
    if ($5->ql_type() && !$5->ql_type()->IsParametric()) {
      PTExprListNode::SharedPtr args = MAKE_NODE(@1, PTExprListNode);
      args->Append($3);
      args->Append(PTExpr::CreateConst(PTREE_MEM, PTREE_LOC(@5), $5));
      auto name = parser_->MakeString(bfql::kCqlCastFuncName);
      $$ = MAKE_NODE(@1, PTBcall, name, args);
    } else {
      PARSER_CQL_INVALID_MSG(@5, "Unsupported cast type");
    }
  }
  | func_name '(' VARIADIC func_arg_expr opt_sort_clause ')' {
    PARSER_UNSUPPORTED(@1);
  }
  | func_name '(' func_arg_list ',' VARIADIC func_arg_expr opt_sort_clause ')' {
    PARSER_UNSUPPORTED(@1);
  }
  | func_name '(' ALL func_arg_list opt_sort_clause ')' {
    PARSER_UNSUPPORTED(@1);
  }
  | func_name '(' DISTINCT func_arg_list opt_sort_clause ')' {
    PARSER_UNSUPPORTED(@1);
  }
  | func_name '(' '*' ')' {
    if (*$1 == "count") {
      PTExpr::SharedPtr arg = MAKE_NODE(@3, PTStar);
      PTExprListNode::SharedPtr args = MAKE_NODE(@2, PTExprListNode, arg);
      $$ = MAKE_NODE(@1, PTBcall, $1, args);
    } else {
      PARSER_INVALID(@1);
    }
  }
;

// Special expressions that are considered to be functions.
func_expr_common_subexpr:
  COLLATION FOR '(' a_expr ')' {
  }
  | CURRENT_DATE {
  }
  | CURRENT_TIME {
  }
  | CURRENT_TIME '(' Iconst ')' {
  }
  | CURRENT_TIMESTAMP {
  }
  | CURRENT_TIMESTAMP '(' Iconst ')' {
  }
  | LOCALTIME {
  }
  | LOCALTIME '(' Iconst ')' {
  }
  | LOCALTIMESTAMP {
  }
  | LOCALTIMESTAMP '(' Iconst ')' {
  }
  | CURRENT_ROLE {
  }
  | CURRENT_USER {
  }
  | SESSION_USER {
  }
  | CURRENT_CATALOG {
  }
  | CURRENT_SCHEMA {
  }
  | EXTRACT '(' extract_list ')' {
  }
  | OVERLAY '(' overlay_list ')' {
  }
  | POSITION '(' position_list ')' {
  }
  | SUBSTRING '(' substr_list ')' {
  }
  | TREAT '(' a_expr AS Typename ')' {
  }
  | TRIM '(' BOTH trim_list ')' {
  }
  | TRIM '(' LEADING trim_list ')' {
  }
  | TRIM '(' TRAILING trim_list ')' {
  }
  | TRIM '(' trim_list ')' {
  }
  | NULLIF '(' a_expr ',' a_expr ')' {
  }
  | COALESCE '(' expr_list ')' {
  }
  | GREATEST '(' expr_list ')' {
  }
  | LEAST '(' expr_list ')' {
  }
  | XMLCONCAT '(' expr_list ')' {
  }
  | XMLELEMENT '(' NAME_P ColLabel ')' {
  }
  | XMLELEMENT '(' NAME_P ColLabel ',' xml_attributes ')' {
  }
  | XMLELEMENT '(' NAME_P ColLabel ',' expr_list ')' {
  }
  | XMLELEMENT '(' NAME_P ColLabel ',' xml_attributes ',' expr_list ')' {
  }
  | XMLEXISTS '(' c_expr xmlexists_argument ')' {
  }
  | XMLFOREST '(' xml_attribute_list ')' {
  }
  | XMLPARSE '(' document_or_content a_expr xml_whitespace_option ')' {
  }
  | XMLPI '(' NAME_P ColLabel ')' {
  }
  | XMLPI '(' NAME_P ColLabel ',' a_expr ')' {
  }
  | XMLROOT '(' a_expr ',' xml_root_version opt_xml_root_standalone ')' {
  }
  | XMLSERIALIZE '(' document_or_content a_expr AS SimpleTypename ')' {
  }
;

// As func_expr but does not accept WINDOW functions directly
// (but they can still be contained in arguments for functions etc).
// Use this when window expressions are not allowed, where needed to
// disambiguate the grammar (e.g. in CREATE INDEX).
func_expr_windowless:
  func_application {
    $$ = nullptr;
  }
  | func_expr_common_subexpr {
    $$ = nullptr;
  }
;

// SQL/XML support
xml_root_version:
  VERSION_P a_expr {
    $$ = nullptr;
  }
  | VERSION_P NO VALUE_P {
    $$ = nullptr;
  }
;

opt_xml_root_standalone:
  ',' STANDALONE_P YES_P {
  }
  | ',' STANDALONE_P NO {
  }
  | ',' STANDALONE_P NO VALUE_P {
  }
  | /*EMPTY*/ {
  }
;

xml_attributes:
  XMLATTRIBUTES '(' xml_attribute_list ')' {
  }
;

xml_attribute_list:
  xml_attribute_el {
  }
  | xml_attribute_list ',' xml_attribute_el {
  }
;

xml_attribute_el:
  a_expr AS ColLabel {
  }
  | a_expr {
  }
;

document_or_content:
  DOCUMENT_P                { }
  | CONTENT_P               { }
;

xml_whitespace_option:
  PRESERVE WHITESPACE_P     { $$ = true; }
  | STRIP_P WHITESPACE_P    { $$ = false; }
  | /*EMPTY*/               { $$ = false; }
;

// We allow several variants for SQL and other compatibility.
xmlexists_argument:
  PASSING c_expr {
    $$ = nullptr;
  }
  | PASSING c_expr BY REF {
    $$ = nullptr;
  }
  | PASSING BY REF c_expr {
    $$ = nullptr;
  }
  | PASSING BY REF c_expr BY REF {
    $$ = nullptr;
  }
;

// Aggregate decoration clauses
within_group_clause:
  /*EMPTY*/ {
    $$ = nullptr;
  }
;

filter_clause:
  /*EMPTY*/ {
    $$ = nullptr;
  }
  | FILTER '(' WHERE a_expr ')' {
    PARSER_UNSUPPORTED(@1);
    $$ = nullptr;
  }
;

// Window Definitions
opt_window_clause:
  /*EMPTY*/ {
  }
  | WINDOW window_definition_list {
    PARSER_UNSUPPORTED(@1);
  }
;

window_definition_list:
  window_definition {
  }
  | window_definition_list ',' window_definition {
  }
;

window_definition:
  ColId AS window_specification {
  }
;

over_clause:
  /*EMPTY*/ {
    $$ = nullptr;
  }
  | OVER window_specification {
    PARSER_UNSUPPORTED(@1);
  }
  | OVER ColId {
    PARSER_UNSUPPORTED(@1);
  }
;

window_specification:
  '(' opt_existing_window_name opt_partition_clause
  opt_sort_clause opt_frame_clause ')' {
    PARSER_UNSUPPORTED(@1);
  }
;

// If we see PARTITION, RANGE, or ROWS as the first token after the '('
// of a window_specification, we want the assumption to be that there is
// no existing_window_name; but those keywords are unreserved and so could
// be ColIds.  We fix this by making them have the same precedence as IDENT
// and giving the empty production here a slightly higher precedence, so
// that the shift/reduce conflict is resolved in favor of reducing the rule.
// These keywords are thus precluded from being an existing_window_name but
// are not reserved for any other purpose.
opt_existing_window_name:
  ColId {
    $$ = $1;
  }
  | /*EMPTY*/       %prec Op {
    $$ = nullptr;
  }
;

opt_partition_clause:
  PARTITION BY expr_list {
    $$ = $3;
  }
  | /*EMPTY*/ {
  }
;

// For frame clauses, we return a WindowDef, but only some fields are used:
// frameOptions, startOffset, and endOffset.
//
// This is only a subset of the full SQL:2008 frame_clause grammar.
// We don't support <window frame exclusion> yet.
opt_frame_clause:
  RANGE frame_extent {
  }
  | ROWS frame_extent {
  }
  | /*EMPTY*/ {
  }
;

frame_extent:
  frame_bound {
  }
  | BETWEEN frame_bound AND frame_bound {
  }
;

// This is used for both frame start and frame end, with output set up on
// the assumption it's frame start; the frame_extent productions must reject
// invalid cases.
frame_bound:
  UNBOUNDED PRECEDING {
  }
  | UNBOUNDED FOLLOWING {
  }
  | CURRENT_P ROW {
  }
  | a_expr PRECEDING {
  }
  | a_expr FOLLOWING {
  }
;

// Supporting nonterminals for expressions.

// Explicit row production.
// SQL99 allows an optional ROW keyword, so we can now do single-element rows
// without conflicting with the parenthesized a_expr production.  Without the
// ROW keyword, there must be more than one a_expr inside the parens.
row:
  ROW '(' expr_list ')' {
    $$ = $3;
  }
  | ROW '(' ')' {
  }
  | '(' expr_list ',' a_expr ')' {
  }
;

explicit_row:
  ROW '(' expr_list ')' {
    $$ = $3;
  }
  | ROW '(' ')' {
  }
;

implicit_row:
  '(' expr_list ',' a_expr ')' {
  }
;

sub_type:
  ANY                       { }
  | SOME                    { }
  | ALL                     { }
;

all_Op:
  Op                        { $$ = $1->c_str(); }
  | MathOp                  { $$ = $1; }
;

MathOp:
  '+'                       { $$ = "+"; }
  | '-'                     { $$ = "-"; }
  | '*'                     { $$ = "*"; }
  | '/'                     { $$ = "/"; }
  | '%'                     { $$ = "%"; }
  | '^'                     { $$ = "^"; }
  | '<'                     { $$ = "<"; }
  | '>'                     { $$ = ">"; }
  | '='                     { $$ = "="; }
  | LESS_EQUALS             { $$ = "<="; }
  | GREATER_EQUALS          { $$ = ">="; }
  | NOT_EQUALS              { $$ = "<>"; }
;

qual_Op:
  Op {
  }
  | OPERATOR '(' any_operator ')' {
  }
;

qual_all_Op:
  all_Op {
  }
  | OPERATOR '(' any_operator ')' {
  }
;

subquery_Op:
  all_Op {
  }
  | OPERATOR '(' any_operator ')' {
  }
  | LIKE {
  }
  | NOT_LA LIKE {
  }
  | ILIKE {
  }
  | NOT_LA ILIKE {
  }

  // cannot put SIMILAR TO here, because SIMILAR TO is a hack.
  // the regular expression is preprocessed by a function (similar_escape),
  // and the ~ operator for posix regular expressions is used.
  //        x SIMILAR TO y     ->    x ~ similar_escape(y)
  // this transformation is made on the fly by the parser upwards.
  // however the SubLink structure which handles any/some/all stuff
  // is not ready for such a thing.
;

expr_list:
  a_expr {
  }
  | expr_list ',' a_expr {
  }
;

// function arguments can have names.
func_arg_list:
  func_arg_expr {
    $$ = MAKE_NODE(@1, PTExprListNode, $1);
  }
  | func_arg_list ',' func_arg_expr {
    $1->Append($3);
    $$ = $1;
  }
;

func_arg_expr:
  a_expr {
    $$ = $1;
  }
  | param_name COLON_EQUALS a_expr {
    PARSER_UNSUPPORTED(@2);
  }
  | param_name EQUALS_GREATER a_expr {
    PARSER_UNSUPPORTED(@2);
  }
;

type_list:
  Typename {
  }
  | type_list ',' Typename {
  }
;

extract_list:
  extract_arg FROM a_expr {
  }
  | /*EMPTY*/ {
  }
;

// Allow delimited string Sconst in extract_arg as an SQL extension.
// - thomas 2001-04-12
extract_arg:
  IDENT                   { $$ = $1->c_str(); }
  | YEAR_P                { $$ = "year"; }
  | MONTH_P               { $$ = "month"; }
  | DAY_P                 { $$ = "day"; }
  | HOUR_P                { $$ = "hour"; }
  | MINUTE_P              { $$ = "minute"; }
  | SECOND_P              { $$ = "second"; }
  | Sconst                { $$ = $1->c_str(); }
;

// OVERLAY() arguments
// SQL99 defines the OVERLAY() function:
// o overlay(text placing text from int for int)
// o overlay(text placing text from int)
// and similarly for binary strings
overlay_list:
  a_expr overlay_placing substr_from substr_for {
  }
  | a_expr overlay_placing substr_from {
  }
;

overlay_placing:
  PLACING a_expr {
  }
;

// position_list uses b_expr not a_expr to avoid conflict with general IN.

position_list:
  b_expr IN_P b_expr {
  }
  | /*EMPTY*/ {
  }
;

// SUBSTRING() arguments
// SQL9x defines a specific syntax for arguments to SUBSTRING():
// o substring(text from int for int)
// o substring(text from int) get entire string from starting point "int"
// o substring(text for int) get first "int" characters of string
// o substring(text from pattern) get entire string matching pattern
// o substring(text from pattern for escape) same with specified escape char
// We also want to support generic substring functions which accept
// the usual generic list of arguments. So we will accept both styles
// here, and convert the SQL9x style to the generic list for further
// processing. - thomas 2000-11-28
substr_list:
  a_expr substr_from substr_for {
  }
  | a_expr substr_for substr_from {
  }
  | a_expr substr_from {
  }
  | a_expr substr_for {
  }
  | expr_list {
  }
  | /*EMPTY*/ {
  }
;

substr_from:
  FROM a_expr {
  }
;

substr_for:
  FOR a_expr {
  }
;

trim_list:
  a_expr FROM expr_list {
  }
  | FROM expr_list {
  }
  | expr_list {
  }
;

bindvar:
  '?' {
    $$ = MAKE_NODE(@1, PTBindVar);
  }
  | ':' NonReservedWord {
    $$ = MAKE_NODE(@1, PTBindVar, $2);
  }
  | ':' ICONST {
    PTConstVarInt::SharedPtr pt_constvarint = MAKE_NODE(@1, PTConstVarInt, $2);
    $$ = MAKE_NODE(@1, PTBindVar, pt_constvarint);
  }
;

columnref:
  ColId {
    PTQualifiedName::SharedPtr name_node = MAKE_NODE(@1, PTQualifiedName, $1);
    $$ = MAKE_NODE(@1, PTRef, name_node);
  }
  | ColId indirection {
    PTName::SharedPtr name_node = MAKE_NODE(@1, PTName, $1);
    $2->Prepend(name_node);
    $$ = MAKE_NODE(@1, PTRef, $2);
  }
;

json_ref:
  json_ref_single_arrow {
    $$ = $1;
  }
  | DOUBLE_ARROW AexprConst {
    PTJsonOperator::SharedPtr node = MAKE_NODE(@1, PTJsonOperator, JsonOperator::JSON_TEXT, $2);
    $$ = MAKE_NODE(@1, PTExprListNode, node);
  }
;

json_ref_single_arrow:
  SINGLE_ARROW AexprConst {
    PTJsonOperator::SharedPtr node = MAKE_NODE(@1, PTJsonOperator, JsonOperator::JSON_OBJECT, $2);
    $$ = MAKE_NODE(@1, PTExprListNode, node);
  }
  | SINGLE_ARROW AexprConst json_ref {
    PTJsonOperator::SharedPtr json_op = MAKE_NODE(@1, PTJsonOperator, JsonOperator::JSON_OBJECT,
      $2);
    $3->Prepend(json_op);
    $$ = $3;
  }
;

indirection_el:
  '.' attr_name {
    $$ = MAKE_NODE(@1, PTName, $2);
  }
  | '.' '*' {
    $$ = MAKE_NODE(@1, PTNameAll);
  }
;

indirection:
  indirection_el {
    $$ = MAKE_NODE(@1, PTQualifiedName, $1);
  }
  | indirection indirection_el {
    if ($1 == nullptr) {
      $$ = MAKE_NODE(@1, PTQualifiedName, $2);
    } else {
      $1->Append($2);
      $$ = $1;
    }
  }
;

opt_asymmetric:
  /* EMPTY */
  | ASYMMETRIC
;

// We should allow ROW '(' ctext_expr_list ')' too, but that seems to require
// making VALUES a fully reserved word, which will probably break more apps
// than allowing the noise-word is worth.
ctext_row:
  '(' ctext_expr_list ')' {
    $$ = $2;
  }
;

// The SQL spec defines "contextually typed value expressions" and
// "contextually typed row value constructors", which for our purposes
// are the same as "a_expr" and "row" except that DEFAULT can appear at
// the top level.
ctext_expr_list:
  ctext_expr {
    $$ = MAKE_NODE(@1, PTExprListNode, $1);
  }
  | ctext_expr_list ',' ctext_expr {
    $1->Append($3);
    $$ = $1;
  }
;

ctext_expr:
  a_expr {
    $$ = $1;
  }
  | DEFAULT {
    PARSER_UNSUPPORTED(@1);
  }
;

//--------------------------------------------------------------------------------------------------
// Names and constants
//--------------------------------------------------------------------------------------------------
qualified_name_list:
  qualified_name {
    $$ = nullptr;
  }
  | qualified_name_list ',' qualified_name {
    $$ = nullptr;
  }
;

// The production for a qualified relation name has to exactly match the
// production for a qualified func_name, because in a FROM clause we cannot
// tell which we are parsing until we see what comes after it ('(' for a
// func_name, something else for a relation). Therefore we allow 'indirection'
// which may contain subscripts, and reject that case in the C code.
qualified_name:
  ColId {
    $$ = MAKE_NODE(@1, PTQualifiedName, $1);
  }
  | ColId indirection {
    PTName::SharedPtr name_node = MAKE_NODE(@1, PTName, $1);
    $2->Prepend(name_node);
    $$ = $2;
  }
;

name_list:
  name {
    $$ = nullptr;
  }
  | name_list ',' name {
    $$ = nullptr;
  }
;

name:          ColId            { $$ = $1; };

database_name: ColId            { $$ = $1; };

access_method: ColId            { $$ = $1; };

attr_name:     ColLabel         { $$ = $1; };

index_name:    ColId            { $$ = $1; };

file_name:     Sconst           { $$ = $1; };

property_name: name             { $$ = $1; };

// The production for a qualified func_name has to exactly match the
// production for a qualified columnref, because we cannot tell which we
// are parsing until we see what comes after it ('(' or Sconst for a func_name,
// anything else for a columnref).  Therefore we allow 'indirection' which
// may contain subscripts, and reject that case in the C code.  (If we
// ever implement SQL99-like methods, such syntax may actually become legal!)
func_name:
  type_function_name {
    $$ = $1;
  }
  | ColId indirection {
    PARSER_UNSUPPORTED(@1);
  }
;

map_elems:
  map_elems ',' a_expr ':' a_expr {
    $1->AddKeyValuePair($3, $5);
    $$ = $1;
  }
  | a_expr ':' a_expr {
    $$ = MAKE_NODE(@1, PTCollectionExpr, DataType::MAP);
    $$->AddKeyValuePair($1, $3);
  }
;

map_expr:
  '{' map_elems '}' {
    $$ = $2;
  }
;

set_elems:
  set_elems ',' a_expr {
    $1->AddElement($3);
    $$ = $1;
  }
  | a_expr {
    $$ = MAKE_NODE(@1, PTCollectionExpr, DataType::SET);
    $$->AddElement($1);
  }
;

set_expr:
  '{' set_elems '}' {
    $$ = $2;
  }
;

list_elems:
  list_elems ',' a_expr {
    $1->AddElement($3);
    $$ = $1;
  }
  | a_expr {
    $$ = MAKE_NODE(@1, PTCollectionExpr, DataType::LIST);
    $$->AddElement($1);
  }
;

list_expr:
  '[' list_elems ']' {
    $$ = $2;
  }
  | '[' ']' {
    $$ = MAKE_NODE(@1, PTCollectionExpr, DataType::LIST);
  }
;

tuple_elems:
  tuple_elems ',' a_expr {
    $1->AddElement($3);
    $$ = $1;
  }
  | a_expr {
    $$ = MAKE_NODE(@1, PTCollectionExpr, DataType::TUPLE);
    $$->AddElement($1);
  }
;

tuple_expr:
  '(' tuple_elems ')' {
    $$ = $2;
  }
  | '(' ')' {
    $$ = MAKE_NODE(@1, PTCollectionExpr, DataType::TUPLE);
  }
;

collection_expr:
 // '{ }' can mean either (empty) map or set so we treat it separately here and infer the expected
 // type (i.e. map or set) during type analysis
  '{' '}' {
    $$ = MAKE_NODE(@1, PTCollectionExpr, DataType::SET);
  }
  | map_expr {
    $$ = $1;
  }
  | set_expr {
    $$ = $1;
  }
  | list_expr {
    $$ = $1;
  }
;

in_expr:
  tuple_expr {
    $$ = $1;
  }
  | bindvar {
    if ($1 != nullptr) {
      parser_->AddBindVariable(static_cast<PTBindVar*>($1.get()));
    }
    $$ = $1;
  }
;

// Constants.
AexprConst:
  ICONST {
    $$ = MAKE_NODE(@1, PTConstVarInt, $1);
  }
  | FCONST {
    $$ = MAKE_NODE(@1, PTConstDecimal, $1);
  }
  | INFINITY {
    $$ = MAKE_NODE(@1, PTConstDecimal, parser_->MakeString("Infinity"));
  }
  | NAN {
    $$ = MAKE_NODE(@1, PTConstDecimal, parser_->MakeString("NaN"));
  }
  | UCONST {
    $$ = MAKE_NODE(@1, PTConstUuid, $1);
  }
  | Sconst {
    $$ = MAKE_NODE(@1, PTConstText, $1);
  }
  | TRUE_P {
    $$ = MAKE_NODE(@1, PTConstBool, true);
  }
  | FALSE_P {
    $$ = MAKE_NODE(@1, PTConstBool, false);
  }
  | NULL_P {
    $$ = MAKE_NODE(@1, PTNull, nullptr);
  }
  | BCONST {                                                           // Binary string (BLOB type)
    $$ = MAKE_NODE(@1, PTConstBinary, $1);
  }
  | XCONST {                                                                        // Hexadecimal.
    PARSER_NOCODE(@1);
  }
  | func_name Sconst {
    PARSER_CQL_INVALID(@1);
  }
  | func_name '(' func_arg_list opt_sort_clause ')' Sconst {
    PARSER_CQL_INVALID(@1);
  }
  | ConstTypename Sconst {
    PARSER_CQL_INVALID(@1);
  }
  | ConstInterval Sconst opt_interval {
    PARSER_CQL_INVALID(@1);
  }
  | ConstInterval '(' Iconst ')' Sconst {
    PARSER_CQL_INVALID(@1);
  }
;

Iconst: ICONST {
  auto val = CheckedStoll($1->c_str());
  if (!val.ok()) {
    PARSER_CQL_INVALID_MSG(@1, "invalid integer");
  } else {
    $$ = *val;
  }
};

Sconst:   SCONST                  { $$ = $1; };

SignedIconst:
  Iconst                          { $$ = $1; }
  | '+' Iconst                    { $$ = + $2; }
  | '-' Iconst                    { $$ = - $2; }
;

// Role specifications.
RoleId:
  RoleSpec {
  }
;

RoleSpec:
  NonReservedWord {
  }
  | CURRENT_USER {
  }
  | SESSION_USER {
  }
;

role_list:
  RoleSpec                        { }
  | role_list ',' RoleSpec        { }
;

//--------------------------------------------------------------------------------------------------
// Name classification hierarchy.
//
// IDENT is the lexeme returned by the lexer for identifiers that match
// no known keyword.  In most cases, we can accept certain keywords as
// names, not only IDENTs.  We prefer to accept as many such keywords
// as possible to minimize the impact of "reserved words" on programmers.
// So, we divide names into several possible classes.  The classification
// is chosen in part to make keywords acceptable as names wherever possible.
//--------------------------------------------------------------------------------------------------

// Column identifier --- names that can be column, table, etc names.
ColId:
  IDENT                         { $$ = $1; }
  | unreserved_keyword          { $$ = parser_->MakeString($1); }
  | col_name_keyword            { $$ = parser_->MakeString($1); }
;

// Type/function identifier --- names that can be type or function names.
type_function_name:
  IDENT                         { $$ = $1; }
  | unreserved_keyword          { $$ = parser_->MakeString($1); }
  | type_func_name_keyword      { $$ = parser_->MakeString($1); }
  | UUID                        { $$ = parser_->MakeString($1); }
;

// Any not-fully-reserved word --- these names can be, eg, role names.
NonReservedWord:
  IDENT                         { $$ = $1; }
  | unreserved_keyword          { $$ = parser_->MakeString($1); }
  | col_name_keyword            { $$ = parser_->MakeString($1); }
  | type_func_name_keyword      { $$ = parser_->MakeString($1); }
;

// Column label --- allowed labels in "AS" clauses.
// This presently includes *all* Postgres keywords.
ColLabel:
  IDENT                         { $$ = $1; }
  | unreserved_keyword          { $$ = parser_->MakeString($1); }
  | col_name_keyword            { $$ = parser_->MakeString($1); }
  | type_func_name_keyword      { $$ = parser_->MakeString($1); }
  | reserved_keyword            { $$ = parser_->MakeString($1); }
;

//--------------------------------------------------------------------------------------------------
//  Type syntax
//    SQL introduces a large amount of type-specific syntax.
//    Define individual clauses to handle these cases, and use
//     the generic case to handle regular type-extensible Postgres syntax.
//    - thomas 1997-10-10
//--------------------------------------------------------------------------------------------------
Typename:
  SimpleTypename opt_array_bounds {
    $$ = $1;
  }
  | ParametricTypename {
    $$ = $1;
  }
  | SETOF SimpleTypename opt_array_bounds {
    PARSER_UNSUPPORTED(@1);
  }
  // SQL standard syntax, currently only one-dimensional.
  | SimpleTypename ARRAY '[' Iconst ']' {
    PARSER_UNSUPPORTED(@2);
  }
  | SETOF SimpleTypename ARRAY '[' Iconst ']' {
    PARSER_UNSUPPORTED(@1);
  }
  | SimpleTypename ARRAY {
    PARSER_UNSUPPORTED(@2);
  }
  | SETOF SimpleTypename ARRAY {
    PARSER_UNSUPPORTED(@1);
  }
;

ParametricTypename:
  MAP '<' Typename ',' Typename '>' {
    $$ = MAKE_NODE(@1, PTMap, $3, $5);
  }
  | SET '<' Typename '>' {
    $$ = MAKE_NODE(@1, PTSet, $3);
  }
  | LIST '<' Typename '>' {
    $$ = MAKE_NODE(@1, PTList, $3);
  }
  | TUPLE '<' type_name_list '>' {
    PARSER_UNSUPPORTED(@1);
  }
  | FROZEN '<' Typename '>' {
    $$ = MAKE_NODE(@1, PTFrozen, $3);
  }
;

SimpleTypename:
  Numeric {
    $$ = $1;
  }
  | Character {
    $$ = $1;
  }
  | ConstDatetime {
    $$ = $1;
  }
  | INET {
    $$ = MAKE_NODE(@1, PTInet);
  }
  | JSONB {
    $$ = MAKE_NODE(@1, PTJsonb);
  }
  | UUID {
    $$ = MAKE_NODE(@1, PTUuid);
  }
  | TIMEUUID {
    $$ = MAKE_NODE(@1, PTTimeUuid);
  }
  | BLOB {
    $$ = MAKE_NODE(@1, PTBlob);
  }
  | UserDefinedType {
    $$ = $1;
  }
  | Bit {
    PARSER_UNSUPPORTED(@1);
  }
  | ConstInterval opt_interval {
    PARSER_UNSUPPORTED(@1);
  }
  | ConstInterval '(' Iconst ')' {
    PARSER_UNSUPPORTED(@1);
  }
;

// UserDefinedType covers all type names that don't have a special syntax mandated by the standard,
// including qualified names. Currently this is used for CQL user-defined types but later scope can
// be expanded to cover other things (e.g. PostgreSQL GenericType)
UserDefinedType:
  udt_name opt_type_modifiers {
    $$ = MAKE_NODE(@1, PTUserDefinedType, $1);
  }
;

udt_name:
  IDENT {
    $$ = MAKE_NODE(@1, PTQualifiedName, $1);
  }
  | udt_name '.' IDENT {
    $1->Append(MAKE_NODE(@1, PTName, $3));
    $$ = $1;
  }
;

opt_array_bounds:
  opt_array_bounds '[' ']' {
    PARSER_UNSUPPORTED(@1);
  }
  | opt_array_bounds '[' Iconst ']' {
    PARSER_UNSUPPORTED(@1);
  }
  | /*EMPTY*/ {
  }
;

// We have a separate ConstTypename to allow defaulting fixed-length
// types such as CHAR() and BIT() to an unspecified length.
// SQL9x requires that these default to a length of one, but this
// makes no sense for constructs like CHAR 'hi' and BIT '0101',
// where there is an obvious better choice to make.
// Note that ConstInterval is not included here since it must
// be pushed up higher in the rules to accommodate the postfix
// options (e.g. INTERVAL '1' YEAR). Likewise, we have to handle
// the generic-type-name case in AExprConst to avoid premature
// reduce/reduce conflicts against function names.
ConstTypename:
  Numeric {
    $$ = $1;
  }
  | ConstBit {
    PARSER_UNSUPPORTED(@1);
  }
  | ConstCharacter {
    PARSER_UNSUPPORTED(@1);
  }
  | ConstDatetime {
    PARSER_UNSUPPORTED(@1);
  }
;

opt_type_modifiers:
  '(' expr_list ')' {
    PARSER_UNSUPPORTED(@1);
  }
  | /* EMPTY */ {
  }
;

// SQL numeric data types.
Numeric:
  BOOLEAN_P {
    $$ = MAKE_NODE(@1, PTBoolean);
  }
  | INT_P {
    $$ = MAKE_NODE(@1, PTInt);
  }
  | INTEGER {
    $$ = MAKE_NODE(@1, PTInt);
  }
  | TINYINT {
    $$ = MAKE_NODE(@1, PTTinyInt);
  }
  | SMALLINT {
    $$ = MAKE_NODE(@1, PTSmallInt);
  }
  | BIGINT {
    $$ = MAKE_NODE(@1, PTBigInt);
  }
  | COUNTER {
    $$ = MAKE_NODE(@1, PTCounter);
  }
  | REAL {
    $$ = MAKE_NODE(@1, PTFloat, 24);
  }
  | FLOAT_P opt_float {
    $$ = MAKE_NODE(@1, PTFloat, $2);
  }
  | DOUBLE_P {
    $$ = MAKE_NODE(@1, PTDouble);
  }
  | DOUBLE_P PRECISION {
    $$ = MAKE_NODE(@1, PTDouble);
  }
  | DECIMAL_P opt_type_modifiers {
    $$ = MAKE_NODE(@1, PTDecimal);
  }
  | VARINT opt_type_modifiers {
    $$ = MAKE_NODE(@1, PTVarInt);
  }
  | DEC opt_type_modifiers {
    PARSER_UNSUPPORTED(@1);
  }
  | NUMERIC opt_type_modifiers {
    PARSER_UNSUPPORTED(@1);
  }
;

opt_float:
  '(' Iconst ')' {
    if ($2 < 1 || $2 > 53) {
      PARSER_ERROR_MSG(@2, INVALID_PARAMETER_VALUE,
                       "Precision for FLOAT must be between 1 and 53");
    }
    $$ = $2;
  }
  | /*EMPTY*/ {
    $$ = 24;
  }
;

// SQL bit-field data types
// The following implements BIT() and BIT VARYING().
Bit:
  BitWithLength {
    $$ = $1;
  }
  | BitWithoutLength {
    $$ = $1;
  }
;

// ConstBit is like Bit except "BIT" defaults to unspecified length.
// See notes for ConstCharacter, which addresses same issue for "CHAR".
ConstBit:
  BitWithLength {
    $$ = $1;
  }
  | BitWithoutLength {
    $$ = $1;
  }
;

BitWithLength:
  BIT opt_varying '(' expr_list ')' {
  }
;

BitWithoutLength:
  BIT opt_varying {
  }
;

// SQL character data types.
// The following implements CHAR() and VARCHAR().
Character:
  CharacterWithLength {
    $$ = $1;
  }
  | CharacterWithoutLength {
    $$ = $1;
  }
;

ConstCharacter:
  CharacterWithLength {
    $$ = $1;
  }
  | CharacterWithoutLength {
    $$ = $1;
  }
;

CharacterWithLength:
  character '(' Iconst ')' opt_charset {
    PARSER_UNSUPPORTED(@2);
    $1->set_max_length($3);
    $$ = $1;
  }
;

CharacterWithoutLength:
  character opt_charset {
    $$ = $1;
  }
;

character:
  VARCHAR {
    $$ = MAKE_NODE(@1, PTVarchar);
  }
  | TEXT_P {
    $$ = MAKE_NODE(@1, PTVarchar);
  }
  | CHARACTER opt_varying {
    PARSER_UNSUPPORTED(@1);
    $$ = MAKE_NODE(@1, PTChar);
  }
  | CHAR_P opt_varying {
    PARSER_UNSUPPORTED(@1);
    $$ = MAKE_NODE(@1, PTChar);
  }
  | NATIONAL CHARACTER opt_varying {
    PARSER_UNSUPPORTED(@1);
  }
  | NATIONAL CHAR_P opt_varying {
    PARSER_UNSUPPORTED(@1);
  }
  | NCHAR opt_varying {
    PARSER_UNSUPPORTED(@1);
  }
;

opt_varying:
  /*EMPTY*/ {
    $$ = false;
  }
  | VARYING {
    PARSER_UNSUPPORTED(@1);
    $$ = true;
  }
;

opt_charset:
  /*EMPTY*/ {
  }
  | CHARACTER SET ColId {
    PARSER_UNSUPPORTED(@1);
  }
;

// SQL date/time types.
ConstDatetime:
  TIMESTAMP {
    $$ = MAKE_NODE(@1, PTTimestamp);
  }
  | TIMESTAMP '(' Iconst ')' opt_timezone {
    PARSER_UNSUPPORTED(@1);
  }
  | DATE {
    $$ = MAKE_NODE(@1, PTDate);
  }
  | TIME '(' Iconst ')' opt_timezone {
    PARSER_UNSUPPORTED(@1);
  }
  | TIME {
    $$ = MAKE_NODE(@1, PTTime);
  }
;

ConstInterval:
  INTERVAL {
  }
;

opt_timezone:
  WITH_LA TIME ZONE             { $$ = true; }
  | WITHOUT TIME ZONE           { $$ = false; }
  | /*EMPTY*/                   { $$ = false; }
    ;

opt_interval:
  YEAR_P {
  }
  | MONTH_P {
  }
  | DAY_P {
  }
  | HOUR_P {
  }
  | MINUTE_P {
  }
  | interval_second {
  }
  | YEAR_P TO MONTH_P {
  }
  | DAY_P TO HOUR_P {
  }
  | DAY_P TO MINUTE_P {
  }
  | DAY_P TO interval_second {
  }
  | HOUR_P TO MINUTE_P {
  }
  | HOUR_P TO interval_second {
  }
  | MINUTE_P TO interval_second {
  }
  | /*EMPTY*/ {
  }
;

interval_second:
  SECOND_P {
  } | SECOND_P '(' Iconst ')' {
  }
;

//--------------------------------------------------------------------------------------------------
// Keyword category lists.
// Every keyword present in the Postgres grammar should appear in exactly one of these lists.
// - Put a new keyword into the first list that it can go into without causing shift or reduce
//   conflicts.  The earlier lists define "less reserved" categories of keywords.
// - Make sure that each keyword's category in kwlist.h matches where it is listed here.  (Someday
//   we may be able to generate these lists and kwlist.h's table from a common master list.)
//
// NOTE:
// BISON (YACC) should apply the default action "$$ = $1;" when left and right values of a syntax
// rule are of the same datatype. Unfortunately, BISON 3.0.4 didn't do this for the case where left
// variable is a non-terminal and right hand value is a terminal. As a result, we won't rely on
// BISON default behavior.
//--------------------------------------------------------------------------------------------------

// "Unreserved" keywords --- available for use as any kind of name.
unreserved_keyword:
  ABORT_P { $$ = $1; }
  | ABSOLUTE_P { $$ = $1; }
  | ACCESS { $$ = $1; }
  | ACTION { $$ = $1; }
  | ADD_P { $$ = $1; }
  | ADMIN { $$ = $1; }
  | AFTER { $$ = $1; }
  | AGGREGATE { $$ = $1; }
  | ALSO { $$ = $1; }
  | ALTER { $$ = $1; }
  | ALWAYS { $$ = $1; }
  | ASSERTION { $$ = $1; }
  | ASSIGNMENT { $$ = $1; }
  | AT { $$ = $1; }
  | ATTRIBUTE { $$ = $1; }
  | BACKWARD { $$ = $1; }
  | BEFORE { $$ = $1; }
  | BEGIN_P { $$ = $1; }
  | BY { $$ = $1; }
  | CACHE { $$ = $1; }
  | CALLED { $$ = $1; }
  | CASCADE { $$ = $1; }
  | CASCADED { $$ = $1; }
  | CATALOG_P { $$ = $1; }
  | CHAIN { $$ = $1; }
  | CHARACTERISTICS { $$ = $1; }
  | CHECKPOINT { $$ = $1; }
  | CLASS { $$ = $1; }
  | CLOSE { $$ = $1; }
  | CLUSTER { $$ = $1; }
  | CLUSTERING { $$ = $1; }
  | COMMENT { $$ = $1; }
  | COMMENTS { $$ = $1; }
  | COMMIT { $$ = $1; }
  | COMMITTED { $$ = $1; }
  | COMPACT { $$ = $1; }
  | CONFIGURATION { $$ = $1; }
  | CONFLICT { $$ = $1; }
  | CONNECTION { $$ = $1; }
  | CONSTRAINTS { $$ = $1; }
  | CONTENT_P { $$ = $1; }
  | CONTINUE_P { $$ = $1; }
  | CONVERSION_P { $$ = $1; }
  | COPY { $$ = $1; }
  | COST { $$ = $1; }
  | COVERING { $$ = $1; }
  | CSV { $$ = $1; }
  | CUBE { $$ = $1; }
  | CURRENT_P { $$ = $1; }
  | CURSOR { $$ = $1; }
  | CYCLE { $$ = $1; }
  | DATA_P { $$ = $1; }
  | DATABASE { $$ = $1; }
  | DAY_P { $$ = $1; }
  | DEALLOCATE { $$ = $1; }
  | DECLARE { $$ = $1; }
  | DEFAULTS { $$ = $1; }
  | DEFERRED { $$ = $1; }
  | DEFINER { $$ = $1; }
  | DELETE_P { $$ = $1; }
  | DELIMITER { $$ = $1; }
  | DELIMITERS { $$ = $1; }
  | DICTIONARY { $$ = $1; }
  | DISABLE_P { $$ = $1; }
  | DISCARD { $$ = $1; }
  | DOCUMENT_P { $$ = $1; }
  | DOMAIN_P { $$ = $1; }
  | DROP { $$ = $1; }
  | EACH { $$ = $1; }
  | ENABLE_P { $$ = $1; }
  | ENCODING { $$ = $1; }
  | ENCRYPTED { $$ = $1; }
  | ENUM_P { $$ = $1; }
  | ERROR { $$ = $1; }
  | ESCAPE { $$ = $1; }
  | EVENT { $$ = $1; }
  | EXCLUDE { $$ = $1; }
  | EXCLUDING { $$ = $1; }
  | EXCLUSIVE { $$ = $1; }
  | EXECUTE { $$ = $1; }
  | EXPLAIN { $$ = $1; }
  | EXTENSION { $$ = $1; }
  | EXTERNAL { $$ = $1; }
  | FAMILY { $$ = $1; }
  | FILTER { $$ = $1; }
  | FILTERING { $$ = $1; }
  | FIRST_P { $$ = $1; }
  | FOLLOWING { $$ = $1; }
  | FORCE { $$ = $1; }
  | FORWARD { $$ = $1; }
  | FUNCTION { $$ = $1; }
  | FUNCTIONS { $$ = $1; }
  | GLOBAL { $$ = $1; }
  | GRANTED { $$ = $1; }
  | HANDLER { $$ = $1; }
  | HEADER_P { $$ = $1; }
  | HOLD { $$ = $1; }
  | HOUR_P { $$ = $1; }
  | IDENTITY_P { $$ = $1; }
  | IMMEDIATE { $$ = $1; }
  | IMMUTABLE { $$ = $1; }
  | IMPLICIT_P { $$ = $1; }
  | IMPORT_P { $$ = $1; }
  | INCLUDE { $$ = $1; }
  | INCLUDING { $$ = $1; }
  | INCREMENT { $$ = $1; }
  | INDEX { $$ = $1; }
  | INDEXES { $$ = $1; }
  | INHERIT { $$ = $1; }
  | INHERITS { $$ = $1; }
  | INLINE_P { $$ = $1; }
  | INPUT_P { $$ = $1; }
  | INSENSITIVE { $$ = $1; }
  | INSERT { $$ = $1; }
  | INSTEAD { $$ = $1; }
  | INVOKER { $$ = $1; }
  | ISOLATION { $$ = $1; }
  | KEY { $$ = $1; }
  | LABEL { $$ = $1; }
  | LANGUAGE { $$ = $1; }
  | LARGE_P { $$ = $1; }
  | LAST_P { $$ = $1; }
  | LEAKPROOF { $$ = $1; }
  | LEVEL { $$ = $1; }
  | LISTEN { $$ = $1; }
  | LOAD { $$ = $1; }
  | LOCAL { $$ = $1; }
  | LOCATION { $$ = $1; }
  | LOCK_P { $$ = $1; }
  | LOCKED { $$ = $1; }
  | LOGGED { $$ = $1; }
  | LOGIN { $$ = $1; }
  | MAPPING { $$ = $1; }
  | MATCH { $$ = $1; }
  | MATERIALIZED { $$ = $1; }
  | MAXVALUE { $$ = $1; }
  | MINUTE_P { $$ = $1; }
  | MINVALUE { $$ = $1; }
  | MODE { $$ = $1; }
  | MONTH_P { $$ = $1; }
  | MOVE { $$ = $1; }
  | NAME_P { $$ = $1; }
  | NAMES { $$ = $1; }
  | NEXT { $$ = $1; }
  | NO { $$ = $1; }
  | NOTHING { $$ = $1; }
  | NOTIFY { $$ = $1; }
  | NOWAIT { $$ = $1; }
  | NULLS_P { $$ = $1; }
  | OBJECT_P { $$ = $1; }
  | OF { $$ = $1; }
  | OFF { $$ = $1; }
  | OIDS { $$ = $1; }
  | OPERATOR { $$ = $1; }
  | OPTION { $$ = $1; }
  | OPTIONS { $$ = $1; }
  | ORDINALITY { $$ = $1; }
  | OVER { $$ = $1; }
  | OWNED { $$ = $1; }
  | OWNER { $$ = $1; }
  | PARSER { $$ = $1; }
  | PARTIAL { $$ = $1; }
  | PARTITION { $$ = $1; }
  | PASSING { $$ = $1; }
  | PASSWORD { $$ = $1; }
  | PERMISSION { $$ = $1; }
  | PERMISSIONS { $$ = $1; }
  | PLANS { $$ = $1; }
  | POLICY { $$ = $1; }
  | PRECEDING { $$ = $1; }
  | PREPARE { $$ = $1; }
  | PREPARED { $$ = $1; }
  | PRESERVE { $$ = $1; }
  | PRIOR { $$ = $1; }
  | PRIVILEGES { $$ = $1; }
  | PROCEDURAL { $$ = $1; }
  | PROCEDURE { $$ = $1; }
  | PROGRAM { $$ = $1; }
  | QUOTE { $$ = $1; }
  | RANGE { $$ = $1; }
  | READ { $$ = $1; }
  | REASSIGN { $$ = $1; }
  | RECHECK { $$ = $1; }
  | RECURSIVE { $$ = $1; }
  | REF { $$ = $1; }
  | REFRESH { $$ = $1; }
  | REINDEX { $$ = $1; }
  | RELATIVE_P { $$ = $1; }
  | RELEASE { $$ = $1; }
  | RENAME { $$ = $1; }
  | REPEATABLE { $$ = $1; }
  | REPLACE { $$ = $1; }
  | REPLICA { $$ = $1; }
  | RESET { $$ = $1; }
  | RESTART { $$ = $1; }
  | RESTRICT { $$ = $1; }
  | REVOKE { $$ = $1; }
  | ROLE { $$ = $1; }
  | ROLES { $$ = $1; }
  | ROLLBACK { $$ = $1; }
  | ROLLUP { $$ = $1; }
  | ROWS { $$ = $1; }
  | RULE { $$ = $1; }
  | SAVEPOINT { $$ = $1; }
  | SCHEME { $$ = $1; }
  | SCROLL { $$ = $1; }
  | SEARCH { $$ = $1; }
  | SECOND_P { $$ = $1; }
  | SECURITY { $$ = $1; }
  | SEQUENCE { $$ = $1; }
  | SEQUENCES { $$ = $1; }
  | SERIALIZABLE { $$ = $1; }
  | SERVER { $$ = $1; }
  | SESSION { $$ = $1; }
  | SET { $$ = $1; }
  | SETS { $$ = $1; }
  | SHARE { $$ = $1; }
  | SHOW { $$ = $1; }
  | SIMPLE { $$ = $1; }
  | SKIP { $$ = $1; }
  | SNAPSHOT { $$ = $1; }
  | SQL_P { $$ = $1; }
  | STABLE { $$ = $1; }
  | STANDALONE_P { $$ = $1; }
  | START { $$ = $1; }
  | STATEMENT { $$ = $1; }
  | STATIC { $$ = $1; }
  | STATISTICS { $$ = $1; }
  | STATUS { $$ = $1; }
  | STDIN { $$ = $1; }
  | STDOUT { $$ = $1; }
  | STORAGE { $$ = $1; }
  | STRICT_P { $$ = $1; }
  | STRIP_P { $$ = $1; }
  | SUPERUSER { $$ = $1; }
  | SYSID { $$ = $1; }
  | SYSTEM_P { $$ = $1; }
  | TABLES { $$ = $1; }
  | TABLESPACE { $$ = $1; }
  | TEMP { $$ = $1; }
  | TEMPLATE { $$ = $1; }
  | TEMPORARY { $$ = $1; }
  | TRANSACTION { $$ = $1; }
  | TRANSFORM { $$ = $1; }
  | TRIGGER { $$ = $1; }
  | TRUNCATE { $$ = $1; }
  | TRUSTED { $$ = $1; }
  | TTL { $$ = $1; }
  | TYPE_P { $$ = $1; }
  | TYPES_P { $$ = $1; }
  | UNBOUNDED { $$ = $1; }
  | UNCOMMITTED { $$ = $1; }
  | UNENCRYPTED { $$ = $1; }
  | UNKNOWN { $$ = $1; }
  | UNLISTEN { $$ = $1; }
  | UNLOGGED { $$ = $1; }
  | UNSET { $$ = $1; }
  | UNTIL { $$ = $1; }
  | UPDATE { $$ = $1; }
  | USER { $$ = $1; }
  | VACUUM { $$ = $1; }
  | VALID { $$ = $1; }
  | VALIDATE { $$ = $1; }
  | VALIDATOR { $$ = $1; }
  | VALUE_P { $$ = $1; }
  | VARYING { $$ = $1; }
  | VERSION_P { $$ = $1; }
  | VIEW { $$ = $1; }
  | VIEWS { $$ = $1; }
  | VOLATILE { $$ = $1; }
  | WHEN { $$ = $1; }
  | WHITESPACE_P { $$ = $1; }
  | WITHIN { $$ = $1; }
  | WITHOUT { $$ = $1; }
  | WORK { $$ = $1; }
  | WRAPPER { $$ = $1; }
  | WRITE { $$ = $1; }
  | XML_P { $$ = $1; }
  | YEAR_P { $$ = $1; }
  | YES_P { $$ = $1; }
  | ZONE { $$ = $1; }
  ;

// Column identifier --- keywords that can be column, table, etc names.
//
// Many of these keywords will in fact be recognized as type or function
// names too; but they have special productions for the purpose, and so
// can't be treated as "generic" type or function names.
//
// The type names appearing here are not usable as function names
// because they can be followed by '(' in typename productions, which
// looks too much like a function call for an LR(1) parser.
col_name_keyword:
  BETWEEN { $$ = $1; }
  | BIGINT { $$ = $1; }
  | BIT { $$ = $1; }
  | BLOB { $$ = $1; }
  | BOOLEAN_P { $$ = $1; }
  | CHAR_P { $$ = $1; }
  | CHARACTER { $$ = $1; }
  | COALESCE { $$ = $1; }
  | COUNTER { $$ = $1; }
  | DATE { $$ = $1; }
  | DEC { $$ = $1; }
  | DECIMAL_P { $$ = $1; }
  | DOUBLE_P { $$ = $1; }
  | EXTRACT { $$ = $1; }
  | FLOAT_P { $$ = $1; }
  | FROZEN { $$ = $1; }
  | GREATEST { $$ = $1; }
  | GROUP_P { $$ = $1; }
  | GROUPING { $$ = $1; }
  | INET { $$ = $1; }
  | JSON { $$ = $1; }
  | JSONB { $$ = $1; }
  | INOUT { $$ = $1; }
  | INT_P { $$ = $1; }
  | INTEGER { $$ = $1; }
  | INTERVAL { $$ = $1; }
  | LEAST { $$ = $1; }
  | LIST { $$ = $1; }
  | MAP { $$ = $1; }
  | NATIONAL { $$ = $1; }
  | NCHAR { $$ = $1; }
  | NONE { $$ = $1; }
  | NULLIF { $$ = $1; }
  | NUMERIC { $$ = $1; }
  | OFFSET { $$ = $1; }
  | OUT_P { $$ = $1; }
  | OVERLAY { $$ = $1; }
  | POSITION { $$ = $1; }
  | PRECISION { $$ = $1; }
  | REAL { $$ = $1; }
  | ROW { $$ = $1; }
  | SETOF { $$ = $1; }
  | SMALLINT { $$ = $1; }
  | SUBSTRING { $$ = $1; }
  | TEXT_P { $$ = $1; }
  | TIME { $$ = $1; }
  | TIMESTAMP { $$ = $1; }
  | TIMEUUID { $$ = $1; }
  | TINYINT { $$ = $1; }
  | TREAT { $$ = $1; }
  | TRIM { $$ = $1; }
  | TUPLE { $$ = $1; }
  | UUID { $$ = $1; }
  | VALUES { $$ = $1; }
  | VARCHAR { $$ = $1; }
  | VARINT { $$ = $1; }
  | XMLATTRIBUTES { $$ = $1; }
  | XMLCONCAT { $$ = $1; }
  | XMLELEMENT { $$ = $1; }
  | XMLEXISTS { $$ = $1; }
  | XMLFOREST { $$ = $1; }
  | XMLPARSE { $$ = $1; }
  | XMLPI { $$ = $1; }
  | XMLROOT { $$ = $1; }
  | XMLSERIALIZE { $$ = $1; }
;

// Type/function identifier --- keywords that can be type or function names.
//
// Most of these are keywords that are used as operators in expressions;
// in general such keywords can't be column names because they would be
// ambiguous with variables, but they are unambiguous as function identifiers.
//
// Do not include POSITION, SUBSTRING, etc here since they have explicit
// productions in a_expr to support the goofy SQL9x argument syntax.
// - thomas 2000-11-28
type_func_name_keyword:
  AUTHORIZATION { $$ = $1; }
  | BINARY { $$ = $1; }
  | COLLATION { $$ = $1; }
  | CONCURRENTLY { $$ = $1; }
  | CROSS { $$ = $1; }
  | CURRENT_SCHEMA { $$ = $1; }
  | FREEZE { $$ = $1; }
  | FULL { $$ = $1; }
  | ILIKE { $$ = $1; }
  | INNER_P { $$ = $1; }
  | IS { $$ = $1; }
  | ISNULL { $$ = $1; }
  | JOIN { $$ = $1; }
  | LEFT { $$ = $1; }
  | LIKE { $$ = $1; }
  | NATURAL { $$ = $1; }
  | NOTNULL { $$ = $1; }
  | OUTER_P { $$ = $1; }
  | OVERLAPS { $$ = $1; }
  | RIGHT { $$ = $1; }
  | SIMILAR { $$ = $1; }
  | TABLESAMPLE { $$ = $1; }
  | VERBOSE { $$ = $1; }
;

// Reserved keyword --- these keywords are usable only as a ColLabel.
//
// Keywords appear here if they could not be distinguished from variable,
// type, or function names in some contexts.  Don't put things here unless
// forced to.
reserved_keyword:
  ALL { $$ = $1; }
  | ALLOW { $$ = $1; }
  | ANALYSE { $$ = $1; }
  | ANALYZE { $$ = $1; }
  | AND { $$ = $1; }
  | ANY { $$ = $1; }
  | ARRAY { $$ = $1; }
  | AS { $$ = $1; }
  | ASC { $$ = $1; }
  | ASYMMETRIC { $$ = $1; }
  | AUTHORIZE { $$ = $1; }
  | BOTH { $$ = $1; }
  | CASE { $$ = $1; }
  | CAST { $$ = $1; }
  | CHECK { $$ = $1; }
  | COLLATE { $$ = $1; }
  | COLUMN { $$ = $1; }
  | CONSTRAINT { $$ = $1; }
  | CREATE { $$ = $1; }
  | CURRENT_CATALOG { $$ = $1; }
  | CURRENT_DATE { $$ = $1; }
  | CURRENT_ROLE { $$ = $1; }
  | CURRENT_TIME { $$ = $1; }
  | CURRENT_TIMESTAMP { $$ = $1; }
  | CURRENT_USER { $$ = $1; }
  | DEFAULT { $$ = $1; }
  | DEFERRABLE { $$ = $1; }
  | DESC { $$ = $1; }
  | DESCRIBE { $$ = $1; }
  | DISTINCT { $$ = $1; }
  | DO { $$ = $1; }
  | ELSE { $$ = $1; }
  | END_P { $$ = $1; }
  | EXCEPT { $$ = $1; }
  | FALSE_P { $$ = $1; }
  | FETCH { $$ = $1; }
  | FOR { $$ = $1; }
  | FOREIGN { $$ = $1; }
  | FROM { $$ = $1; }
  | GRANT { $$ = $1; }
  | HAVING { $$ = $1; }
  | IF_P { $$ = $1; }
  | IN_P { $$ = $1; }
  | INFINITY { $$ = $1; }
  | INITIALLY { $$ = $1; }
  | INTERSECT { $$ = $1; }
  | INTO { $$ = $1; }
  | KEYSPACE { $$ = $1; }
  | KEYSPACES { $$ = $1; }
  | LATERAL_P { $$ = $1; }
  | LEADING { $$ = $1; }
  | LIMIT { $$ = $1; }
  | LOCALTIME { $$ = $1; }
  | LOCALTIMESTAMP { $$ = $1; }
  | MODIFY { $$ = $1; }
  | NAN { $$ = $1; }
  | NOT { $$ = $1; }
  | NULL_P { $$ = $1; }
  | ON { $$ = $1; }
  | ONLY { $$ = $1; }
  | OR { $$ = $1; }
  | ORDER { $$ = $1; }
  | PARTITION_HASH { $$ = $1; }
  | PLACING { $$ = $1; }
  | PRIMARY { $$ = $1; }
  | RETURNING { $$ = $1; }
  | RETURNS { $$ = $1; }
  | SCHEMA { $$ = $1; }
  | SELECT { $$ = $1; }
  | SESSION_USER { $$ = $1; }
  | SOME { $$ = $1; }
  | SYMMETRIC { $$ = $1; }
  | TABLE { $$ = $1; }
  | THEN { $$ = $1; }
  | TO { $$ = $1; }
  | TOKEN { $$ = $1; }
  | TRAILING { $$ = $1; }
  | TRUE_P { $$ = $1; }
  | UNION { $$ = $1; }
  | UNIQUE { $$ = $1; }
  | USE { $$ = $1; }
  | USING { $$ = $1; }
  | VARIADIC { $$ = $1; }
  | WHERE { $$ = $1; }
  | WINDOW { $$ = $1; }
  | WITH { $$ = $1; }
;

//--------------------------------------------------------------------------------------------------
// INACTIVE PARSING RULES. INACTIVE PARSING RULES. INACTIVE PARSING RULES. INACTIVE PARSING RULES.
//
// QL currently does not use these PostgreQL rules.
//--------------------------------------------------------------------------------------------------

inactive_stmt:
  AlterEventTrigStmt
  | AlterDatabaseStmt
  | AlterDatabaseSetStmt
  | AlterDefaultPrivilegesStmt
  | AlterDomainStmt
  | AlterEnumStmt
  | AlterExtensionStmt
  | AlterExtensionContentsStmt
  | AlterFdwStmt
  | AlterForeignServerStmt
  | AlterForeignTableStmt
  | AlterFunctionStmt
  | AlterObjectSchemaStmt
  | AlterOwnerStmt
  | AlterPolicyStmt
  | AlterSeqStmt
  | AlterSystemStmt
  | AlterTblSpcStmt
  | AlterCompositeTypeStmt
  | AlterTSConfigurationStmt
  | AlterTSDictionaryStmt
  | AlterUserSetStmt
  | AlterUserStmt
  | AnalyzeStmt
  | CheckPointStmt
  | ClosePortalStmt
  | ClusterStmt
  | CommentStmt
  | ConstraintsSetStmt
  | CopyStmt
  | CreateAsStmt
  | CreateAssertStmt
  | CreateCastStmt
  | CreateConversionStmt
  | CreateDomainStmt
  | CreateExtensionStmt
  | CreateFdwStmt
  | CreateForeignServerStmt
  | CreateForeignTableStmt
  | CreateFunctionStmt
  | CreateMatViewStmt
  | CreateOpClassStmt
  | CreateOpFamilyStmt
  | AlterOpFamilyStmt
  | CreatePolicyStmt
  | CreatePLangStmt
  | CreateSeqStmt
  | CreateTableSpaceStmt
  | CreateTransformStmt
  | CreateTrigStmt
  | CreateEventTrigStmt
  | CreateUserStmt
  | CreatedbStmt
  | DeallocateStmt
  | DeclareCursorStmt
  | DefineStmt
  | DiscardStmt
  | DoStmt
  | DropAssertStmt
  | DropCastStmt
  | DropFdwStmt
  | DropForeignServerStmt
  | DropOpClassStmt
  | DropOpFamilyStmt
  | DropOwnedStmt
  | DropPolicyStmt
  | DropPLangStmt
  | DropRuleStmt
  | DropTableSpaceStmt
  | DropTransformStmt
  | DropTrigStmt
  | DropUserStmt
  | DropdbStmt
  | ExecuteStmt
  | FetchStmt
  | ImportForeignSchemaStmt
  | ListenStmt
  | RefreshMatViewStmt
  | LoadStmt
  | LockStmt
  | NotifyStmt
  | PrepareStmt
  | ReassignOwnedStmt
  | ReindexStmt
  | RemoveAggrStmt
  | RemoveFuncStmt
  | RemoveOperStmt
  | RenameStmt
  | RuleStmt
  | SecLabelStmt
  | UnlistenStmt
  | VacuumStmt
  | VariableResetStmt
  | VariableSetStmt
  | VariableShowStmt
  | ViewStmt
;

/*****************************************************************************
 *
 * Create a new role
 *
 *****************************************************************************/

CreateRoleStmt: CREATE ROLE role_name optRoleOptionList {
    $$ = MAKE_NODE(@1, PTCreateRole, $3 , $4, false /* create_if_not_exists */ );
  }
  | CREATE ROLE IF_P NOT_LA EXISTS role_name optRoleOptionList{
    $$ = MAKE_NODE(@1, PTCreateRole, $6 , $7, true /* create_if_not_exists */ );
  }
;

role_name:
    NonReservedWord { $$ = $1; }
  | Sconst { $$ = $1; }
;

optRoleOptionList:
  /*EMPTY*/ {
    $$ = nullptr;
  }
  | WITH RoleOptionList {
    $$ = $2;
  }
;

RoleOptionList:
  RoleOption {
    $$ = MAKE_NODE(@1, PTRoleOptionListNode, $1);
  }
  | RoleOptionList AND RoleOption {
    $1->Append($3);
    $$ = $1;
  }
;

RoleOption:
  PASSWORD '=' Sconst {
      $$ = MAKE_NODE(@1, PTRolePassword, $3);
  }
  | LOGIN '=' boolean {
      $$ = MAKE_NODE(@1, PTRoleLogin, $3);
  }
  | SUPERUSER '=' boolean {
      $$ = MAKE_NODE(@1, PTRoleSuperuser, $3);
  }
  | OPTIONS '=' map_expr {
      PARSER_UNSUPPORTED(@1);
  }
;

boolean:
  TRUE_P {
    $$ = true;
  }
  | FALSE_P {
    $$ = false;
  }
;

opt_with: WITH {}
  | WITH_LA {}
  | /*EMPTY*/ {}
;

/*
 * Options for CREATE ROLE and ALTER ROLE (also used by CREATE/ALTER USER
 * for backwards compatibility).  Note: the only option required by SQL99
 * is "WITH ADMIN name".
 */
OptRoleList: OptRoleList CreateOptRoleElem
  { $$ = nullptr; }
  | /* EMPTY */ { $$ = nullptr; }
;

AlterOptRoleList:
  AlterOptRoleList AlterOptRoleElem {
    $$ = nullptr;
  }
  | /* EMPTY */ {
  }
;

AlterOptRoleElem:
  PASSWORD Sconst {
    $$ = nullptr;
  }
  | PASSWORD NULL_P {
    $$ = nullptr;
  }
  | ENCRYPTED PASSWORD Sconst {
    $$ = nullptr;
  }
  | UNENCRYPTED PASSWORD Sconst {
    $$ = nullptr;
  }
  | INHERIT {
    $$ = nullptr;
  }
  | CONNECTION LIMIT SignedIconst {
    $$ = nullptr;
  }
  | VALID UNTIL Sconst {
    $$ = nullptr;
  }
  | USER role_list {
    $$ = nullptr;
  }
  | IDENT {
    $$ = nullptr;
  }
;

CreateOptRoleElem:
  AlterOptRoleElem {
    $$ = $1;
  }
  | SYSID Iconst {
  }
  | ADMIN role_list {
  }
  | ROLE role_list {
  }
  | IN_P ROLE role_list {
  }
;

/*****************************************************************************
 *
 * Create a new Postgres DBMS user (role with implied login ability)
 *
 *****************************************************************************/

CreateUserStmt:
  CREATE USER RoleId opt_with OptRoleList {
  }
;

/*****************************************************************************
 *
 * Alter a role
 *
 *****************************************************************************/

AlterRoleStmt:
  ALTER ROLE role_name WITH RoleOptionList {
    $$ = MAKE_NODE(@1, PTAlterRole, $3 , $5);
  }
;

/*****************************************************************************
 *
 * Alter a postgresql DBMS user
 *
 *****************************************************************************/

AlterUserStmt: ALTER USER RoleSpec opt_with AlterOptRoleList {
  }
;

AlterUserSetStmt: ALTER USER RoleSpec SetResetClause {
  }
;

/*****************************************************************************
 *
 * Drop a postgresql DBMS user
 *
 * XXX Ideally this would have CASCADE/RESTRICT options, but since a user
 * might own objects in multiple databases, there is presently no way to
 * implement either cascading or restricting.  Caveat DBA.
 *****************************************************************************/

DropUserStmt:
  DROP USER role_list {
  }
  | DROP USER IF_P EXISTS role_list {
  }
;

/*****************************************************************************
 *
 * Manipulate a schema
 *
 *****************************************************************************/

/*
 *  schema_stmt are the ones that can show up inside a CREATE SCHEMA
 *  statement (in addition to by themselves).
 */
inactive_schema_stmt:
  CreateSeqStmt
  | CreateTrigStmt
  | ViewStmt
;

/*****************************************************************************
 *
 * Set PG internal variable
 *    SET name TO 'var_value'
 * Include SQL syntax (thomas 1997-10-22):
 *    SET TIME ZONE 'var_value'
 *
 *****************************************************************************/

VariableSetStmt:
  SET set_rest {
  }
  | SET LOCAL set_rest {
  }
  | SET SESSION set_rest {
  }
;

set_rest:
  TRANSACTION transaction_mode_list {
  }
  | SESSION CHARACTERISTICS AS TRANSACTION transaction_mode_list {
  }
  | set_rest_more
  ;

generic_set:
  var_name TO var_list {
  }
  | var_name '=' var_list {
  }
  | var_name TO DEFAULT {
  }
  | var_name '=' DEFAULT {
  }
;

set_rest_more:  /* Generic SET syntaxes: */
  generic_set {$$ = $1;}
  | var_name FROM CURRENT_P {
  }
  /* Special syntaxes mandated by SQL standard: */
  | TIME ZONE zone_value {
  }
  | CATALOG_P Sconst {
  }
  | SCHEMA Sconst {
  }
  | NAMES opt_encoding {
  }
  | ROLE NonReservedWord_or_Sconst {
  }
  | SESSION AUTHORIZATION NonReservedWord_or_Sconst {
  }
  | SESSION AUTHORIZATION DEFAULT {
  }
  | XML_P OPTION document_or_content {
  }
  /* Special syntaxes invented by PostgreSQL: */
  | TRANSACTION SNAPSHOT Sconst {
  }
;

var_name:
  ColId { $$ = $1; }
  | var_name '.' ColId {
  }
;

var_list:
  var_value { }
  | var_list ',' var_value { }
;

var_value:
  opt_boolean_or_string {}
  | NumericOnly {}
;

iso_level:
  READ UNCOMMITTED            { $$ = "read uncommitted"; }
  | READ COMMITTED            { $$ = "read committed"; }
  | REPEATABLE READ           { $$ = "repeatable read"; }
  | SERIALIZABLE              { $$ = "serializable"; }
;

opt_boolean_or_string:
  TRUE_P                      { $$ = "true"; }
  | FALSE_P                   { $$ = "false"; }
  | ON                        { $$ = "on"; }
  /*
   * OFF is also accepted as a boolean value, but is handled by
   * the NonReservedWord rule.  The action for booleans and strings
   * is the same, so we don't need to distinguish them here.
   */
  | NonReservedWord_or_Sconst { $$ = $1->c_str(); }
;

/* Timezone values can be:
 * - a string such as 'pst8pdt'
 * - an identifier such as "pst8pdt"
 * - an integer or floating point number
 * - a time interval per SQL99
 * ColId gives reduce/reduce errors against ConstInterval and LOCAL,
 * so use IDENT (meaning we reject anything that is a key word).
 */
zone_value:
  Sconst {
  }
  | IDENT {
  }
  | ConstInterval Sconst opt_interval {
  }
  | ConstInterval '(' Iconst ')' Sconst {
  }
  | NumericOnly {}
  | DEFAULT {}
  | LOCAL {}
;

opt_encoding:
  Sconst {}
  | DEFAULT {}
  | /*EMPTY*/ {}
;

NonReservedWord_or_Sconst:
  NonReservedWord { $$ = $1; }
  | Sconst { $$ = $1; }
;

VariableResetStmt:
  RESET reset_rest {}
;

reset_rest:
  generic_reset {}
  | TIME ZONE {
  }
  | TRANSACTION ISOLATION LEVEL {
  }
  | SESSION AUTHORIZATION {
  }
;

generic_reset:
  var_name {
  }
  | ALL {
  }
;

/* SetResetClause allows SET or RESET without LOCAL */
SetResetClause:
  SET set_rest {}
  | VariableResetStmt {}
;

/* SetResetClause allows SET or RESET without LOCAL */
FunctionSetResetClause:
  SET set_rest_more {}
  | VariableResetStmt {}
;

VariableShowStmt:
  SHOW var_name {
  }
  | SHOW TIME ZONE {
  }
  | SHOW TRANSACTION ISOLATION LEVEL {
  }
  | SHOW SESSION AUTHORIZATION {
  }
  | SHOW ALL {
  }
;

ConstraintsSetStmt:
  SET CONSTRAINTS constraints_set_list constraints_set_mode {
  }
;

constraints_set_list:
  ALL {}
  | qualified_name_list {}
;

constraints_set_mode:
  DEFERRED { $$ = true; }
  | IMMEDIATE { $$ = false; }
;

/*
 * Checkpoint statement
 */
CheckPointStmt:
  CHECKPOINT {
  }
;

/*****************************************************************************
 *
 * DISCARD { ALL | TEMP | PLANS | SEQUENCES }
 *
 *****************************************************************************/

DiscardStmt:
  DISCARD ALL {
  }
  | DISCARD TEMP {
  }
  | DISCARD TEMPORARY {
  }
  | DISCARD PLANS {
  }
  | DISCARD SEQUENCES {
  }
;

/*****************************************************************************
 *
 *  ALTER [ TABLE | INDEX | SEQUENCE | VIEW | MATERIALIZED VIEW ] variations
 *
 * Note: we accept all subcommands for each of the five variants, and sort
 * out what's really legal at execution time.
 *****************************************************************************/

InactiveAlterTableStmt:
  ALTER TABLE IF_P EXISTS relation_expr alter_table_cmds {
  }
  | ALTER TABLE ALL IN_P TABLESPACE name SET TABLESPACE name opt_nowait {
  }
  | ALTER TABLE ALL IN_P TABLESPACE name OWNED BY role_list SET TABLESPACE name opt_nowait {
  }
  | ALTER INDEX qualified_name alter_table_cmds {
  }
  | ALTER INDEX IF_P EXISTS qualified_name alter_table_cmds {
  }
  | ALTER INDEX ALL IN_P TABLESPACE name SET TABLESPACE name opt_nowait {
  }
  | ALTER INDEX ALL IN_P TABLESPACE name OWNED BY role_list SET TABLESPACE name opt_nowait {
  }
  | ALTER SEQUENCE qualified_name alter_table_cmds {
  }
  | ALTER SEQUENCE IF_P EXISTS qualified_name alter_table_cmds {
  }
  | ALTER VIEW qualified_name alter_table_cmds {
  }
  | ALTER VIEW IF_P EXISTS qualified_name alter_table_cmds {
  }
  | ALTER MATERIALIZED VIEW qualified_name alter_table_cmds {
  }
  | ALTER MATERIALIZED VIEW IF_P EXISTS qualified_name alter_table_cmds {
  }
  | ALTER MATERIALIZED VIEW ALL IN_P TABLESPACE name SET TABLESPACE name opt_nowait {
  }
  | ALTER MATERIALIZED VIEW ALL IN_P TABLESPACE name OWNED BY role_list
    SET TABLESPACE name opt_nowait {
  }
;

alter_table_cmds:
  alter_table_cmd {}
  | alter_table_cmds ',' alter_table_cmd {}
;

alter_table_cmd:
  /* ALTER TABLE <name> ADD <coldef> */
  ADD_P columnDef {
  }
  /* ALTER TABLE <name> ADD COLUMN <coldef> */
  | ADD_P COLUMN columnDef {
  }
  /* ALTER TABLE <name> ALTER [COLUMN] <colname> {SET DEFAULT <expr>|DROP DEFAULT} */
  | ALTER opt_column ColId alter_column_default {
  }
  /* ALTER TABLE <name> ALTER [COLUMN] <colname> DROP NOT NULL */
  | ALTER opt_column ColId DROP NOT NULL_P {
  }
  /* ALTER TABLE <name> ALTER [COLUMN] <colname> SET NOT NULL */
  | ALTER opt_column ColId SET NOT NULL_P {
  }
  /* ALTER TABLE <name> ALTER [COLUMN] <colname> SET STATISTICS <SignedIconst> */
  | ALTER opt_column ColId SET STATISTICS SignedIconst {
  }
  /* ALTER TABLE <name> ALTER [COLUMN] <colname> SET ( column_parameter = value [, ... ] ) */
  | ALTER opt_column ColId SET reloptions {
  }
  /* ALTER TABLE <name> ALTER [COLUMN] <colname> SET ( column_parameter = value [, ... ] ) */
  | ALTER opt_column ColId RESET reloptions {
  }
  /* ALTER TABLE <name> ALTER [COLUMN] <colname> SET STORAGE <storagemode> */
  | ALTER opt_column ColId SET STORAGE ColId {
  }
  /* ALTER TABLE <name> DROP [COLUMN] IF EXISTS <colname> [RESTRICT|CASCADE] */
  | DROP opt_column IF_P EXISTS ColId opt_drop_behavior {
  }
  /* ALTER TABLE <name> DROP [COLUMN] <colname> [RESTRICT|CASCADE] */
  | DROP opt_column ColId opt_drop_behavior {
  }
  /*
   * ALTER TABLE <name> ALTER [COLUMN] <colname> [SET DATA] TYPE <typename>
   *    [ USING <expression> ]
   */
  | ALTER opt_column ColId opt_set_data TYPE_P Typename opt_collate_clause alter_using {
  }
  /* ALTER FOREIGN TABLE <name> ALTER [COLUMN] <colname> OPTIONS */
  | ALTER opt_column ColId alter_generic_options {
  }
  /* ALTER TABLE <name> ADD CONSTRAINT ... */
  | ADD_P TableConstraint {
  }
  /* ALTER TABLE <name> ALTER CONSTRAINT ... */
  | ALTER CONSTRAINT name ConstraintAttributeSpec {
  }
  /* ALTER TABLE <name> VALIDATE CONSTRAINT ... */
  | VALIDATE CONSTRAINT name {
  }
  /* ALTER TABLE <name> DROP CONSTRAINT IF EXISTS <name> [RESTRICT|CASCADE] */
  | DROP CONSTRAINT IF_P EXISTS name opt_drop_behavior {
  }
  /* ALTER TABLE <name> DROP CONSTRAINT <name> [RESTRICT|CASCADE] */
  | DROP CONSTRAINT name opt_drop_behavior {
  }
  /* ALTER TABLE <name> SET WITH OIDS  */
  | SET WITH OIDS {
  }
  /* ALTER TABLE <name> SET WITHOUT OIDS  */
  | SET WITHOUT OIDS {
  }
  /* ALTER TABLE <name> CLUSTER ON <indexname> */
  | CLUSTER ON name {
  }
  /* ALTER TABLE <name> SET WITHOUT CLUSTER */
  | SET WITHOUT CLUSTER {
  }
  /* ALTER TABLE <name> SET LOGGED  */
  | SET LOGGED {
  }
  /* ALTER TABLE <name> SET UNLOGGED  */
  | SET UNLOGGED {
  }
  /* ALTER TABLE <name> ENABLE TRIGGER <trig> */
  | ENABLE_P TRIGGER name {
  }
  /* ALTER TABLE <name> ENABLE ALWAYS TRIGGER <trig> */
  | ENABLE_P ALWAYS TRIGGER name {
  }
  /* ALTER TABLE <name> ENABLE REPLICA TRIGGER <trig> */
  | ENABLE_P REPLICA TRIGGER name {
  }
  /* ALTER TABLE <name> ENABLE TRIGGER ALL */
  | ENABLE_P TRIGGER ALL {
  }
  /* ALTER TABLE <name> DISABLE TRIGGER <trig> */
  | DISABLE_P TRIGGER name {
  }
  /* ALTER TABLE <name> DISABLE TRIGGER ALL */
  | DISABLE_P TRIGGER ALL {
  }
  /* ALTER TABLE <name> ENABLE RULE <rule> */
  | ENABLE_P RULE name {
  }
  /* ALTER TABLE <name> ENABLE ALWAYS RULE <rule> */
  | ENABLE_P ALWAYS RULE name {
  }
  /* ALTER TABLE <name> ENABLE REPLICA RULE <rule> */
  | ENABLE_P REPLICA RULE name {
  }
  /* ALTER TABLE <name> DISABLE RULE <rule> */
  | DISABLE_P RULE name {
  }
  /* ALTER TABLE <name> INHERIT <parent> */
  | INHERIT qualified_name {
  }
  /* ALTER TABLE <name> NO INHERIT <parent> */
  | NO INHERIT qualified_name {
  }
  /* ALTER TABLE <name> OF <type_name> */
  | OF any_name {
  }
  /* ALTER TABLE <name> NOT OF */
  | NOT OF {
  }
  /* ALTER TABLE <name> OWNER TO RoleSpec */
  | OWNER TO RoleSpec {
  }
  /* ALTER TABLE <name> SET TABLESPACE <tablespacename> */
  | SET TABLESPACE name {
  }
  /* ALTER TABLE <name> SET (...) */
  | SET reloptions {
  }
  /* ALTER TABLE <name> RESET (...) */
  | RESET reloptions {
  }
  /* ALTER TABLE <name> REPLICA IDENTITY  */
  | REPLICA IDENTITY_P replica_identity {
  }
  /* ALTER TABLE <name> ENABLE ROW LEVEL SECURITY */
  | ENABLE_P ROW LEVEL SECURITY {
  }
  /* ALTER TABLE <name> DISABLE ROW LEVEL SECURITY */
  | DISABLE_P ROW LEVEL SECURITY {
  }
  /* ALTER TABLE <name> FORCE ROW LEVEL SECURITY */
  | FORCE ROW LEVEL SECURITY {
  }
  /* ALTER TABLE <name> NO FORCE ROW LEVEL SECURITY */
  | NO FORCE ROW LEVEL SECURITY {
  }
  | alter_generic_options {
  }
;

alter_column_default:
  SET DEFAULT a_expr { $$ = nullptr; }
  | DROP DEFAULT     { $$ = nullptr; }
;

opt_drop_behavior:
  CASCADE             { $$ = DROP_CASCADE; PARSER_CQL_INVALID(@1); }
  | RESTRICT          { $$ = DROP_RESTRICT; PARSER_CQL_INVALID(@1); }
  | /* EMPTY */       { $$ = DROP_RESTRICT; /* default */ }
;

opt_collate_clause:
  COLLATE any_name {
  }
  | /* EMPTY */       { $$ = nullptr; }
;

alter_using:
  USING a_expr        { $$ = nullptr; }
  | /* EMPTY */       { $$ = nullptr; }
;

replica_identity:
  NOTHING {
  }
  | FULL {
  }
  | DEFAULT {
  }
  | USING INDEX name {
  }
;

/*****************************************************************************
 *
 *  ALTER TYPE
 *
 * really variants of the ALTER TABLE subcommands with different spellings
 *****************************************************************************/

AlterCompositeTypeStmt:
  ALTER TYPE_P any_name alter_type_cmds {
  }
;

alter_type_cmds:
  alter_type_cmd                        { }
  | alter_type_cmds ',' alter_type_cmd  { }
;

alter_type_cmd:
  /* ALTER TYPE <name> ADD ATTRIBUTE <coldef> [RESTRICT|CASCADE] */
  ADD_P ATTRIBUTE TableFuncElement opt_drop_behavior {
  }
  /* ALTER TYPE <name> DROP ATTRIBUTE IF EXISTS <attname> [RESTRICT|CASCADE] */
  | DROP ATTRIBUTE IF_P EXISTS ColId opt_drop_behavior {
  }
  /* ALTER TYPE <name> DROP ATTRIBUTE <attname> [RESTRICT|CASCADE] */
  | DROP ATTRIBUTE ColId opt_drop_behavior {
  }
  /* ALTER TYPE <name> ALTER ATTRIBUTE <attname> [SET DATA] TYPE <typename> [RESTRICT|CASCADE] */
  | ALTER ATTRIBUTE ColId opt_set_data TYPE_P Typename opt_collate_clause opt_drop_behavior {
  }
;

/*****************************************************************************
 *
 *    QUERY :
 *        close <portalname>
 *
 *****************************************************************************/

ClosePortalStmt:
  CLOSE cursor_name {}
  | CLOSE ALL {}
;

/*****************************************************************************
 *
 *    QUERY :
 *        COPY relname [(columnList)] FROM/TO file [WITH] [(options)]
 *        COPY ( SELECT ... ) TO file [WITH] [(options)]
 *
 *        where 'file' can be one of:
 *        { PROGRAM 'command' | STDIN | STDOUT | 'filename' }
 *
 *        In the preferred syntax the options are comma-separated
 *        and use generic identifiers instead of keywords.  The pre-9.0
 *        syntax had a hard-wired, space-separated set of options.
 *
 *        Really old syntax, from versions 7.2 and prior:
 *        COPY [ BINARY ] table [ WITH OIDS ] FROM/TO file
 *          [ [ USING ] DELIMITERS 'delimiter' ] ]
 *          [ WITH NULL AS 'null string' ]
 *        This option placement is not supported with COPY (SELECT...).
 *
 *****************************************************************************/

CopyStmt:
  COPY opt_binary qualified_name opt_column_list opt_oids
  copy_from opt_program copy_file_name copy_delimiter opt_with copy_options {}
  | COPY select_with_parens TO opt_program copy_file_name opt_with copy_options {}
;

copy_from:
  FROM                  { $$ = true; }
  | TO                  { $$ = false; }
;

opt_program:
  PROGRAM               { $$ = true; }
  | /* EMPTY */         { $$ = false; }
;

/*
 * copy_file_name NULL indicates stdio is used. Whether stdin or stdout is
 * used depends on the direction. (It really doesn't make sense to copy from
 * stdout. We silently correct the "typo".)    - AY 9/94
 */
copy_file_name:
  Sconst                  { $$ = $1; }
  | STDIN                 { $$ = nullptr; }
  | STDOUT                { $$ = nullptr; }
;

copy_options:
  copy_opt_list                   { $$ = $1; }
  | '(' copy_generic_opt_list ')' { $$ = $2; }
    ;

/* old COPY option syntax */
copy_opt_list:
  copy_opt_list copy_opt_item { }
  | /* EMPTY */               { $$ = nullptr; }
;

copy_opt_item:
  BINARY {}
  | OIDS {}
  | FREEZE {}
  | DELIMITER opt_as Sconst {}
  | NULL_P opt_as Sconst {}
  | CSV {}
  | HEADER_P {}
  | QUOTE opt_as Sconst {}
  | ESCAPE opt_as Sconst {}
  | FORCE QUOTE columnList {}
  | FORCE QUOTE '*' {}
  | FORCE NOT NULL_P columnList {}
  | FORCE NULL_P columnList {}
  | ENCODING Sconst {}
;

/* The following exist for backward compatibility with very old versions */

opt_binary:
  BINARY {}
  | /*EMPTY*/ { $$ = nullptr; }
;

opt_oids:
  WITH OIDS {}
  | /*EMPTY*/  { $$ = nullptr; }
    ;

copy_delimiter:
  opt_using DELIMITERS Sconst { }
  | /*EMPTY*/                 { $$ = nullptr; }
    ;

opt_using:
  USING                   {}
  | /*EMPTY*/             {}
;

/* new COPY option syntax */
copy_generic_opt_list:
  copy_generic_opt_elem {
  }
  | copy_generic_opt_list ',' copy_generic_opt_elem {
  }
;

copy_generic_opt_elem:
  ColLabel copy_generic_opt_arg {
  }
;

copy_generic_opt_arg:
  opt_boolean_or_string                 { }
  | NumericOnly                         { }
  | '*'                                 { }
  | '(' copy_generic_opt_arg_list ')'   { }
  | /* EMPTY */                         { }
;

copy_generic_opt_arg_list:
  copy_generic_opt_arg_list_item {
  }
  | copy_generic_opt_arg_list ',' copy_generic_opt_arg_list_item {
  }
;

/* beware of emitting non-string list elements here; see commands/define.c */
copy_generic_opt_arg_list_item:
  opt_boolean_or_string {}
;

/*****************************************************************************
 *
 *    QUERY :
 *        CREATE TABLE relname AS SelectStmt [ WITH [NO] DATA ]
 *
 *
 * Note: SELECT ... INTO is a now-deprecated alternative for this.
 *
 *****************************************************************************/

CreateAsStmt:
  CREATE OptTemp TABLE create_as_target AS SelectStmt opt_with_data {
  }
  | CREATE OptTemp TABLE IF_P NOT_LA EXISTS create_as_target AS SelectStmt opt_with_data {
  }
;

create_as_target:
  qualified_name opt_column_list opt_table_options OnCommitOption OptTableSpace {
  }
;

opt_with_data:
  WITH DATA_P               { $$ = true; }
  | WITH NO DATA_P          { $$ = false; }
  | /*EMPTY*/               { $$ = true; }
;

/*****************************************************************************
 *
 *    QUERY :
 *        CREATE MATERIALIZED VIEW relname AS SelectStmt
 *
 *****************************************************************************/

CreateMatViewStmt:
  CREATE OptNoLog MATERIALIZED VIEW create_mv_target AS SelectStmt opt_with_data {
  }
  | CREATE OptNoLog MATERIALIZED VIEW IF_P NOT_LA EXISTS create_mv_target AS SelectStmt
  opt_with_data {
  }
;

create_mv_target:
  qualified_name opt_column_list opt_reloptions OptTableSpace {
  }
;

OptNoLog:
  UNLOGGED            { }
  | /*EMPTY*/         { }
;

/*****************************************************************************
 *
 *    QUERY :
 *        REFRESH MATERIALIZED VIEW qualified_name
 *
 *****************************************************************************/

RefreshMatViewStmt:
  REFRESH MATERIALIZED VIEW opt_concurrently qualified_name opt_with_data {
  }
;

/*****************************************************************************
 *
 *    QUERY :
 *        CREATE SEQUENCE seqname
 *        ALTER SEQUENCE seqname
 *
 *****************************************************************************/

CreateSeqStmt:
  CREATE OptTemp SEQUENCE qualified_name OptSeqOptList {
  }
  | CREATE OptTemp SEQUENCE IF_P NOT_LA EXISTS qualified_name OptSeqOptList {
  }
;

AlterSeqStmt:
  ALTER SEQUENCE qualified_name SeqOptList {
  }
  | ALTER SEQUENCE IF_P EXISTS qualified_name SeqOptList {
  }
;

OptSeqOptList:
  SeqOptList                { $$ = $1; }
  | /*EMPTY*/ {
  }
;

SeqOptList:
  SeqOptElem {
  }
  | SeqOptList SeqOptElem {
  }
;

SeqOptElem:
  CACHE NumericOnly {
  }
  | CYCLE {
  }
  | NO CYCLE {
  }
  | INCREMENT opt_by NumericOnly {
  }
  | MAXVALUE NumericOnly {
  }
  | MINVALUE NumericOnly {
  }
  | NO MAXVALUE {
  }
  | NO MINVALUE {
  }
  | OWNED BY any_name {
  }
  | START opt_with NumericOnly {
  }
  | RESTART {
  }
  | RESTART opt_with NumericOnly {
  }
;

opt_by:
  BY            {}
  | /* empty */ {}
;

NumericOnly:
  FCONST {
    // $$ = makeFloat($1);
  }
  | '-' FCONST {
    // $$ = makeFloat($2);
    // doNegateFloat($$);
  }
  | SignedIconst {
    // $$ = makeInteger($1);
  }
;

NumericOnly_list:
  NumericOnly {
    // $$ = list_make1($1);
  }
  | NumericOnly_list ',' NumericOnly {
    // $$ = lappend($1, $3);
  }
;

/*****************************************************************************
 *
 *    QUERIES :
 *        CREATE [OR REPLACE] [TRUSTED] [PROCEDURAL] LANGUAGE ...
 *        DROP [PROCEDURAL] LANGUAGE ...
 *
 *****************************************************************************/

CreatePLangStmt:
  CREATE opt_or_replace opt_trusted opt_procedural LANGUAGE NonReservedWord_or_Sconst {
  }
  | CREATE opt_or_replace opt_trusted opt_procedural LANGUAGE NonReservedWord_or_Sconst
    HANDLER handler_name opt_inline_handler opt_validator {
  }
;

opt_trusted:
  TRUSTED                 { $$ = true; }
  | /*EMPTY*/             { $$ = false; }
;

/* This ought to be just func_name, but that causes reduce/reduce conflicts
 * (CREATE LANGUAGE is the only place where func_name isn't followed by '(').
 * Work around by using simple names, instead.
 */
handler_name:
  name {
    // $$ = list_make1(makeString($1));
  }
  | name attrs {
    // $$ = lcons(makeString($1), $2);
  }
;

opt_inline_handler:
  INLINE_P handler_name         { $$ = $2; }
  | /*EMPTY*/ {
  }
;

validator_clause:
  VALIDATOR handler_name        { $$ = $2; }
  | NO VALIDATOR {
  }
;

opt_validator:
  validator_clause              { $$ = $1; }
  | /*EMPTY*/ {
  }
;

DropPLangStmt:
  DROP opt_procedural LANGUAGE NonReservedWord_or_Sconst opt_drop_behavior {
  }
  | DROP opt_procedural LANGUAGE IF_P EXISTS NonReservedWord_or_Sconst opt_drop_behavior {
  }
;

opt_procedural:
  PROCEDURAL                {}
  | /*EMPTY*/               {}
;

/*****************************************************************************
 *
 *    QUERY:
 *             CREATE TABLESPACE tablespace LOCATION '/path/to/tablespace/'
 *
 *****************************************************************************/

CreateTableSpaceStmt:
  CREATE TABLESPACE name OptTableSpaceOwner LOCATION Sconst opt_reloptions {
  }
;

OptTableSpaceOwner:
  OWNER RoleSpec      { $$ = $2; }
  | /*EMPTY */        { $$ = nullptr; }
;

/*****************************************************************************
 *
 *    QUERY :
 *        DROP TABLESPACE <tablespace>
 *
 *    No need for drop behaviour as we cannot implement dependencies for
 *    objects in other databases; we can only support RESTRICT.
 *
 ****************************************************************************/

DropTableSpaceStmt:
  DROP TABLESPACE name {
  }
  |  DROP TABLESPACE IF_P EXISTS name {
  }
;

/*****************************************************************************
 *
 *    QUERY:
 *             CREATE EXTENSION extension
 *             [ WITH ] [ SCHEMA schema ] [ VERSION version ] [ FROM oldversion ]
 *
 *****************************************************************************/

CreateExtensionStmt:
  CREATE EXTENSION name opt_with create_extension_opt_list {
  }
  | CREATE EXTENSION IF_P NOT_LA EXISTS name opt_with create_extension_opt_list {
  }
;

create_extension_opt_list:
  create_extension_opt_list create_extension_opt_item {
    // $$ = lappend($1, $2);
  }
  | /* EMPTY */ {
  }
;

create_extension_opt_item:
  SCHEMA name {
    // $$ = makeDefElem("schema", (Node *)makeString($2));
  }
  | VERSION_P NonReservedWord_or_Sconst {
    // $$ = makeDefElem("new_version", (Node *)makeString($2));
  }
  | FROM NonReservedWord_or_Sconst {
    // $$ = makeDefElem("old_version", (Node *)makeString($2));
  }
;

/*****************************************************************************
 *
 * ALTER EXTENSION name UPDATE [ TO version ]
 *
 *****************************************************************************/

AlterExtensionStmt:
  ALTER EXTENSION name UPDATE alter_extension_opt_list {
  }
;

alter_extension_opt_list:
  alter_extension_opt_list alter_extension_opt_item {
    // $$ = lappend($1, $2);
  }
  | /* EMPTY */ {
  }
;

alter_extension_opt_item:
  TO NonReservedWord_or_Sconst {
  }
;

/*****************************************************************************
 *
 * ALTER EXTENSION name ADD/DROP object-identifier
 *
 *****************************************************************************/

AlterExtensionContentsStmt:
  ALTER EXTENSION name add_drop AGGREGATE func_name aggr_args {
  }
  | ALTER EXTENSION name add_drop CAST '(' Typename AS Typename ')' {
  }
  | ALTER EXTENSION name add_drop COLLATION any_name {
  }
  | ALTER EXTENSION name add_drop CONVERSION_P any_name {
  }
  | ALTER EXTENSION name add_drop DOMAIN_P Typename {
  }
  | ALTER EXTENSION name add_drop FUNCTION function_with_argtypes {
  }
  | ALTER EXTENSION name add_drop opt_procedural LANGUAGE name {
  }
  | ALTER EXTENSION name add_drop OPERATOR any_operator oper_argtypes {
  }
  | ALTER EXTENSION name add_drop OPERATOR CLASS any_name USING access_method {
  }
  | ALTER EXTENSION name add_drop OPERATOR FAMILY any_name USING access_method {
  }
  | ALTER EXTENSION name add_drop SCHEMA name {
  }
  | ALTER EXTENSION name add_drop EVENT TRIGGER name {
  }
  | ALTER EXTENSION name add_drop TABLE any_name {
  }
  | ALTER EXTENSION name add_drop TEXT_P SEARCH PARSER any_name {
  }
  | ALTER EXTENSION name add_drop TEXT_P SEARCH DICTIONARY any_name {
  }
  | ALTER EXTENSION name add_drop TEXT_P SEARCH TEMPLATE any_name {
  }
  | ALTER EXTENSION name add_drop TEXT_P SEARCH CONFIGURATION any_name {
  }
  | ALTER EXTENSION name add_drop SEQUENCE any_name {
  }
  | ALTER EXTENSION name add_drop VIEW any_name {
  }
  | ALTER EXTENSION name add_drop MATERIALIZED VIEW any_name {
  }
  | ALTER EXTENSION name add_drop FOREIGN TABLE any_name {
  }
  | ALTER EXTENSION name add_drop FOREIGN DATA_P WRAPPER name {
  }
  | ALTER EXTENSION name add_drop SERVER name {
  }
  | ALTER EXTENSION name add_drop TRANSFORM FOR Typename LANGUAGE name {
  }
  | ALTER EXTENSION name add_drop TYPE_P Typename {
  }
;

add_drop:
  ADD_P                   { $$ = +1; }
  | DROP                  { $$ = -1; }
;

/*****************************************************************************
 *
 *    QUERY:
 *             CREATE FOREIGN DATA WRAPPER name options
 *
 *****************************************************************************/

CreateFdwStmt:
  CREATE FOREIGN DATA_P WRAPPER name opt_fdw_options create_generic_options {
  }
;

fdw_option:
  HANDLER handler_name        { }
  | NO HANDLER                { }
  | VALIDATOR handler_name    { }
  | NO VALIDATOR              { }
;

fdw_options:
  fdw_option                  { }
  | fdw_options fdw_option    { }
;

opt_fdw_options:
  fdw_options                 { $$ = $1; }
  | /*EMPTY*/ {
  }
;

/*****************************************************************************
 *
 *    QUERY :
 *        DROP FOREIGN DATA WRAPPER name
 *
 ****************************************************************************/

DropFdwStmt:
  DROP FOREIGN DATA_P WRAPPER name opt_drop_behavior {
  }
  | DROP FOREIGN DATA_P WRAPPER IF_P EXISTS name opt_drop_behavior {
  }
;

/*****************************************************************************
 *
 *    QUERY :
 *        ALTER FOREIGN DATA WRAPPER name options
 *
 ****************************************************************************/

AlterFdwStmt:
  ALTER FOREIGN DATA_P WRAPPER name opt_fdw_options alter_generic_options {
  }
  | ALTER FOREIGN DATA_P WRAPPER name fdw_options {
  }
;

/* Options definition for CREATE FDW, SERVER and USER MAPPING */
create_generic_options:
  /*EMPTY*/ {
  }
  | OPTIONS '(' generic_option_list ')' {
    PARSER_UNSUPPORTED(@1);
  }
;

generic_option_list:
  generic_option_elem {
    // $$ = list_make1($1);
  }
  | generic_option_list ',' generic_option_elem {
    // $$ = lappend($1, $3);
  }
;

/* Options definition for ALTER FDW, SERVER and USER MAPPING */
alter_generic_options:
  OPTIONS '(' alter_generic_option_list ')' {
    $$ = $3;
  }
;

alter_generic_option_list:
  alter_generic_option_elem {
    // $$ = list_make1($1);
  }
  | alter_generic_option_list ',' alter_generic_option_elem {
    // $$ = lappend($1, $3);
  }
;

alter_generic_option_elem:
  generic_option_elem {
  }
  | SET generic_option_elem {
  }
  | ADD_P generic_option_elem {
  }
  | DROP generic_option_name {
  }
;

generic_option_elem:
  generic_option_name generic_option_arg {
  }
;

generic_option_name:
  ColLabel { $$ = $1; }
;

/* We could use def_arg here, but the spec only requires string literals */
generic_option_arg:
  Sconst {
  }
;

/*****************************************************************************
 *
 *    QUERY:
 *             CREATE SERVER name [TYPE] [VERSION] [OPTIONS]
 *
 *****************************************************************************/

CreateForeignServerStmt:
  CREATE SERVER name opt_type opt_foreign_server_version
  FOREIGN DATA_P WRAPPER name create_generic_options {
  }
;

opt_type:
  TYPE_P Sconst           { $$ = $2; }
  | /*EMPTY*/             { $$ = nullptr; }
;

foreign_server_version:
  VERSION_P Sconst        { $$ = $2; }
  | VERSION_P NULL_P      { $$ = nullptr; }
;

opt_foreign_server_version:
  foreign_server_version  { $$ = $1; }
  | /*EMPTY*/             { $$ = nullptr; }
;

/*****************************************************************************
 *
 *    QUERY :
 *        DROP SERVER name
 *
 ****************************************************************************/

DropForeignServerStmt:
  DROP SERVER name opt_drop_behavior {
  }
  |  DROP SERVER IF_P EXISTS name opt_drop_behavior {
  }
;

/*****************************************************************************
 *
 *    QUERY :
 *        ALTER SERVER name [VERSION] [OPTIONS]
 *
 ****************************************************************************/

AlterForeignServerStmt:
  ALTER SERVER name foreign_server_version alter_generic_options {
  }
  | ALTER SERVER name foreign_server_version {
  }
  | ALTER SERVER name alter_generic_options {
  }
;

/*****************************************************************************
 *
 *    QUERY:
 *             CREATE FOREIGN TABLE relname (...) SERVER name (...)
 *
 *****************************************************************************/

CreateForeignTableStmt:
  CREATE FOREIGN TABLE qualified_name '(' OptTableElementList ')'
  OptInherit SERVER name create_generic_options {
  }
  | CREATE FOREIGN TABLE IF_P NOT_LA EXISTS qualified_name '(' OptTableElementList ')'
  OptInherit SERVER name create_generic_options {
  }
;

/*****************************************************************************
 *
 *    QUERY:
 *             ALTER FOREIGN TABLE relname [...]
 *
 *****************************************************************************/

AlterForeignTableStmt:
  ALTER FOREIGN TABLE relation_expr alter_table_cmds {
  }
  | ALTER FOREIGN TABLE IF_P EXISTS relation_expr alter_table_cmds {
  }
;

/*****************************************************************************
 *
 *    QUERY:
 *        IMPORT FOREIGN SCHEMA remote_schema
 *        [ { LIMIT TO | EXCEPT } ( table_list ) ]
 *        FROM SERVER server_name INTO local_schema [ OPTIONS (...) ]
 *
 ****************************************************************************/

ImportForeignSchemaStmt:
  IMPORT_P FOREIGN SCHEMA name import_qualification
  FROM SERVER name INTO name create_generic_options {
  }
;

import_qualification_type:
  LIMIT TO        { $$ = FDW_IMPORT_SCHEMA_LIMIT_TO; }
  | EXCEPT        { $$ = FDW_IMPORT_SCHEMA_EXCEPT; }
;

import_qualification:
  import_qualification_type '(' relation_expr_list ')' {
  }
  | /*EMPTY*/ {
  }
;

/*****************************************************************************
 *
 *    QUERIES:
 *        CREATE POLICY name ON table [FOR cmd] [TO role, ...]
 *          [USING (qual)] [WITH CHECK (with_check)]
 *        ALTER POLICY name ON table [TO role, ...]
 *          [USING (qual)] [WITH CHECK (with_check)]
 *        DROP POLICY name ON table
 *
 *****************************************************************************/

CreatePolicyStmt:
  CREATE POLICY name ON qualified_name RowSecurityDefaultForCmd RowSecurityDefaultToRole
  RowSecurityOptionalExpr RowSecurityOptionalWithCheck {
  }
;

AlterPolicyStmt:
  ALTER POLICY name ON qualified_name RowSecurityOptionalToRole
  RowSecurityOptionalExpr RowSecurityOptionalWithCheck {
  }
;

DropPolicyStmt:
  DROP POLICY name ON any_name opt_drop_behavior {
  }
  | DROP POLICY IF_P EXISTS name ON any_name opt_drop_behavior {
  }
;

RowSecurityOptionalExpr:
  USING '(' a_expr ')'        { $$ = nullptr; }
  | /* EMPTY */               { $$ = nullptr; }
;

RowSecurityOptionalWithCheck:
  WITH CHECK '(' a_expr ')'   { $$ = nullptr; }
  | /* EMPTY */               { $$ = nullptr; }
;

RowSecurityDefaultToRole:
  TO role_list                { $$ = $2; }
  | /* EMPTY */               { }
;

RowSecurityOptionalToRole:
  TO role_list                { $$ = $2; }
  | /* EMPTY */               { $$ = nullptr; }
;

RowSecurityDefaultForCmd:
  FOR row_security_cmd        { $$ = $2; }
  | /* EMPTY */               { $$ = "all"; }
    ;

row_security_cmd:
  ALL                         { $$ = "all"; }
  | SELECT                    { $$ = "select"; }
  | INSERT                    { $$ = "insert"; }
  | UPDATE                    { $$ = "update"; }
  | DELETE_P                  { $$ = "delete"; }
;

/*****************************************************************************
 *
 *    QUERIES :
 *        CREATE TRIGGER ...
 *        DROP TRIGGER ...
 *
 *****************************************************************************/

CreateTrigStmt:
  CREATE TRIGGER name TriggerActionTime TriggerEvents ON
  qualified_name TriggerForSpec TriggerWhen EXECUTE PROCEDURE func_name '(' TriggerFuncArgs ')' {
    PARSER_UNSUPPORTED(@2);
    $$ = nullptr;
  }
  | CREATE CONSTRAINT TRIGGER name AFTER TriggerEvents ON
  qualified_name OptConstrFromTable ConstraintAttributeSpec
  FOR EACH ROW TriggerWhen EXECUTE PROCEDURE func_name '(' TriggerFuncArgs ')' {
    PARSER_UNSUPPORTED(@3);
    $$ = nullptr;
  }
;

TriggerActionTime:
  BEFORE                  { }
  | AFTER                 { }
  | INSTEAD OF            { }
;

TriggerEvents:
  TriggerOneEvent {
    $$ = $1;
  }
  | TriggerEvents OR TriggerOneEvent {
  }
;

TriggerOneEvent:
  INSERT {
  }
  | DELETE_P {
  }
  | UPDATE {
  }
  | UPDATE OF columnList {
  }
  | TRUNCATE {
  }
;

TriggerForSpec:
  FOR TriggerForOptEach TriggerForType {
    $$ = $3;
  }
  | /* EMPTY */ {
  }
;

TriggerForOptEach:
  EACH                  {}
  | /*EMPTY*/           {}
;

TriggerForType:
  ROW                   { $$ = true; }
  | STATEMENT           { $$ = false; }
;

TriggerWhen:
  WHEN '(' a_expr ')'   { $$ = nullptr; }
  | /*EMPTY*/           { $$ = nullptr; }
;

TriggerFuncArgs:
  TriggerFuncArg {
  }
  | TriggerFuncArgs ',' TriggerFuncArg  {
  }
  | /*EMPTY*/ {
  }
;

TriggerFuncArg:
  Iconst {
    // $$ = makeString(psprintf("%d", $1));
  }
  | FCONST {
    // $$ = makeString($1);
  }
  | UCONST {
    // $$ = makeString($1);
  }
  | Sconst {
    // $$ = makeString($1);
  }
  | ColLabel {
    // $$ = makeString($1);
  }
;

OptConstrFromTable:
  FROM qualified_name {
    $$ = nullptr;
  }
  | /*EMPTY*/ {
    $$ = nullptr;
  }
;

ConstraintAttributeSpec:
  /*EMPTY*/ {
    $$ = 0;
  }
  | ConstraintAttributeSpec ConstraintAttributeElem {
    PARSER_UNSUPPORTED(@2);
  }
;

ConstraintAttributeElem:
  NOT DEFERRABLE            { }
  | DEFERRABLE              { }
  | INITIALLY IMMEDIATE     { }
  | INITIALLY DEFERRED      { }
  | NOT VALID               { }
  | NO INHERIT              { }
;

DropTrigStmt:
  DROP TRIGGER name ON any_name opt_drop_behavior {
  }
  | DROP TRIGGER IF_P EXISTS name ON any_name opt_drop_behavior {
  }
;

/*****************************************************************************
 *
 *    QUERIES :
 *        CREATE EVENT TRIGGER ...
 *        ALTER EVENT TRIGGER ...
 *
 *****************************************************************************/

CreateEventTrigStmt:
  CREATE EVENT TRIGGER name ON ColLabel EXECUTE PROCEDURE func_name '(' ')' {
  }
  | CREATE EVENT TRIGGER name ON ColLabel WHEN event_trigger_when_list
  EXECUTE PROCEDURE func_name '(' ')' {
  }
;

event_trigger_when_list:
  event_trigger_when_item {
  }
  | event_trigger_when_list AND event_trigger_when_item {
  }
;

event_trigger_when_item:
  ColId IN_P '(' event_trigger_value_list ')' {
  }
;

event_trigger_value_list:
  SCONST {
  }
  | event_trigger_value_list ',' SCONST {
  }
;

AlterEventTrigStmt:
  ALTER EVENT TRIGGER name enable_trigger {
  }
;

enable_trigger:
  ENABLE_P                { }
  | ENABLE_P REPLICA      { }
  | ENABLE_P ALWAYS       { }
  | DISABLE_P             { }
;

/*****************************************************************************
 *
 *    QUERIES :
 *        CREATE ASSERTION ...
 *        DROP ASSERTION ...
 *
 *****************************************************************************/

CreateAssertStmt:
  CREATE ASSERTION name CHECK '(' a_expr ')' ConstraintAttributeSpec {
  }
;

DropAssertStmt:
  DROP ASSERTION name opt_drop_behavior {
  }
;

/*****************************************************************************
 *
 *    QUERY :
 *        define (aggregate,operator)
 *
 *****************************************************************************/

DefineStmt:
  CREATE AGGREGATE func_name aggr_args definition {
  }
  | CREATE AGGREGATE func_name old_aggr_definition {
  }
  | CREATE OPERATOR any_operator definition {
  }
  | CREATE TEXT_P SEARCH PARSER any_name definition {
  }
  | CREATE TEXT_P SEARCH DICTIONARY any_name definition {
  }
  | CREATE TEXT_P SEARCH TEMPLATE any_name definition {
  }
  | CREATE TEXT_P SEARCH CONFIGURATION any_name definition {
  }
  | CREATE COLLATION any_name definition {
  }
  | CREATE COLLATION any_name FROM any_name {
  }
;

definition:
  '(' def_list ')' {
    $$ = $2;
  }
;

def_list:
  def_elem {
  }
  | def_list ',' def_elem {
  }
;

def_elem:
  ColLabel '=' def_arg {
  }
  | ColLabel {
  }
;

/* Note: any simple identifier will be returned as a type name! */
def_arg:
  func_type             {}
  | reserved_keyword    {}
  | qual_all_Op         {}
  | NumericOnly         {}
  | Sconst              {}
;

old_aggr_definition:
  '(' old_aggr_list ')' {
    $$ = $2;
  }
;

old_aggr_list:
  old_aggr_elem {
  }
  | old_aggr_list ',' old_aggr_elem  {
  }
;

/*
 * Must use IDENT here to avoid reduce/reduce conflicts; fortunately none of
 * the item names needed in old aggregate definitions are likely to become
 * SQL keywords.
 */
old_aggr_elem:
  IDENT '=' def_arg {
  }
;

opt_enum_val_list:
  enum_val_list {
    $$ = $1;
  }
  | /*EMPTY*/ {
  }
;

enum_val_list:
  Sconst {
  }
  | enum_val_list ',' Sconst {
  }
;

/*****************************************************************************
 *
 *  ALTER TYPE enumtype ADD ...
 *
 *****************************************************************************/

AlterEnumStmt:
  ALTER TYPE_P any_name ADD_P VALUE_P opt_if_not_exists Sconst {
  }
  | ALTER TYPE_P any_name ADD_P VALUE_P opt_if_not_exists Sconst BEFORE Sconst {
  }
  | ALTER TYPE_P any_name ADD_P VALUE_P opt_if_not_exists Sconst AFTER Sconst {
  }
;

opt_if_not_exists:
  IF_P NOT_LA EXISTS         { $$ = true; }
  | /* empty */              { $$ = false; }
;

/*****************************************************************************
 *
 *    QUERIES :
 *        CREATE OPERATOR CLASS ...
 *        CREATE OPERATOR FAMILY ...
 *        ALTER OPERATOR FAMILY ...
 *        DROP OPERATOR CLASS ...
 *        DROP OPERATOR FAMILY ...
 *
 *****************************************************************************/

CreateOpClassStmt:
  CREATE OPERATOR CLASS any_name opt_default FOR TYPE_P Typename
  USING access_method opt_opfamily AS opclass_item_list {
  }
;

opclass_item_list:
  opclass_item {
  }
  | opclass_item_list ',' opclass_item {
  }
;

opclass_item:
  OPERATOR Iconst any_operator opclass_purpose opt_recheck {
  }
  | OPERATOR Iconst any_operator oper_argtypes opclass_purpose opt_recheck {
  }
  | FUNCTION Iconst func_name func_args {
  }
  | FUNCTION Iconst '(' type_list ')' func_name func_args {
  }
  | STORAGE Typename {
  }
;

opt_default:
  DEFAULT                 { $$ = true; }
  | /*EMPTY*/             { $$ = false; }
;

opt_opfamily:
  FAMILY any_name         { $$ = $2; }
  | /*EMPTY*/ {
  }
;

opclass_purpose:
  FOR SEARCH {
  }
  | FOR ORDER BY any_name { $$ = $4; }
  | /*EMPTY*/ {
  }
;

opt_recheck:
  RECHECK {
  }
  | /*EMPTY*/             { $$ = false; }
;

CreateOpFamilyStmt:
  CREATE OPERATOR FAMILY any_name USING access_method {
  }
;

AlterOpFamilyStmt:
  ALTER OPERATOR FAMILY any_name USING access_method ADD_P opclass_item_list {
  }
  | ALTER OPERATOR FAMILY any_name USING access_method DROP opclass_drop_list {
  }
;

opclass_drop_list:
  opclass_drop {
  }
  | opclass_drop_list ',' opclass_drop {
  }
;

opclass_drop:
  OPERATOR Iconst '(' type_list ')' {
  }
  | FUNCTION Iconst '(' type_list ')' {
  }
;

DropOpClassStmt:
  DROP OPERATOR CLASS any_name USING access_method opt_drop_behavior {
  }
  | DROP OPERATOR CLASS IF_P EXISTS any_name USING access_method opt_drop_behavior {
  }
;

DropOpFamilyStmt:
  DROP OPERATOR FAMILY any_name USING access_method opt_drop_behavior {
  }
  | DROP OPERATOR FAMILY IF_P EXISTS any_name USING access_method opt_drop_behavior {
  }
;

/*****************************************************************************
 *
 *    QUERY:
 *
 *    DROP OWNED BY username [, username ...] [ RESTRICT | CASCADE ]
 *    REASSIGN OWNED BY username [, username ...] TO username
 *
 *****************************************************************************/
DropOwnedStmt:
  DROP OWNED BY role_list opt_drop_behavior {
  }
;

ReassignOwnedStmt:
  REASSIGN OWNED BY role_list TO RoleSpec {
  }
;

/*****************************************************************************
 *
 *    QUERY:
 *        truncate table relname1, relname2, ...
 *
 *****************************************************************************/

TruncateStmt:
  TRUNCATE opt_table relation_expr_list opt_restart_seqs opt_drop_behavior {
    $$ = MAKE_NODE(@1, PTTruncateStmt, $3);
  }
;

opt_restart_seqs:
  CONTINUE_P IDENTITY_P   { $$ = false; PARSER_CQL_INVALID(@1); }
  | RESTART IDENTITY_P    { $$ = true; PARSER_CQL_INVALID(@1); }
  | /* EMPTY */           { $$ = false; }
;

/*****************************************************************************
 *
 *  The COMMENT ON statement can take different forms based upon the type of
 *  the object associated with the comment. The form of the statement is:
 *
 *  COMMENT ON [ [ CONVERSION | COLLATION | DATABASE | DOMAIN |
 *                 EXTENSION | EVENT TRIGGER | FOREIGN DATA WRAPPER |
 *                 FOREIGN TABLE | INDEX | [PROCEDURAL] LANGUAGE |
 *                 MATERIALIZED VIEW | POLICY | ROLE | SCHEMA | SEQUENCE |
 *                 SERVER | TABLE | TABLESPACE |
 *                 TEXT SEARCH CONFIGURATION | TEXT SEARCH DICTIONARY |
 *                 TEXT SEARCH PARSER | TEXT SEARCH TEMPLATE | TYPE |
 *                 VIEW] <objname> |
 *         AGGREGATE <aggname> (arg1, ...) |
 *         CAST (<src type> AS <dst type>) |
 *         COLUMN <relname>.<colname> |
 *         CONSTRAINT <constraintname> ON <relname> |
 *         CONSTRAINT <constraintname> ON DOMAIN <domainname> |
 *         FUNCTION <funcname> (arg1, arg2, ...) |
 *         LARGE OBJECT <oid> |
 *         OPERATOR <op> (leftoperand_typ, rightoperand_typ) |
 *         OPERATOR CLASS <name> USING <access-method> |
 *         OPERATOR FAMILY <name> USING <access-method> |
 *         RULE <rulename> ON <relname> |
 *         TRIGGER <triggername> ON <relname> ]
 *         IS 'text'
 *
 *****************************************************************************/

CommentStmt:
  COMMENT ON comment_type any_name IS comment_text {
  }
  | COMMENT ON TYPE_P Typename IS comment_text {
  }
  | COMMENT ON DOMAIN_P Typename IS comment_text {
  }
  | COMMENT ON AGGREGATE func_name aggr_args IS comment_text {
  }
  | COMMENT ON FUNCTION func_name func_args IS comment_text {
  }
  | COMMENT ON OPERATOR any_operator oper_argtypes IS comment_text {
  }
  | COMMENT ON CONSTRAINT name ON any_name IS comment_text {
  }
  | COMMENT ON CONSTRAINT name ON DOMAIN_P any_name IS comment_text {
  }
  | COMMENT ON POLICY name ON any_name IS comment_text {
  }
  | COMMENT ON RULE name ON any_name IS comment_text {
  }
  | COMMENT ON RULE name IS comment_text {
  }
  | COMMENT ON TRANSFORM FOR Typename LANGUAGE name IS comment_text {
  }
  | COMMENT ON TRIGGER name ON any_name IS comment_text {
  }
  | COMMENT ON OPERATOR CLASS any_name USING access_method IS comment_text {
  }
  | COMMENT ON OPERATOR FAMILY any_name USING access_method IS comment_text {
  }
  | COMMENT ON LARGE_P OBJECT_P NumericOnly IS comment_text {
  }
  | COMMENT ON CAST '(' Typename AS Typename ')' IS comment_text {
  }
  | COMMENT ON opt_procedural LANGUAGE any_name IS comment_text {
  }
;

comment_type:
  COLUMN                          { $$ = OBJECT_COLUMN; }
  | DATABASE                      { $$ = OBJECT_DATABASE; }
  | SCHEMA                        { $$ = OBJECT_SCHEMA; }
  | INDEX                         { $$ = OBJECT_INDEX; }
  | SEQUENCE                      { $$ = OBJECT_SEQUENCE; }
  | TABLE                         { $$ = OBJECT_TABLE; }
  | VIEW                          { $$ = OBJECT_VIEW; }
  | MATERIALIZED VIEW             { $$ = OBJECT_MATVIEW; }
  | COLLATION                     { $$ = OBJECT_COLLATION; }
  | CONVERSION_P                  { $$ = OBJECT_CONVERSION; }
  | TABLESPACE                    { $$ = OBJECT_TABLESPACE; }
  | EXTENSION                     { $$ = OBJECT_EXTENSION; }
  | ROLE                          { $$ = OBJECT_ROLE; }
  | FOREIGN TABLE                 { $$ = OBJECT_FOREIGN_TABLE; }
  | SERVER                        { $$ = OBJECT_FOREIGN_SERVER; }
  | FOREIGN DATA_P WRAPPER        { $$ = OBJECT_FDW; }
  | EVENT TRIGGER                 { $$ = OBJECT_EVENT_TRIGGER; }
  | TEXT_P SEARCH CONFIGURATION   { $$ = OBJECT_TSCONFIGURATION; }
  | TEXT_P SEARCH DICTIONARY      { $$ = OBJECT_TSDICTIONARY; }
  | TEXT_P SEARCH PARSER          { $$ = OBJECT_TSPARSER; }
  | TEXT_P SEARCH TEMPLATE        { $$ = OBJECT_TSTEMPLATE; }
;

comment_text:
  Sconst                { $$ = $1; }
  | NULL_P              { $$ = nullptr; }
;

/*****************************************************************************
 *
 *  SECURITY LABEL [FOR <provider>] ON <object> IS <label>
 *
 *  As with COMMENT ON, <object> can refer to various types of database
 *  objects (e.g. TABLE, COLUMN, etc.).
 *
 *****************************************************************************/

SecLabelStmt:
  SECURITY LABEL opt_provider ON security_label_type any_name IS security_label {
  }
  | SECURITY LABEL opt_provider ON TYPE_P Typename IS security_label {
  }
  | SECURITY LABEL opt_provider ON DOMAIN_P Typename IS security_label {
  }
  | SECURITY LABEL opt_provider ON AGGREGATE func_name aggr_args IS security_label {
  }
  | SECURITY LABEL opt_provider ON FUNCTION func_name func_args IS security_label {
  }
  | SECURITY LABEL opt_provider ON LARGE_P OBJECT_P NumericOnly IS security_label {
  }
  | SECURITY LABEL opt_provider ON opt_procedural LANGUAGE any_name IS security_label {
  }
;

opt_provider:
  FOR NonReservedWord_or_Sconst { $$ = $2; }
  | /* empty */                 { $$ = nullptr; }
;

security_label_type:
  COLUMN                        { $$ = OBJECT_COLUMN; }
  | DATABASE                    { $$ = OBJECT_DATABASE; }
  | EVENT TRIGGER               { $$ = OBJECT_EVENT_TRIGGER; }
  | FOREIGN TABLE               { $$ = OBJECT_FOREIGN_TABLE; }
  | SCHEMA                      { $$ = OBJECT_SCHEMA; }
  | SEQUENCE                    { $$ = OBJECT_SEQUENCE; }
  | TABLE                       { $$ = OBJECT_TABLE; }
  | ROLE                        { $$ = OBJECT_ROLE; }
  | TABLESPACE                  { $$ = OBJECT_TABLESPACE; }
  | VIEW                        { $$ = OBJECT_VIEW; }
  | MATERIALIZED VIEW           { $$ = OBJECT_MATVIEW; }
;

security_label:
  Sconst        { $$ = $1; }
  | NULL_P      { $$ = nullptr; }
;

/*****************************************************************************
 *
 *    QUERY:
 *      fetch/move
 *
 *****************************************************************************/

FetchStmt:
  FETCH fetch_args {
  }
  | MOVE fetch_args {
  }
;

fetch_args:
  cursor_name {
  }
  | from_in cursor_name {
  }
  | NEXT opt_from_in cursor_name {
  }
  | PRIOR opt_from_in cursor_name {
  }
  | FIRST_P opt_from_in cursor_name {
  }
  | LAST_P opt_from_in cursor_name {
  }
  | ABSOLUTE_P SignedIconst opt_from_in cursor_name {
  }
  | RELATIVE_P SignedIconst opt_from_in cursor_name {
  }
  | SignedIconst opt_from_in cursor_name {
  }
  | ALL opt_from_in cursor_name {
  }
  | FORWARD opt_from_in cursor_name {
  }
  | FORWARD SignedIconst opt_from_in cursor_name {
  }
  | FORWARD ALL opt_from_in cursor_name {
  }
  | BACKWARD opt_from_in cursor_name {
  }
  | BACKWARD SignedIconst opt_from_in cursor_name {
  }
  | BACKWARD ALL opt_from_in cursor_name {
  }
;

from_in:
  FROM                  {}
  | IN_P                {}
;

opt_from_in:
  from_in               {}
  | /* EMPTY */         {}
;

/*****************************************************************************
 *
 * GRANT and REVOKE statements
 *
 *****************************************************************************/
GrantStmt:
  GRANT permissions ON ALL KEYSPACES TO role_name {
    PTQualifiedName::SharedPtr role_node = MAKE_NODE(@1, PTQualifiedName, $7);
    $$ = MAKE_NODE(@1, PTGrantRevokePermission, GrantRevokeStatementType::GRANT,
                   $2, ResourceType::ALL_KEYSPACES, nullptr, role_node);
  }
  | GRANT permissions ON KEYSPACE ColId TO role_name {
    PTQualifiedName::SharedPtr role_node = MAKE_NODE(@1, PTQualifiedName, $7);
    PTQualifiedName::SharedPtr keyspace_node = MAKE_NODE(@1, PTQualifiedName, $5);
    $$ = MAKE_NODE(@1, PTGrantRevokePermission, GrantRevokeStatementType::GRANT,
                   $2, ResourceType::KEYSPACE, keyspace_node, role_node);
  }
  | GRANT permissions ON TABLE qualified_name TO role_name {
    PTQualifiedName::SharedPtr role_node = MAKE_NODE(@1, PTQualifiedName, $7);
    $$ = MAKE_NODE(@1, PTGrantRevokePermission, GrantRevokeStatementType::GRANT,
                   $2, ResourceType::TABLE, $5, role_node);
  }
  | GRANT permissions ON qualified_name TO role_name {
    PTQualifiedName::SharedPtr role_node = MAKE_NODE(@1, PTQualifiedName, $6);
    $$ = MAKE_NODE(@1, PTGrantRevokePermission, GrantRevokeStatementType::GRANT,
                   $2, ResourceType::TABLE, $4, role_node);
  }
  | GRANT permissions ON ALL ROLES TO role_name {
    PTQualifiedName::SharedPtr role_node = MAKE_NODE(@1, PTQualifiedName, $7);
    $$ = MAKE_NODE(@1, PTGrantRevokePermission, GrantRevokeStatementType::GRANT,
                   $2, ResourceType::ALL_ROLES, nullptr , role_node);
  }
  | GRANT permissions ON ROLE role_name TO role_name {
    PTQualifiedName::SharedPtr to_role_node = MAKE_NODE(@1, PTQualifiedName, $7);
    PTQualifiedName::SharedPtr on_role_node = MAKE_NODE(@1, PTQualifiedName, $5);
    $$ = MAKE_NODE(@1, PTGrantRevokePermission, GrantRevokeStatementType::GRANT,
                   $2, ResourceType::ROLE, on_role_node, to_role_node);
  }
;

RevokeStmt:
  REVOKE permissions ON ALL KEYSPACES FROM role_name {
    PTQualifiedName::SharedPtr role_node = MAKE_NODE(@1, PTQualifiedName, $7);
    $$ = MAKE_NODE(@1, PTGrantRevokePermission, GrantRevokeStatementType::REVOKE,
                   $2, ResourceType::ALL_KEYSPACES, nullptr, role_node);
  }
  | REVOKE permissions ON KEYSPACE ColId FROM role_name {
    PTQualifiedName::SharedPtr role_node = MAKE_NODE(@1, PTQualifiedName, $7);
    PTQualifiedName::SharedPtr keyspace_node = MAKE_NODE(@1, PTQualifiedName, $5);
    $$ = MAKE_NODE(@1, PTGrantRevokePermission, GrantRevokeStatementType::REVOKE,
                   $2, ResourceType::KEYSPACE, keyspace_node, role_node);
  }
  | REVOKE permissions ON TABLE qualified_name FROM role_name {
    PTQualifiedName::SharedPtr role_node = MAKE_NODE(@1, PTQualifiedName, $7);
    $$ = MAKE_NODE(@1, PTGrantRevokePermission, GrantRevokeStatementType::REVOKE,
                   $2, ResourceType::TABLE, $5, role_node);
  }
  | REVOKE permissions ON qualified_name FROM role_name {
    PTQualifiedName::SharedPtr role_node = MAKE_NODE(@1, PTQualifiedName, $6);
    $$ = MAKE_NODE(@1, PTGrantRevokePermission, GrantRevokeStatementType::REVOKE,
                   $2, ResourceType::TABLE, $4, role_node);
  }
  | REVOKE permissions ON ALL ROLES FROM role_name {
    PTQualifiedName::SharedPtr role_node = MAKE_NODE(@1, PTQualifiedName, $7);
    $$ = MAKE_NODE(@1, PTGrantRevokePermission, GrantRevokeStatementType::REVOKE,
                   $2, ResourceType::ALL_ROLES, nullptr , role_node);
  }
  | REVOKE permissions ON ROLE role_name FROM role_name {
    PTQualifiedName::SharedPtr to_role_node = MAKE_NODE(@1, PTQualifiedName, $7);
    PTQualifiedName::SharedPtr on_role_node = MAKE_NODE(@1, PTQualifiedName, $5);
    $$ = MAKE_NODE(@1, PTGrantRevokePermission, GrantRevokeStatementType::REVOKE,
                   $2, ResourceType::ROLE, on_role_node, to_role_node);
  }
;

permissions:
  ALL opt_permissions {
    $$ = parser_->MakeString($1);
  }
  | permission opt_permission {
    $$ = $1;
  }
;

permission:
  CREATE {
    $$ = parser_->MakeString($1);
  }
  | ALTER {
    $$ = parser_->MakeString($1);
  }
  | DROP {
    $$ = parser_->MakeString($1);
  }
  | SELECT {
    $$ = parser_->MakeString($1);
  }
  | MODIFY {
    $$ = parser_->MakeString($1);
  }
  | AUTHORIZE {
    $$ = parser_->MakeString($1);
  }
  | DESCRIBE {
    $$ = parser_->MakeString($1);
  }
;

opt_permissions:
  /* EMPTY */ {
  }
  | PERMISSIONS {
  }
;

opt_permission:
  /* EMPTY */ {
  }
  | PERMISSION {
  }
;

/*
 * Privilege names are represented as strings; the validity of the privilege
 * names gets checked at execution.  This is a bit annoying but we have little
 * choice because of the syntactic conflict with lists of role names in
 * GRANT/REVOKE.  What's more, we have to call out in the "privilege"
 * production any reserved keywords that need to be usable as privilege names.
 */

/* either ALL [PRIVILEGES] or a list of individual privileges */
privileges:
  privilege_list {
  }
  | ALL {
  }
  | ALL PRIVILEGES {
  }
  | ALL '(' columnList ')' {
  }
  | ALL PRIVILEGES '(' columnList ')' {
  }
;

privilege_list:
  privilege {
  }
  | privilege_list ',' privilege {
  }
;

privilege:
  SELECT opt_column_list {
  }
  | CREATE opt_column_list {
  }
  | ColId opt_column_list {
  }
;

/* Don't bother trying to fold the first two rules into one using
 * opt_table.  You're going to get conflicts.
 */
privilege_target:
  qualified_name_list {
  }
  | TABLE qualified_name_list {
  }
  | SEQUENCE qualified_name_list {
  }
  | FOREIGN DATA_P WRAPPER name_list {
  }
  | FOREIGN SERVER name_list {
  }
  | FUNCTION function_with_argtypes_list {
  }
  | DATABASE name_list {
  }
  | DOMAIN_P any_name_list {
  }
  | LANGUAGE name_list {
  }
  | LARGE_P OBJECT_P NumericOnly_list {
  }
  | SCHEMA name_list {
  }
  | TABLESPACE name_list {
  }
  | TYPE_P any_name_list {
  }
  | ALL TABLES IN_P SCHEMA name_list {
  }
  | ALL SEQUENCES IN_P SCHEMA name_list {
  }
  | ALL FUNCTIONS IN_P SCHEMA name_list {
  }
;

grantee_list:
  grantee {
  }
  | grantee_list ',' grantee {
  }
;

grantee:
  RoleSpec {
  }
;

opt_grant_grant_option:
  WITH GRANT OPTION        { $$ = true; }
  | /*EMPTY*/              { $$ = false; }
;

function_with_argtypes_list:
  function_with_argtypes {
  }
  | function_with_argtypes_list ',' function_with_argtypes {
  }
;

function_with_argtypes:
  func_name func_args {
  }
;

/*****************************************************************************
 *
 * GRANT and REVOKE ROLE statements
 *
 *****************************************************************************/
/*
GrantRoleStmt:
  GRANT privilege_list TO role_list opt_grant_admin_option opt_granted_by {
  }
;
*/

GrantRoleStmt:
  GRANT role_name TO role_name {
    $$ = MAKE_NODE(@1, PTGrantRevokeRole, GrantRevokeStatementType::GRANT, $2, $4);
  }
;

RevokeRoleStmt:
  REVOKE role_name FROM role_name {
    $$ = MAKE_NODE(@1, PTGrantRevokeRole, GrantRevokeStatementType::REVOKE, $2, $4);
  }
;

/*
opt_grant_admin_option:
  WITH ADMIN OPTION {
    $$ = true;
  }
  | /-*EMPTY*-/ {
    $$ = false;
  }
;
*/

opt_granted_by:
  GRANTED BY RoleSpec {
    $$ = $3;
  }
  | /*EMPTY*/ { $$ = nullptr; }
;

/*****************************************************************************
 *
 * ALTER DEFAULT PRIVILEGES statement
 *
 *****************************************************************************/

AlterDefaultPrivilegesStmt:
  ALTER DEFAULT PRIVILEGES DefACLOptionList DefACLAction {
  }
;

DefACLOptionList:
  DefACLOptionList DefACLOption {
  }
  | /* EMPTY */ {
  }
;

DefACLOption:
  IN_P SCHEMA name_list {
  }
  | FOR ROLE role_list {
  }
  | FOR USER role_list {
  }
;

/*
 * This should match GRANT/REVOKE, except that individual target objects
 * are not mentioned and we only allow a subset of object types.
 */

DefACLAction:
  GRANT privileges ON defacl_privilege_target TO grantee_list opt_grant_grant_option {
  }
  | REVOKE privileges ON defacl_privilege_target FROM grantee_list opt_drop_behavior {
  }
  | REVOKE GRANT OPTION FOR privileges ON defacl_privilege_target
  FROM grantee_list opt_drop_behavior {
  }
;

defacl_privilege_target:
  TABLES        { $$ = ACL_OBJECT_RELATION; }
  | FUNCTIONS   { $$ = ACL_OBJECT_FUNCTION; }
  | SEQUENCES   { $$ = ACL_OBJECT_SEQUENCE; }
  | TYPES_P     { $$ = ACL_OBJECT_TYPE; }
;

/*****************************************************************************
 *
 *    QUERY: CREATE INDEX
 *
 * Note: we cannot put TABLESPACE clause after WHERE clause unless we are
 * willing to make TABLESPACE a fully reserved word.
 *****************************************************************************/

IndexStmt:
  CREATE opt_unique INDEX opt_concurrently opt_index_name ON qualified_name
  access_method_clause '(' index_params ')' opt_include_clause OptTableSpace opt_where_clause
  opt_index_options {
    if ($14 && FLAGS_cql_raise_index_where_clause_error) {
       // WHERE is not supported.
       PARSER_UNSUPPORTED(@1);
    }
    $$ = MAKE_NODE(@1, PTCreateIndex, $2, $5, $7, $10, false, $15, $12);
  }
  | CREATE opt_unique INDEX opt_concurrently IF_P NOT_LA EXISTS opt_index_name ON qualified_name
  access_method_clause '(' index_params ')' opt_include_clause OptTableSpace opt_where_clause
  opt_index_options {
    if ($17 && FLAGS_cql_raise_index_where_clause_error) {
       // WHERE is not supported.
       PARSER_UNSUPPORTED(@1);
    }
    $$ = MAKE_NODE(@1, PTCreateIndex, $2, $8, $10, $13, true, $18, $15);
  }
;

opt_unique:
  UNIQUE                            { $$ = true; }
  | /*EMPTY*/                       { $$ = false; }
;

opt_concurrently:
  CONCURRENTLY                      { PARSER_UNSUPPORTED(@1); }
  | /*EMPTY*/                       { }
;

opt_index_name:
  index_name                        { $$ = $1; }
  | /*EMPTY*/                       { $$ = nullptr; }
;

access_method_clause:
  USING access_method               { PARSER_UNSUPPORTED(@1); }
  | /*EMPTY*/                       { }
;

index_params:
  NestedColumnList                  {
    // Wrap the index column list as a primary key definition for index table creation purpose.
    PTPrimaryKey::SharedPtr pk = MAKE_NODE(@1, PTPrimaryKey, $1);
    $$ = MAKE_NODE(@1, PTListNode, pk);
  }
;

opt_index_options:
  /*EMPTY*/ {
    $$ = nullptr;
  }
  | WITH table_properties {
    $$ = $2;
  }
  | WITH reloptions {
    PARSER_UNSUPPORTED(@1);
  }
;

opt_include_clause:
  /*EMPTY*/ {
    $$ = nullptr;
  }
  | INCLUDE '(' index_column_list ')' {
    $$ = $3;
  }
  | COVERING '(' index_column_list ')' {
    $$ = $3;
  }
;

/*
 * Index attributes can be either simple column references, or arbitrary
 * expressions in parens.  For backwards-compatibility reasons, we allow
 * an expression that's just a function call to be written without parens.
 */
index_elem:
  ColId opt_collate opt_class opt_asc_desc opt_nulls_order {
  }
  | func_expr_windowless opt_collate opt_class opt_asc_desc opt_nulls_order {
  }
  | '(' a_expr ')' opt_collate opt_class opt_asc_desc opt_nulls_order {
  }
;

opt_collate:
  COLLATE any_name {
    $$ = $2;
  }
  | /*EMPTY*/ {
  }
;

opt_class:
  any_name {
    $$ = $1;
  }
  | USING any_name {
    $$ = $2;
  }
  | /*EMPTY*/ {
  }
;

opt_asc_desc:
  ASC             { $$ = PTOrderBy::Direction::kASC; }
  | DESC          { $$ = PTOrderBy::Direction::kDESC; }
  | /*EMPTY*/     { $$ = PTOrderBy::Direction::kASC; }
;

opt_nulls_order:
  NULLS_LA FIRST_P     { $$ = PTOrderBy::NullPlacement::kFIRST; }
  | NULLS_LA LAST_P    { $$ = PTOrderBy::NullPlacement::kLAST; }
  | /*EMPTY*/          { $$ = PTOrderBy::NullPlacement::kFIRST; }
;

/*****************************************************************************
 *
 *    QUERY:
 *        create [or replace] function <fname>
 *            [(<type-1> { , <type-n>})]
 *            returns <type-r>
 *            as <filename or code in language as appropriate>
 *            language <lang> [with parameters]
 *
 *****************************************************************************/

CreateFunctionStmt:
  CREATE opt_or_replace FUNCTION func_name func_args_with_defaults
  RETURNS func_return createfunc_opt_list opt_definition {
    PARSER_UNSUPPORTED(@3);
  }
  | CREATE opt_or_replace FUNCTION func_name func_args_with_defaults
  RETURNS TABLE '(' table_func_column_list ')' createfunc_opt_list opt_definition {
    PARSER_UNSUPPORTED(@3);
  }
  | CREATE opt_or_replace FUNCTION func_name func_args_with_defaults
  createfunc_opt_list opt_definition {
    PARSER_UNSUPPORTED(@3);
  }
;

opt_or_replace:
  OR REPLACE                { $$ = true; }
  | /*EMPTY*/               { $$ = false; }
;

func_args:
  '(' func_args_list ')'    { $$ = $2; }
  | '(' ')' {
  }
;

func_args_list:
  func_arg {
  }
  | func_args_list ',' func_arg {
  }
;

/*
 * func_args_with_defaults is separate because we only want to accept
 * defaults in CREATE FUNCTION, not in ALTER etc.
 */
func_args_with_defaults:
  '(' func_args_with_defaults_list ')' {
    $$ = $2;
  }
  | '(' ')' {
  }
;

func_args_with_defaults_list:
  func_arg_with_default {
  }
  | func_args_with_defaults_list ',' func_arg_with_default {
  }
;

/*
 * The style with arg_class first is SQL99 standard, but Oracle puts
 * param_name first; accept both since it's likely people will try both
 * anyway.  Don't bother trying to save productions by letting arg_class
 * have an empty alternative ... you'll get shift/reduce conflicts.
 *
 * We can catch over-specified arguments here if we want to,
 * but for now better to silently swallow typmod, etc.
 * - thomas 2000-03-22
 */
func_arg:
  arg_class param_name func_type {
  }
  | param_name arg_class func_type {
  }
  | param_name func_type {
  }
  | arg_class func_type {
  }
  | func_type {
  }
;

/* INOUT is SQL99 standard, IN OUT is for Oracle compatibility */
arg_class:
  IN_P                { $$ = FUNC_PARAM_IN; }
  | OUT_P             { $$ = FUNC_PARAM_OUT; }
  | INOUT             { $$ = FUNC_PARAM_INOUT; }
  | IN_P OUT_P        { $$ = FUNC_PARAM_INOUT; }
  | VARIADIC          { $$ = FUNC_PARAM_VARIADIC; }
;

/*
 * Ideally param_name should be ColId, but that causes too many conflicts.
 */
param_name:
  type_function_name
;

func_return:
 func_type {}
;

/*
 * We would like to make the %TYPE productions here be ColId attrs etc,
 * but that causes reduce/reduce conflicts.  type_function_name
 * is next best choice.
 */
func_type:
  Typename {
  }
;

func_arg_with_default:
  func_arg {
  }
  | func_arg DEFAULT a_expr {
  }
  | func_arg '=' a_expr {
  }
;

/* Aggregate args can be most things that function args can be */
aggr_arg:
  func_arg {
  }
;

/*
 * The SQL standard offers no guidance on how to declare aggregate argument
 * lists, since it doesn't have CREATE AGGREGATE etc.  We accept these cases:
 *
 * (*)                  - normal agg with no args
 * (aggr_arg,...)           - normal agg with args
 * (ORDER BY aggr_arg,...)        - ordered-set agg with no direct args
 * (aggr_arg,... ORDER BY aggr_arg,...) - ordered-set agg with direct args
 *
 * The zero-argument case is spelled with '*' for consistency with COUNT(*).
 *
 * An additional restriction is that if the direct-args list ends in a
 * VARIADIC item, the ordered-args list must contain exactly one item that
 * is also VARIADIC with the same type.  This allows us to collapse the two
 * VARIADIC items into one, which is necessary to represent the aggregate in
 * pg_proc.  We check this at the grammar stage so that we can return a list
 * in which the second VARIADIC item is already discarded, avoiding extra work
 * in cases such as DROP AGGREGATE.
 *
 * The return value of this production is a two-element list, in which the
 * first item is a sublist of FunctionParameter nodes (with any duplicate
 * VARIADIC item already dropped, as per above) and the second is an integer
 * Value node, containing -1 if there was no ORDER BY and otherwise the number
 * of argument declarations before the ORDER BY.  (If this number is equal
 * to the first sublist's length, then we dropped a duplicate VARIADIC item.)
 * This representation is passed as-is to CREATE AGGREGATE; for operations
 * on existing aggregates, we can just apply extractArgTypes to the first
 * sublist.
 */
aggr_args:
  '(' '*' ')' {
  }
  | '(' aggr_args_list ')' {
  }
  | '(' ORDER BY aggr_args_list ')' {
  }
  | '(' aggr_args_list ORDER BY aggr_args_list ')' {
  }
;

aggr_args_list:
  aggr_arg {
  }
  | aggr_args_list ',' aggr_arg {
  }
;

createfunc_opt_list:
  /* Must be at least one to prevent conflict */
  createfunc_opt_item {
  }
  | createfunc_opt_list createfunc_opt_item {
  }
;

/*
 * Options common to both CREATE FUNCTION and ALTER FUNCTION
 */
common_func_opt_item:
  CALLED ON NULL_P INPUT_P {
  }
  | RETURNS NULL_P ON NULL_P INPUT_P {
  }
  | STRICT_P {
  }
  | IMMUTABLE {
  }
  | STABLE {
  }
  | VOLATILE {
  }
  | EXTERNAL SECURITY DEFINER {
  }
  | EXTERNAL SECURITY INVOKER {
  }
  | SECURITY DEFINER {
  }
  | SECURITY INVOKER {
  }
  | LEAKPROOF {
  }
  | NOT LEAKPROOF {
  }
  | COST NumericOnly {
  }
  | ROWS NumericOnly {
  }
  | FunctionSetResetClause {
  }
;

createfunc_opt_item:
  AS func_as {
  }
  | LANGUAGE NonReservedWord_or_Sconst {
  }
  | TRANSFORM transform_type_list {
  }
  | WINDOW {
  }
  | common_func_opt_item {
  }
;

func_as:
  Sconst {
  }
  | Sconst ',' Sconst {
  }
;

transform_type_list:
  FOR TYPE_P Typename {
  }
  | transform_type_list ',' FOR TYPE_P Typename {
  }
;

opt_definition:
  /*EMPTY*/ {
  }
  | WITH definition {
    PARSER_UNSUPPORTED(@1);
    $$ = $2;
  }
;

table_func_column:
  param_name func_type {
  }
;

table_func_column_list:
  table_func_column {
  }
  | table_func_column_list ',' table_func_column {
  }
;

/*****************************************************************************
 * ALTER FUNCTION
 *
 * RENAME and OWNER subcommands are already provided by the generic
 * ALTER infrastructure, here we just specify alterations that can
 * only be applied to functions.
 *
 *****************************************************************************/
AlterFunctionStmt:
  ALTER FUNCTION function_with_argtypes alterfunc_opt_list opt_restrict {
  }
;

alterfunc_opt_list:
  /* At least one option must be specified */
  common_func_opt_item {
  }
  | alterfunc_opt_list common_func_opt_item {
  }
;

/* Ignored, merely for SQL compliance */
opt_restrict:
  RESTRICT
  | /* EMPTY */
;

/*****************************************************************************
 *
 *    QUERY:
 *
 *    DROP FUNCTION funcname (arg1, arg2, ...) [ RESTRICT | CASCADE ]
 *    DROP AGGREGATE aggname (arg1, ...) [ RESTRICT | CASCADE ]
 *    DROP OPERATOR opname (leftoperand_typ, rightoperand_typ) [ RESTRICT | CASCADE ]
 *
 *****************************************************************************/

RemoveFuncStmt:
  DROP FUNCTION func_name func_args opt_drop_behavior {
  }
  | DROP FUNCTION IF_P EXISTS func_name func_args opt_drop_behavior {
  }
;

RemoveAggrStmt:
  DROP AGGREGATE func_name aggr_args opt_drop_behavior {
  }
  | DROP AGGREGATE IF_P EXISTS func_name aggr_args opt_drop_behavior {
  }
;

RemoveOperStmt:
  DROP OPERATOR any_operator oper_argtypes opt_drop_behavior {
  }
  | DROP OPERATOR IF_P EXISTS any_operator oper_argtypes opt_drop_behavior {
  }
;

oper_argtypes:
  '(' Typename ')' {
  }
  | '(' Typename ',' Typename ')' {
  }
  | '(' NONE ',' Typename ')' {                                                    /* left unary */
  }
  | '(' Typename ',' NONE ')' {                                                   /* right unary */
  }
;

any_operator:
  all_Op {
  }
  | ColId '.' any_operator {
  }
;

/*****************************************************************************
 *
 *    DO <anonymous code block> [ LANGUAGE language ]
 *
 * We use a DefElem list for future extensibility, and to allow flexibility
 * in the clause order.
 *
 *****************************************************************************/

DoStmt:
  DO dostmt_opt_list {
  }
;

dostmt_opt_list:
  dostmt_opt_item {
  }
  | dostmt_opt_list dostmt_opt_item {
  }
;

dostmt_opt_item:
  Sconst {
  }
  | LANGUAGE NonReservedWord_or_Sconst {
  }
;

/*****************************************************************************
 *
 *    CREATE CAST / DROP CAST
 *
 *****************************************************************************/

CreateCastStmt:
  CREATE CAST '(' Typename AS Typename ')'
  WITH FUNCTION function_with_argtypes cast_context {
  }
  | CREATE CAST '(' Typename AS Typename ')'
  WITHOUT FUNCTION cast_context {
  }
  | CREATE CAST '(' Typename AS Typename ')'
  WITH INOUT cast_context {
  }
;

cast_context:
  AS IMPLICIT_P             { }
  | AS ASSIGNMENT           { }
  | /*EMPTY*/               { }
;

DropCastStmt:
  DROP CAST opt_if_exists '(' Typename AS Typename ')' opt_drop_behavior {
  }
;

opt_if_exists:
  IF_P EXISTS               { $$ = true; }
  | /*EMPTY*/               { $$ = false; }
;

/*****************************************************************************
 *
 *    CREATE TRANSFORM / DROP TRANSFORM
 *
 *****************************************************************************/

CreateTransformStmt:
  CREATE opt_or_replace TRANSFORM FOR Typename LANGUAGE name '(' transform_element_list ')' {
  }
;

transform_element_list:
  FROM SQL_P WITH FUNCTION function_with_argtypes ','
  TO SQL_P WITH FUNCTION function_with_argtypes {
  }
  | TO SQL_P WITH FUNCTION function_with_argtypes ','
  FROM SQL_P WITH FUNCTION function_with_argtypes {
  }
  | FROM SQL_P WITH FUNCTION function_with_argtypes {
  }
  | TO SQL_P WITH FUNCTION function_with_argtypes {
  }
;

DropTransformStmt:
  DROP TRANSFORM opt_if_exists FOR Typename LANGUAGE name opt_drop_behavior {
  }
;

/*****************************************************************************
 *
 *    QUERY:
 *
 *    REINDEX [ (options) ] type <name>
 *****************************************************************************/

ReindexStmt:
  REINDEX reindex_target_type qualified_name {
  }
  | REINDEX reindex_target_multitable name {
  }
  | REINDEX '(' reindex_option_list ')' reindex_target_type qualified_name {
  }
  | REINDEX '(' reindex_option_list ')' reindex_target_multitable name {
  }
;

reindex_target_type:
  INDEX             { $$ = REINDEX_OBJECT_INDEX; }
  | TABLE           { $$ = REINDEX_OBJECT_TABLE; }
;
reindex_target_multitable:
  SCHEMA            { $$ = REINDEX_OBJECT_SCHEMA; }
  | SYSTEM_P        { $$ = REINDEX_OBJECT_SYSTEM; }
  | DATABASE        { $$ = REINDEX_OBJECT_DATABASE; }
;
reindex_option_list:
  reindex_option_elem {
  }
  | reindex_option_list ',' reindex_option_elem {
  }
;
reindex_option_elem:
  VERBOSE           { $$ = REINDEXOPT_VERBOSE; }
;

/*****************************************************************************
 *
 * ALTER TABLESPACE
 *
 *****************************************************************************/

AlterTblSpcStmt:
  ALTER TABLESPACE name SET reloptions {
  }
  | ALTER TABLESPACE name RESET reloptions {
  }
;

/*****************************************************************************
 *
 * ALTER THING name RENAME TO newname
 *
 *****************************************************************************/

RenameStmt:
  ALTER AGGREGATE func_name aggr_args RENAME TO name {
  }
  | ALTER COLLATION any_name RENAME TO name {
  }
  | ALTER CONVERSION_P any_name RENAME TO name {
  }
  | ALTER DATABASE database_name RENAME TO database_name {
  }
  | ALTER DOMAIN_P any_name RENAME TO name {
  }
  | ALTER DOMAIN_P any_name RENAME CONSTRAINT name TO name {
  }
  | ALTER FOREIGN DATA_P WRAPPER name RENAME TO name {
  }
  | ALTER FUNCTION function_with_argtypes RENAME TO name {
  }
  | ALTER opt_procedural LANGUAGE name RENAME TO name {
  }
  | ALTER OPERATOR CLASS any_name USING access_method RENAME TO name {
  }
  | ALTER OPERATOR FAMILY any_name USING access_method RENAME TO name {
  }
  | ALTER POLICY name ON qualified_name RENAME TO name {
  }
  | ALTER POLICY IF_P EXISTS name ON qualified_name RENAME TO name {
  }
  | ALTER SCHEMA name RENAME TO name {
  }
  | ALTER SERVER name RENAME TO name {
  }
  | ALTER TABLE relation_expr RENAME TO name {
  }
  | ALTER TABLE IF_P EXISTS relation_expr RENAME TO name {
  }
  | ALTER SEQUENCE qualified_name RENAME TO name {
  }
  | ALTER SEQUENCE IF_P EXISTS qualified_name RENAME TO name {
  }
  | ALTER VIEW qualified_name RENAME TO name {
  }
  | ALTER VIEW IF_P EXISTS qualified_name RENAME TO name {
  }
  | ALTER MATERIALIZED VIEW qualified_name RENAME TO name {
  }
  | ALTER MATERIALIZED VIEW IF_P EXISTS qualified_name RENAME TO name {
  }
  | ALTER INDEX qualified_name RENAME TO name {
  }
  | ALTER INDEX IF_P EXISTS qualified_name RENAME TO name {
  }
  | ALTER FOREIGN TABLE relation_expr RENAME TO name {
  }
  | ALTER FOREIGN TABLE IF_P EXISTS relation_expr RENAME TO name {
  }
  | ALTER TABLE IF_P EXISTS relation_expr RENAME opt_column name TO name {
  }
  | ALTER MATERIALIZED VIEW qualified_name RENAME opt_column name TO name {
  }
  | ALTER MATERIALIZED VIEW IF_P EXISTS qualified_name RENAME opt_column name TO name {
  }
  | ALTER TABLE relation_expr RENAME CONSTRAINT name TO name {
  }
  | ALTER TABLE IF_P EXISTS relation_expr RENAME CONSTRAINT name TO name {
  }
  | ALTER FOREIGN TABLE relation_expr RENAME opt_column name TO name {
  }
  | ALTER FOREIGN TABLE IF_P EXISTS relation_expr RENAME opt_column name TO name {
  }
  | ALTER RULE name ON qualified_name RENAME TO name {
  }
  | ALTER TRIGGER name ON qualified_name RENAME TO name {
  }
  | ALTER EVENT TRIGGER name RENAME TO name {
  }
  | ALTER USER RoleId RENAME TO RoleId {
  }
  | ALTER TABLESPACE name RENAME TO name {
  }
  | ALTER TEXT_P SEARCH PARSER any_name RENAME TO name {
  }
  | ALTER TEXT_P SEARCH DICTIONARY any_name RENAME TO name {
  }
  | ALTER TEXT_P SEARCH TEMPLATE any_name RENAME TO name {
  }
  | ALTER TEXT_P SEARCH CONFIGURATION any_name RENAME TO name {
  }
  | ALTER TYPE_P any_name RENAME TO name {
  }
  | ALTER TYPE_P any_name RENAME ATTRIBUTE name TO name opt_drop_behavior {
  }
;

opt_column:
  COLUMN                    { $$ = 1; }
  | /*EMPTY*/               { $$ = 0; }
;

opt_set_data:
  SET DATA_P                { $$ = 1; }
  | /*EMPTY*/               { $$ = 0; }
;

/*****************************************************************************
 *
 * ALTER THING name SET SCHEMA name
 *
 *****************************************************************************/

AlterObjectSchemaStmt:
  ALTER AGGREGATE func_name aggr_args SET SCHEMA name {
  }
  | ALTER COLLATION any_name SET SCHEMA name {
  }
  | ALTER CONVERSION_P any_name SET SCHEMA name {
  }
  | ALTER DOMAIN_P any_name SET SCHEMA name {
  }
  | ALTER EXTENSION any_name SET SCHEMA name {
  }
  | ALTER FUNCTION function_with_argtypes SET SCHEMA name {
  }
  | ALTER OPERATOR any_operator oper_argtypes SET SCHEMA name {
  }
  | ALTER OPERATOR CLASS any_name USING access_method SET SCHEMA name {
  }
  | ALTER OPERATOR FAMILY any_name USING access_method SET SCHEMA name {
  }
  | ALTER TABLE relation_expr SET SCHEMA name {
  }
  | ALTER TABLE IF_P EXISTS relation_expr SET SCHEMA name {
  }
  | ALTER TEXT_P SEARCH PARSER any_name SET SCHEMA name {
  }
  | ALTER TEXT_P SEARCH DICTIONARY any_name SET SCHEMA name {
  }
  | ALTER TEXT_P SEARCH TEMPLATE any_name SET SCHEMA name {
  }
  | ALTER TEXT_P SEARCH CONFIGURATION any_name SET SCHEMA name {
  }
  | ALTER SEQUENCE qualified_name SET SCHEMA name {
  }
  | ALTER SEQUENCE IF_P EXISTS qualified_name SET SCHEMA name {
  }
  | ALTER VIEW qualified_name SET SCHEMA name {
  }
  | ALTER VIEW IF_P EXISTS qualified_name SET SCHEMA name {
  }
  | ALTER MATERIALIZED VIEW qualified_name SET SCHEMA name {
  }
  | ALTER MATERIALIZED VIEW IF_P EXISTS qualified_name SET SCHEMA name {
  }
  | ALTER FOREIGN TABLE relation_expr SET SCHEMA name {
  }
  | ALTER FOREIGN TABLE IF_P EXISTS relation_expr SET SCHEMA name {
  }
  | ALTER TYPE_P any_name SET SCHEMA name {
  }
;

/*****************************************************************************
 *
 * ALTER THING name OWNER TO newname
 *
 *****************************************************************************/

AlterOwnerStmt:
  ALTER AGGREGATE func_name aggr_args OWNER TO RoleSpec {
  }
  | ALTER COLLATION any_name OWNER TO RoleSpec {
  }
  | ALTER CONVERSION_P any_name OWNER TO RoleSpec {
  }
  | ALTER DATABASE database_name OWNER TO RoleSpec {
  }
  | ALTER DOMAIN_P any_name OWNER TO RoleSpec {
  }
  | ALTER FUNCTION function_with_argtypes OWNER TO RoleSpec {
  }
  | ALTER opt_procedural LANGUAGE name OWNER TO RoleSpec {
  }
  | ALTER LARGE_P OBJECT_P NumericOnly OWNER TO RoleSpec {
  }
  | ALTER OPERATOR any_operator oper_argtypes OWNER TO RoleSpec {
  }
  | ALTER OPERATOR CLASS any_name USING access_method OWNER TO RoleSpec {
  }
  | ALTER OPERATOR FAMILY any_name USING access_method OWNER TO RoleSpec {
  }
  | ALTER SCHEMA name OWNER TO RoleSpec {
  }
  | ALTER TYPE_P any_name OWNER TO RoleSpec {
  }
  | ALTER TABLESPACE name OWNER TO RoleSpec {
  }
  | ALTER TEXT_P SEARCH DICTIONARY any_name OWNER TO RoleSpec {
  }
  | ALTER TEXT_P SEARCH CONFIGURATION any_name OWNER TO RoleSpec {
  }
  | ALTER FOREIGN DATA_P WRAPPER name OWNER TO RoleSpec {
  }
  | ALTER SERVER name OWNER TO RoleSpec {
  }
  | ALTER EVENT TRIGGER name OWNER TO RoleSpec {
  }
;

/*****************************************************************************
 *
 *    QUERY:  Define Rewrite Rule
 *
 *****************************************************************************/

RuleStmt:
  CREATE opt_or_replace RULE name AS ON event TO qualified_name opt_where_clause
  DO opt_instead RuleActionList {
    PARSER_UNSUPPORTED(@3);
  }
;

RuleActionList:
  NOTHING {
    $$ = nullptr;
  }
  | RuleActionStmt {
    $$ = nullptr;
  }
  | '(' RuleActionMulti ')' {
    $$ = nullptr;
  }
;

/* the thrashing around here is to discard "empty" statements... */
RuleActionMulti:
  RuleActionMulti ';' RuleActionStmtOrEmpty {
    $$ = nullptr;
  }
  | RuleActionStmtOrEmpty {
    $$ = nullptr;
  }
;

RuleActionStmt:
  SelectStmt                { $$ = nullptr; }
  | InsertStmt              { $$ = nullptr; }
  | UpdateStmt              { $$ = nullptr; }
  | DeleteStmt              { $$ = nullptr; }
  | NotifyStmt              { $$ = nullptr; }
;

RuleActionStmtOrEmpty:
  RuleActionStmt              { $$ = nullptr; }
  | /*EMPTY*/                 { $$ = nullptr; }
;

event:
  SELECT                  { $$ = CMD_SELECT; }
  | UPDATE                { $$ = CMD_UPDATE; }
  | DELETE_P              { $$ = CMD_DELETE; }
  | INSERT                { $$ = CMD_INSERT; }
;

opt_instead:
  INSTEAD                 { $$ = true; }
  | ALSO                  { $$ = false; }
  | /*EMPTY*/             { $$ = false; }
;

DropRuleStmt:
  DROP RULE name ON any_name opt_drop_behavior {
  }
  | DROP RULE IF_P EXISTS name ON any_name opt_drop_behavior {
  }
;

/*****************************************************************************
 *
 *    QUERY:
 *        NOTIFY <identifier> can appear both in rule bodies and
 *        as a query-level command
 *
 *****************************************************************************/

NotifyStmt:
  NOTIFY ColId notify_payload {
  }
;

notify_payload:
  ',' Sconst              { $$ = $2; }
  | /*EMPTY*/             { $$ = nullptr; }
;

ListenStmt:
  LISTEN ColId {
  }
;

UnlistenStmt:
  UNLISTEN ColId {
  }
  | UNLISTEN '*' {
  }
;

/*****************************************************************************
 *
 *    Transactions:
 *
 *    START TRANSACTION / COMMIT / ROLLBACK
 *    (also older versions ABORT)
 *
 *****************************************************************************/

TransactionStmt:
  ABORT_P opt_transaction {
    PARSER_UNSUPPORTED(@1);
  }
  | START TRANSACTION transaction_mode_list_or_empty {
    $$ = MAKE_NODE(@1, PTStartTransaction);
  }
  | COMMIT opt_transaction {
    $$ = MAKE_NODE(@1, PTCommit);
  }
  | ROLLBACK opt_transaction {
    PARSER_UNSUPPORTED(@1);
  }
  | SAVEPOINT ColId {
    PARSER_UNSUPPORTED(@1);
  }
  | RELEASE SAVEPOINT ColId {
    PARSER_UNSUPPORTED(@1);
  }
  | RELEASE ColId {
    PARSER_UNSUPPORTED(@1);
  }
  | ROLLBACK opt_transaction TO SAVEPOINT ColId {
    PARSER_UNSUPPORTED(@1);
  }
  | ROLLBACK opt_transaction TO ColId {
    PARSER_UNSUPPORTED(@1);
  }
  | PREPARE TRANSACTION Sconst {
    PARSER_UNSUPPORTED(@1);
  }
  | COMMIT PREPARED Sconst {
    PARSER_UNSUPPORTED(@1);
  }
  | ROLLBACK PREPARED Sconst {
    PARSER_UNSUPPORTED(@1);
  }
;

opt_transaction:
  WORK                      { PARSER_UNSUPPORTED(@1); }
  | TRANSACTION             {}
  | /*EMPTY*/               {}
;

transaction_mode_item:
  ISOLATION LEVEL iso_level {
  }
  | READ ONLY {
  }
  | READ WRITE {
  }
  | DEFERRABLE {
  }
  | NOT DEFERRABLE {
  }
;

/* Syntax with commas is SQL-spec, without commas is Postgres historical */
transaction_mode_list:
  transaction_mode_item {
  }
  | transaction_mode_list ',' transaction_mode_item {
  }
  | transaction_mode_list transaction_mode_item {
  }
;

transaction_mode_list_or_empty:
  transaction_mode_list   { PARSER_UNSUPPORTED(@1); }
  | /* EMPTY */ {
  }
;

/*****************************************************************************
 *
 *  QUERY:
 *    CREATE [ OR REPLACE ] [ TEMP ] VIEW <viewname> '('target-list ')'
 *      AS <query> [ WITH [ CASCADED | LOCAL ] CHECK OPTION ]
 *
 *****************************************************************************/

ViewStmt:
  CREATE OptTemp VIEW qualified_name opt_column_list opt_reloptions
  AS SelectStmt opt_check_option {
  }
  | CREATE OR REPLACE OptTemp VIEW qualified_name opt_column_list opt_reloptions
  AS SelectStmt opt_check_option {
  }
  | CREATE OptTemp RECURSIVE VIEW qualified_name '(' columnList ')' opt_reloptions
  AS SelectStmt opt_check_option {
  }
  | CREATE OR REPLACE OptTemp RECURSIVE VIEW qualified_name '(' columnList ')' opt_reloptions
  AS SelectStmt opt_check_option {
  }
;

opt_check_option:
  WITH CHECK OPTION             { $$ = CASCADED_CHECK_OPTION; }
  | WITH CASCADED CHECK OPTION  { $$ = CASCADED_CHECK_OPTION; }
  | WITH LOCAL CHECK OPTION     { $$ = LOCAL_CHECK_OPTION; }
  | /* EMPTY */                 { $$ = NO_CHECK_OPTION; }
;

/*****************************************************************************
 *
 *    QUERY:
 *        LOAD "filename"
 *
 *****************************************************************************/

LoadStmt:
  LOAD file_name {
  }
;

/*****************************************************************************
 *
 *    CREATE DATABASE
 *
 *****************************************************************************/

CreatedbStmt:
  CREATE DATABASE database_name opt_with createdb_opt_list {
  }
;

createdb_opt_list:
  createdb_opt_items        { $$ = $1; }
  | /* EMPTY */ {
  }
;

createdb_opt_items:
  createdb_opt_item                       { }
  | createdb_opt_items createdb_opt_item  { }
;

createdb_opt_item:
  createdb_opt_name opt_equal SignedIconst {
  }
  | createdb_opt_name opt_equal opt_boolean_or_string {
  }
  | createdb_opt_name opt_equal DEFAULT {
  }
;

/*
 * Ideally we'd use ColId here, but that causes shift/reduce conflicts against
 * the ALTER DATABASE SET/RESET syntaxes.  Instead call out specific keywords
 * we need, and allow IDENT so that database option names don't have to be
 * parser keywords unless they are already keywords for other reasons.
 *
 * XXX this coding technique is fragile since if someone makes a formerly
 * non-keyword option name into a keyword and forgets to add it here, the
 * option will silently break.  Best defense is to provide a regression test
 * exercising every such option, at least at the syntax level.
 */
createdb_opt_name:
  IDENT                 { $$ = $1; }
  | CONNECTION LIMIT    { $$ = parser_->MakeString("connection_limit"); }
  | ENCODING            { $$ = parser_->MakeString($1); }
  | LOCATION            { $$ = parser_->MakeString($1); }
  | OWNER               { $$ = parser_->MakeString($1); }
  | TABLESPACE          { $$ = parser_->MakeString($1); }
  | TEMPLATE            { $$ = parser_->MakeString($1); }
;

/*
 *  Though the equals sign doesn't match other WITH options, pg_dump uses
 *  equals for backward compatibility, and it doesn't seem worth removing it.
 */
opt_equal:
  '='                   {}
  | /*EMPTY*/           {}
;

/*****************************************************************************
 *
 *    ALTER DATABASE
 *
 *****************************************************************************/

AlterDatabaseStmt:
  ALTER DATABASE database_name WITH createdb_opt_list {
  }
  | ALTER DATABASE database_name createdb_opt_list {
  }
  | ALTER DATABASE database_name SET TABLESPACE name {
  }
;

AlterDatabaseSetStmt:
  ALTER DATABASE database_name SetResetClause {
  }
;

/*****************************************************************************
 *
 *    DROP DATABASE [ IF EXISTS ]
 *
 * This is implicitly CASCADE, no need for drop behavior
 *****************************************************************************/

DropdbStmt:
  DROP DATABASE database_name {
  }
  | DROP DATABASE IF_P EXISTS database_name {
  }
;

/*****************************************************************************
 *
 *    ALTER SYSTEM
 *
 * This is used to change configuration parameters persistently.
 *****************************************************************************/

AlterSystemStmt:
  ALTER SYSTEM_P SET generic_set {
  }
  | ALTER SYSTEM_P RESET generic_reset{
  }
;

/*****************************************************************************
 *
 * Manipulate a domain
 *
 *****************************************************************************/

CreateDomainStmt:
  CREATE DOMAIN_P any_name opt_as Typename ColQualList {
  }
;

AlterDomainStmt:
  /* ALTER DOMAIN <domain> {SET DEFAULT <expr>|DROP DEFAULT} */
  ALTER DOMAIN_P any_name alter_column_default {
  }
  /* ALTER DOMAIN <domain> DROP NOT NULL */
  | ALTER DOMAIN_P any_name DROP NOT NULL_P {
  }
  /* ALTER DOMAIN <domain> SET NOT NULL */
  | ALTER DOMAIN_P any_name SET NOT NULL_P {
  }
  /* ALTER DOMAIN <domain> ADD CONSTRAINT ... */
  | ALTER DOMAIN_P any_name ADD_P TableConstraint {
  }
  /* ALTER DOMAIN <domain> DROP CONSTRAINT <name> [RESTRICT|CASCADE] */
  | ALTER DOMAIN_P any_name DROP CONSTRAINT name opt_drop_behavior {
  }
  /* ALTER DOMAIN <domain> DROP CONSTRAINT IF EXISTS <name> [RESTRICT|CASCADE] */
  | ALTER DOMAIN_P any_name DROP CONSTRAINT IF_P EXISTS name opt_drop_behavior {
  }
  /* ALTER DOMAIN <domain> VALIDATE CONSTRAINT <name> */
  | ALTER DOMAIN_P any_name VALIDATE CONSTRAINT name {
  }
;

opt_as:
  AS                        {}
  | /* EMPTY */             {}
;

/*****************************************************************************
 *
 * Manipulate a text search dictionary or configuration
 *
 *****************************************************************************/

AlterTSDictionaryStmt:
  ALTER TEXT_P SEARCH DICTIONARY any_name definition {
  }
;

AlterTSConfigurationStmt:
  ALTER TEXT_P SEARCH CONFIGURATION any_name ADD_P MAPPING FOR name_list any_with any_name_list {
  }
  | ALTER TEXT_P SEARCH CONFIGURATION any_name ALTER MAPPING FOR name_list any_with any_name_list {
  }
  | ALTER TEXT_P SEARCH CONFIGURATION any_name ALTER MAPPING REPLACE any_name any_with any_name {
  }
  | ALTER TEXT_P SEARCH CONFIGURATION any_name ALTER MAPPING FOR name_list
    REPLACE any_name any_with any_name {
  }
  | ALTER TEXT_P SEARCH CONFIGURATION any_name DROP MAPPING FOR name_list {
  }
  | ALTER TEXT_P SEARCH CONFIGURATION any_name DROP MAPPING IF_P EXISTS FOR name_list {
  }
;

/* Use this if TIME or ORDINALITY after WITH should be taken as an identifier */
any_with:
  WITH                    {}
  | WITH_LA               {}
;

/*****************************************************************************
 *
 * Manipulate a conversion
 *
 *    CREATE [DEFAULT] CONVERSION <conversion_name>
 *    FOR <encoding_name> TO <encoding_name> FROM <func_name>
 *
 *****************************************************************************/

CreateConversionStmt:
  CREATE opt_default CONVERSION_P any_name FOR Sconst TO Sconst FROM any_name {
  }
;

/*****************************************************************************
 *
 *    QUERY:
 *        CLUSTER [VERBOSE] <qualified_name> [ USING <index_name> ]
 *        CLUSTER [VERBOSE]
 *        CLUSTER [VERBOSE] <index_name> ON <qualified_name> (for pre-8.3)
 *
 *****************************************************************************/

ClusterStmt:
  CLUSTER opt_verbose qualified_name cluster_index_specification {
  }
  | CLUSTER opt_verbose {
  }
  /* kept for pre-8.3 compatibility */
  | CLUSTER opt_verbose index_name ON qualified_name {
  }
;

cluster_index_specification:
  USING index_name    { $$ = $2; }
  | /*EMPTY*/         { $$ = nullptr; }
;

/*****************************************************************************
 *
 *    QUERY:
 *        VACUUM
 *        ANALYZE
 *
 *****************************************************************************/

VacuumStmt:
  VACUUM opt_full opt_freeze opt_verbose {
  }
  | VACUUM opt_full opt_freeze opt_verbose qualified_name {
  }
  | VACUUM opt_full opt_freeze opt_verbose AnalyzeStmt {
  }
  | VACUUM '(' vacuum_option_list ')' {
  }
  | VACUUM '(' vacuum_option_list ')' qualified_name opt_name_list {
  }
;

vacuum_option_list:
  vacuum_option_elem {
  }
  | vacuum_option_list ',' vacuum_option_elem {
  }
;

vacuum_option_elem:
  analyze_keyword   { $$ = VACOPT_ANALYZE; }
  | VERBOSE     { $$ = VACOPT_VERBOSE; }
  | FREEZE      { $$ = VACOPT_FREEZE; }
  | FULL        { $$ = VACOPT_FULL; }
;

AnalyzeStmt:
  analyze_keyword opt_verbose {
  }
  | analyze_keyword opt_verbose qualified_name opt_name_list {
  }
;

analyze_keyword:
  ANALYZE                         {}
  | ANALYSE /* British */         {}
    ;

opt_verbose:
  VERBOSE                   { $$ = true; }
  | /*EMPTY*/               { $$ = false; }
;

opt_full:
  FULL                      { $$ = true; }
  | /*EMPTY*/               { $$ = false; }
    ;

opt_freeze:
  FREEZE                    { $$ = true; }
  | /*EMPTY*/               { $$ = false; }
;

opt_name_list:
  '(' name_list ')'         { $$ = $2; }
  | /*EMPTY*/ {
  }
;

/*****************************************************************************
 *
 *    QUERY:
 *        EXPLAIN query
 *
 *****************************************************************************/

ExplainStmt:
  EXPLAIN ExplainableStmt {
    $$ = MAKE_NODE(@1, PTExplainStmt, $2);
  }
  | EXPLAIN analyze_keyword ExplainableStmt {
    PARSER_UNSUPPORTED(@2);
  }
  | EXPLAIN analyze_keyword VERBOSE ExplainableStmt {
    PARSER_UNSUPPORTED(@2);
  }
  | EXPLAIN VERBOSE analyze_keyword ExplainableStmt {
    PARSER_UNSUPPORTED(@3);
  }
  | EXPLAIN VERBOSE ExplainableStmt {
    PARSER_UNSUPPORTED(@2);
  }
;

ExplainableStmt:
  SelectStmt { $$ = $1; }
  | InsertStmt { $$ = $1; }
  | UpdateStmt { $$ = $1; }
  | DeleteStmt { $$ = $1; }
;

/*****************************************************************************
 *
 *    QUERY:
 *        PREPARE <plan_name> [(args, ...)] AS <query>
 *
 *****************************************************************************/

PrepareStmt:
  PREPARE name prep_type_clause AS PreparableStmt {
  }
;

prep_type_clause:
  '(' type_list ')' {
  }
  | /* EMPTY */ {
  }
;

PreparableStmt:
  SelectStmt            { $$ = nullptr; }
  | InsertStmt          { $$ = nullptr; }
  | UpdateStmt          { $$ = nullptr; }
  | DeleteStmt          { $$ = nullptr; }
;

/*****************************************************************************
 *
 * EXECUTE <plan_name> [(params, ...)]
 * CREATE TABLE <name> AS EXECUTE <plan_name> [(params, ...)]
 *
 *****************************************************************************/

ExecuteStmt:
  EXECUTE name execute_param_clause {
  }
  | CREATE OptTemp TABLE create_as_target AS
    EXECUTE name execute_param_clause opt_with_data {
  }
;

execute_param_clause:
  '(' expr_list ')' {
  }
  | /* EMPTY */ {
  }
;

/*****************************************************************************
 *
 *    QUERY:
 *        DEALLOCATE [PREPARE] <plan_name>
 *
 *****************************************************************************/

DeallocateStmt:
  DEALLOCATE name {
  }
  | DEALLOCATE PREPARE name {
  }
  | DEALLOCATE ALL {
  }
  | DEALLOCATE PREPARE ALL {
  }
;

/*****************************************************************************
 *
 *    QUERY:
 *        LOCK TABLE
 *
 *****************************************************************************/

LockStmt:
  LOCK_P opt_table relation_expr_list opt_lock opt_nowait {
  }
;

opt_lock:
  IN_P lock_type MODE {
  }
  | /*EMPTY*/ {
  }
;

lock_type:
  ACCESS SHARE {
  }
  | ROW SHARE {
  }
  | ROW EXCLUSIVE {
  }
  | SHARE UPDATE EXCLUSIVE {
  }
  | SHARE {
  }
  | SHARE ROW EXCLUSIVE {
  }
  | EXCLUSIVE {
  }
  | ACCESS EXCLUSIVE {
  }
;

opt_nowait:
  NOWAIT                { $$ = true; }
  | /*EMPTY*/           { $$ = false; }
    ;

opt_nowait_or_skip:
  NOWAIT                { }
  | SKIP LOCKED         { }
  | /*EMPTY*/           { }
;

/*****************************************************************************
 *
 *    QUERY:
 *        CURSOR STATEMENTS
 *
 *****************************************************************************/
DeclareCursorStmt:
  DECLARE cursor_name cursor_options CURSOR opt_hold FOR SelectStmt {
  }
;

cursor_name:
  name {
    $$ = $1;
  }
;

cursor_options:
  /*EMPTY*/                     { $$ = 0; }
  | cursor_options NO SCROLL    { $$ = $1 | CURSOR_OPT_NO_SCROLL; }
  | cursor_options SCROLL       { $$ = $1 | CURSOR_OPT_SCROLL; }
  | cursor_options BINARY       { $$ = $1 | CURSOR_OPT_BINARY; }
  | cursor_options INSENSITIVE  { $$ = $1 | CURSOR_OPT_INSENSITIVE; }
;

opt_hold:
  /* EMPTY */                   { $$ = 0; }
  | WITH HOLD                   { $$ = CURSOR_OPT_HOLD; }
  | WITHOUT HOLD                { $$ = 0; }
;

%%

namespace yb {
namespace ql {

void GramProcessor::error(const location_type& l, const string& m) {
  // Bison parser will raise exception, so we don't return Status::Error here.
  Status s = parser_->parse_context()->Error(l, m.c_str());
  VLOG(3) << s.ToString();
}

}  // namespace ql
}  // namespace yb
