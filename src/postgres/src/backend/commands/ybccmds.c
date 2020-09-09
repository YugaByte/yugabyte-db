/*--------------------------------------------------------------------------------------------------
 *
 * ybccmds.c
 *        YB commands for creating and altering table structures and settings
 *
 * Copyright (c) YugaByte, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License.  You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
 * or implied.  See the License for the specific language governing permissions and limitations
 * under the License.
 *
 * IDENTIFICATION
 *        src/backend/commands/ybccmds.c
 *
 *------------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "access/sysattr.h"
#include "catalog/catalog.h"
#include "catalog/index.h"
#include "catalog/pg_am.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "catalog/pg_type_d.h"
#include "catalog/ybctype.h"
#include "commands/dbcommands.h"
#include "commands/ybccmds.h"
#include "commands/tablegroup.h"

#include "access/htup_details.h"
#include "utils/lsyscache.h"
#include "utils/relcache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "executor/tuptable.h"
#include "executor/ybcExpr.h"

#include "yb/yql/pggate/ybc_pggate.h"
#include "pg_yb_utils.h"

#include "access/nbtree.h"
#include "commands/defrem.h"
#include "nodes/nodeFuncs.h"
#include "parser/parser.h"
#include "parser/parse_coerce.h"
#include "parser/parse_type.h"

/* Utility function to calculate column sorting options */
static void
ColumnSortingOptions(SortByDir dir, SortByNulls nulls, bool* is_desc, bool* is_nulls_first)
{
  if (dir == SORTBY_DESC) {
	/*
	 * From postgres doc NULLS FIRST is the default for DESC order.
	 * So SORTBY_NULLS_DEFAULT is equal to SORTBY_NULLS_FIRST here.
	 */
	*is_desc = true;
	*is_nulls_first = (nulls != SORTBY_NULLS_LAST);
  } else {
	/*
	 * From postgres doc ASC is the default sort order and NULLS LAST is the default for it.
	 * So SORTBY_DEFAULT is equal to SORTBY_ASC and SORTBY_NULLS_DEFAULT is equal
	 * to SORTBY_NULLS_LAST here.
	 */
	*is_desc = false;
	*is_nulls_first = (nulls == SORTBY_NULLS_FIRST);
  }
}

/* -------------------------------------------------------------------------- */
/*  Database Functions. */

void
YBCCreateDatabase(Oid dboid, const char *dbname, Oid src_dboid, Oid next_oid, bool colocated)
{
	YBCPgStatement handle;

	HandleYBStatus(YBCPgNewCreateDatabase(dbname,
										  dboid,
										  src_dboid,
										  next_oid,
										  colocated,
										  &handle));
	HandleYBStatus(YBCPgExecCreateDatabase(handle));
}

void
YBCDropDatabase(Oid dboid, const char *dbname)
{
	YBCPgStatement handle;

	HandleYBStatus(YBCPgNewDropDatabase(dbname,
										dboid,
										&handle));
        bool not_found = false;
        HandleYBStatusIgnoreNotFound(YBCPgExecDropDatabase(handle), &not_found);
}

void
YBCReserveOids(Oid dboid, Oid next_oid, uint32 count, Oid *begin_oid, Oid *end_oid)
{
	HandleYBStatus(YBCPgReserveOids(dboid,
									next_oid,
									count,
									begin_oid,
									end_oid));
}

/* ------------------------------------------------------------------------- */
/*  Tablegroup Functions. */
void
YBCCreateTablegroup(Oid grpoid)
{
	YBCPgStatement handle;
	char *db_name = get_database_name(MyDatabaseId);

	HandleYBStatus(YBCPgNewCreateTablegroup(db_name, MyDatabaseId,
											grpoid, &handle));
	HandleYBStatus(YBCPgExecCreateTablegroup(handle));
}

void
YBCDropTablegroup(Oid grpoid)
{
	YBCPgStatement handle;

	HandleYBStatus(YBCPgNewDropTablegroup(MyDatabaseId, grpoid, &handle));
	HandleYBStatus(YBCPgExecDropTablegroup(handle));
}


/* ------------------------------------------------------------------------- */
/*  Table Functions. */

static void CreateTableAddColumn(YBCPgStatement handle,
								 Form_pg_attribute att,
								 bool is_hash,
								 bool is_primary,
								 bool is_desc,
								 bool is_nulls_first)
{
	const AttrNumber attnum = att->attnum;
	const YBCPgTypeEntity *col_type = YBCDataTypeFromOidMod(attnum,
															att->atttypid);
	HandleYBStatus(YBCPgCreateTableAddColumn(handle,
																					 NameStr(att->attname),
																					 attnum,
																					 col_type,
																					 is_hash,
																					 is_primary,
																					 is_desc,
																					 is_nulls_first));
}

/* Utility function to add columns to the YB create statement
 * Columns need to be sent in order first hash columns, then rest of primary
 * key columns, then regular columns.
 */
static void CreateTableAddColumns(YBCPgStatement handle,
								  TupleDesc desc,
								  Constraint *primary_key,
								  const bool colocated,
								  Oid tablegroupId)
{
	/* Add all key columns first with respect to compound key order */
	ListCell *cell;
	if (primary_key != NULL)
	{
		foreach(cell, primary_key->yb_index_params)
		{
			IndexElem *index_elem = (IndexElem *)lfirst(cell);
			bool column_found = false;
			for (int i = 0; i < desc->natts; ++i)
			{
				Form_pg_attribute att = TupleDescAttr(desc, i);
				if (strcmp(NameStr(att->attname), index_elem->name) == 0)
				{
					if (!YBCDataTypeIsValidForKey(att->atttypid))
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("PRIMARY KEY containing column of type"
										" '%s' not yet supported",
										YBPgTypeOidToStr(att->atttypid))));
					SortByDir order = index_elem->ordering;
					/* In YB mode, the first column defaults to HASH if it is
					 * not set and its table is not colocated */
					const bool is_first_key =
						cell == list_head(primary_key->yb_index_params);
					bool is_hash = (order == SORTBY_HASH ||
									(is_first_key &&
									 order == SORTBY_DEFAULT &&
									 !colocated && tablegroupId == InvalidOid));
					bool is_desc = false;
					bool is_nulls_first = false;
					ColumnSortingOptions(order,
										 index_elem->nulls_ordering,
										 &is_desc,
										 &is_nulls_first);
					CreateTableAddColumn(handle,
										 att,
										 is_hash,
										 true /* is_primary */,
										 is_desc,
										 is_nulls_first);
					column_found = true;
					break;
				}
			}
			if (!column_found)
				ereport(FATAL,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("Column '%s' not found in table",
								index_elem->name)));
		}
	}

	/* Add all non-key columns */
	for (int i = 0; i < desc->natts; ++i)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);
		bool is_key = false;
		if (primary_key)
			foreach(cell, primary_key->yb_index_params)
			{
				IndexElem *index_elem = (IndexElem *) lfirst(cell);
				if (strcmp(NameStr(att->attname), index_elem->name) == 0)
				{
					is_key = true;
					break;
				}
			}
		if (!is_key)
			CreateTableAddColumn(handle,
								 att,
								 false /* is_hash */,
								 false /* is_primary */,
								 false /* is_desc */,
								 false /* is_nulls_first */);
	}
}

static Datum*
CreateSplitPointDatums(ParseState *pstate,
                       List *split_point,
                       Oid *col_attrtypes,
                       int32 *col_attrtypmods)
{
	Datum *datums = palloc(sizeof(Datum) * list_length(split_point));
	/* Within a split point, go through the splits for each column */
	int split_num = 0;
	ListCell *cell;
	foreach(cell, split_point)
	{
		/* Get the column's split */
		PartitionRangeDatum *split = (PartitionRangeDatum*) lfirst(cell);

		/* If it contains a value, convert that value */
		if (split->kind == PARTITION_RANGE_DATUM_VALUE)
		{
			A_Const *aconst = (A_Const*) split->value;
			Node *value = (Node *) make_const(pstate, &aconst->val, aconst->location);
			value = coerce_to_target_type(pstate,
			                              value,
			                              exprType(value),
			                              col_attrtypes[split_num],
			                              col_attrtypmods[split_num],
			                              COERCION_ASSIGNMENT,
			                              COERCE_IMPLICIT_CAST,
			                              -1);
			if (value == NULL || ((Const*)value)->consttype == 0)
				ereport(ERROR, (errmsg("Type mismatch in split point")));

			split->value = value;
			datums[split_num] = ((Const*)value)->constvalue;
		}
		else
		{
			/*
			 * TODO (george): maybe we'll allow MINVALUE/MAXVALUE in the future,
			 * but for now it is illegal
			 */
			ereport(ERROR, (errmsg("Split points must specify finite values")));
		}
		++split_num;
	}
	return datums;
}

/* Utility function to handle split points */
static void CreateTableHandleSplitOptions(YBCPgStatement handle,
										  TupleDesc desc,
										  OptSplit *split_options,
										  Constraint *primary_key,
										  Oid namespaceId,
										  const bool colocated,
										  YBCPgOid tablegroup_id)
{
	/* Address both types of split options */
	switch (split_options->split_type)
	{
		case NUM_TABLETS: ;
			/* Make sure we have HASH columns */
			bool hashable = true;
			if (primary_key) {
				/* If a primary key exists, we utilize it to check its ordering */
				ListCell *head = list_head(primary_key->yb_index_params);
				IndexElem *index_elem = (IndexElem*) lfirst(head);

				if (!index_elem ||
				   !(index_elem->ordering == SORTBY_HASH ||
				   index_elem->ordering == SORTBY_DEFAULT))
					hashable = false;
			} else {
				/* In the abscence of a primary key, we use ybrowid as the PK to hash partition */
				bool is_pg_catalog_table_ = IsSystemNamespace(namespaceId) && IsToastNamespace(namespaceId);
				/*
				 * Checking if  table_oid is valid simple means if the table is
				 * part of a tablegroup.
				 */
				hashable = !is_pg_catalog_table_ && !colocated && tablegroup_id == kInvalidOid;
			}

			if (!hashable)
				ereport(ERROR, (errmsg("HASH columns must be present to "
							"split by number of tablets")));
			/* Tell pggate about it */
			HandleYBStatus(YBCPgCreateTableSetNumTablets(handle, split_options->num_tablets));
			break;
		case SPLIT_POINTS: ;
			/* Number of columns used in the primary key */
			int num_key_cols = list_length(primary_key->keys);

			/* Get the type information on each column of the primary key,
			 * and verify none are HASH columns */
			Oid *col_attrtypes = palloc(sizeof(Oid) * num_key_cols);
			int32 *col_attrtypmods = palloc(sizeof(int32) * num_key_cols);
			YBCPgTypeEntity **type_entities = palloc(sizeof(YBCPgTypeEntity *) * num_key_cols);

			bool *skips = palloc0(sizeof(bool) * desc->natts);
			int col_num = 0;
			ListCell *cell;
			foreach(cell, primary_key->yb_index_params)
			{
				/* Column constraint for the primary key */
				IndexElem *index_elem = (IndexElem *) lfirst(cell);

				/* Locate the table column that matches */
				for (int i = 0; i < desc->natts; i++)
				{
					if (skips[i]) continue;

					Form_pg_attribute att = TupleDescAttr(desc, i);
					char *attname = NameStr(att->attname);

					/* Found it */
					if (strcmp(attname, index_elem->name) == 0)
					{
						/* Prohibit the use of HASH columns */
						if (index_elem->ordering == SORTBY_HASH ||
							(col_num == 0 && index_elem->ordering == SORTBY_DEFAULT))
						{
							ereport(ERROR, (errmsg("HASH columns cannot be used for "
												   "split points")));
						}

						/* Record information on the attribute */
						col_attrtypes[col_num] = att->atttypid;
						col_attrtypmods[col_num] = att->atttypmod;
						type_entities[col_num] = (YBCPgTypeEntity*)YBCDataTypeFromOidMod(
								att->attnum, att->atttypid);

						/* Know to skip this in any future searches */
						skips[i] = true;
						break;
					}
				}

				/* Next primary key column */
				col_num++;
			}

			/* Parser state for type conversion and validation */
			ParseState *pstate = make_parsestate(NULL);

			/* Ensure that each split point matches the primary key columns
			 * in number and type, and are in order */
			ListCell *cell1;
			foreach(cell1, split_options->split_points)
			{
				List *split_point = (List *) lfirst(cell1);
				int split_columns = list_length(split_point);

				if (split_columns > num_key_cols)
				{
					ereport(ERROR, (errmsg("Split points cannot be more than the number of "
										   "primary key columns")));
				}

				Datum *datums = CreateSplitPointDatums(
					pstate, split_point, col_attrtypes, col_attrtypmods);

				HandleYBStatus(YBCPgCreateTableAddSplitRow(
					handle, split_columns, type_entities, (uint64_t *)datums));
			}

			break;
		default:
			ereport(ERROR, (errmsg("Illegal memory state for SPLIT options")));
			break;
	}
}

void
YBCCreateTable(CreateStmt *stmt, char relkind, TupleDesc desc,
							 Oid relationId, Oid namespaceId, Oid tablegroupId)
{
	if (relkind != RELKIND_RELATION && relkind != RELKIND_PARTITIONED_TABLE)
	{
		return;
	}

	if (stmt->relation->relpersistence == RELPERSISTENCE_TEMP)
	{
		return; /* Nothing to do. */
	}

	YBCPgStatement handle = NULL;
	ListCell       *listptr;

	char *db_name = get_database_name(MyDatabaseId);
	char *schema_name = stmt->relation->schemaname;
	if (schema_name == NULL)
	{
		schema_name = get_namespace_name(namespaceId);
	}
	if (!IsBootstrapProcessingMode())
		YBC_LOG_INFO("Creating Table %s.%s.%s",
					 db_name,
					 schema_name,
					 stmt->relation->relname);

	Constraint *primary_key = NULL;

	foreach(listptr, stmt->constraints)
	{
		Constraint *constraint = lfirst(listptr);

		if (constraint->contype == CONSTR_PRIMARY)
		{
			primary_key = constraint;
		}
	}

	/* By default, inherit the colocated option from the database */
	bool colocated = MyDatabaseColocated;

	/* Handle user-supplied colocated reloption */
	ListCell *opt_cell;
	foreach(opt_cell, stmt->options)
	{
		DefElem *def = (DefElem *) lfirst(opt_cell);

		if (strcmp(def->defname, "colocated") == 0)
		{
			if (stmt->tablegroup)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot use \'colocated=true/false\' with tablegroup")));

			bool colocated_relopt = defGetBoolean(def);
			if (MyDatabaseColocated)
				colocated = colocated_relopt;
			else if (colocated_relopt)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot set colocated true on a non-colocated"
								" database")));
			/* The following break is fine because there should only be one
			 * colocated reloption at this point due to checks in
			 * parseRelOptions */
			break;
		}
	}

	HandleYBStatus(YBCPgNewCreateTable(db_name,
									   schema_name,
									   stmt->relation->relname,
									   MyDatabaseId,
									   relationId,
									   false, /* is_shared_table */
									   false, /* if_not_exists */
									   primary_key == NULL /* add_primary_key */,
									   colocated,
									   tablegroupId,
									   &handle));

	CreateTableAddColumns(handle, desc, primary_key, colocated, tablegroupId);

	/* Handle SPLIT statement, if present */
	OptSplit *split_options = stmt->split_options;
	if (split_options)
		CreateTableHandleSplitOptions(handle, desc, split_options, primary_key, namespaceId, colocated, tablegroupId);

	/* Create the table. */
	HandleYBStatus(YBCPgExecCreateTable(handle));
}

void
YBCDropTable(Oid relationId)
{
	YBCPgStatement	handle = NULL;
	bool			colocated = false;

	/* Determine if table is colocated */
	if (MyDatabaseColocated)
	{
		bool not_found = false;
		HandleYBStatusIgnoreNotFound(YBCPgIsTableColocated(MyDatabaseId,
																											 relationId,
																											 &colocated),
																 &not_found);
	}

	/* Create table-level tombstone for colocated tables / tables in a tablegroup */
	Oid tablegroupId = InvalidOid;
	if (TablegroupCatalogExists)
		tablegroupId = get_tablegroup_oid_by_table_oid(relationId);
	if (colocated || tablegroupId != InvalidOid)
	{
		bool not_found = false;
		HandleYBStatusIgnoreNotFound(YBCPgNewTruncateColocated(MyDatabaseId,
																													 relationId,
																													 false,
																													 &handle),
																 &not_found);
		/* Since the creation of the handle could return a 'NotFound' error,
		 * execute the statement only if the handle is valid.
		 */
		const bool valid_handle = !not_found;
		if (valid_handle)
		{
			HandleYBStatusIgnoreNotFound(YBCPgDmlBindTable(handle), &not_found);
			int rows_affected_count = 0;
			HandleYBStatusIgnoreNotFound(YBCPgDmlExecWriteOp(handle, &rows_affected_count), &not_found);
		}
	}

	/* Drop the table */
	{
		bool not_found = false;
		HandleYBStatusIgnoreNotFound(YBCPgNewDropTable(MyDatabaseId,
																									 relationId,
																									 false, /* if_exists */
																									 &handle),
																 &not_found);
		const bool valid_handle = !not_found;
		if (valid_handle)
		{
			HandleYBStatusIgnoreNotFound(YBCPgExecDropTable(handle), &not_found);
		}
	}
}

void
YBCTruncateTable(Relation rel) {
	YBCPgStatement	handle;
	Oid				relationId = RelationGetRelid(rel);
	bool			colocated = false;

	/* Determine if table is colocated */
	if (MyDatabaseColocated)
		HandleYBStatus(YBCPgIsTableColocated(MyDatabaseId,
											 relationId,
											 &colocated));
	Oid tablegroupId = InvalidOid;
	if (TablegroupCatalogExists)
		tablegroupId = get_tablegroup_oid_by_table_oid(relationId);
	if (colocated || tablegroupId != InvalidOid)
	{
		/* Create table-level tombstone for colocated tables / tables in tablegroups */
		HandleYBStatus(YBCPgNewTruncateColocated(MyDatabaseId,
												 relationId,
												 false,
												 &handle));
		HandleYBStatus(YBCPgDmlBindTable(handle));
		int rows_affected_count = 0;
		HandleYBStatus(YBCPgDmlExecWriteOp(handle, &rows_affected_count));
	}
	else
	{
		/* Send truncate table RPC to master for non-colocated tables */
		HandleYBStatus(YBCPgNewTruncateTable(MyDatabaseId,
											 relationId,
											 &handle));
		HandleYBStatus(YBCPgExecTruncateTable(handle));
	}

	if (!rel->rd_rel->relhasindex)
		return;

	/* Truncate the associated secondary indexes */
	List	 *indexlist = RelationGetIndexList(rel);
	ListCell *lc;

	foreach(lc, indexlist)
	{
		Oid indexId = lfirst_oid(lc);

		if (indexId == rel->rd_pkindex)
			continue;

		/* Determine if table is colocated */
		if (MyDatabaseColocated)
			HandleYBStatus(YBCPgIsTableColocated(MyDatabaseId,
												 relationId,
												 &colocated));

		tablegroupId = InvalidOid;
		if (TablegroupCatalogExists)
			tablegroupId = get_tablegroup_oid_by_table_oid(indexId);
		if (colocated || tablegroupId != InvalidOid)
		{
			/* Create table-level tombstone for colocated tables / tables in tablegroups */
			HandleYBStatus(YBCPgNewTruncateColocated(MyDatabaseId,
													 relationId,
													 false,
													 &handle));
			HandleYBStatus(YBCPgDmlBindTable(handle));
			int rows_affected_count = 0;
			HandleYBStatus(YBCPgDmlExecWriteOp(handle, &rows_affected_count));
		}
		else
		{
			/* Send truncate table RPC to master for non-colocated tables */
			HandleYBStatus(YBCPgNewTruncateTable(MyDatabaseId,
												 indexId,
												 &handle));
			HandleYBStatus(YBCPgExecTruncateTable(handle));
		}
	}

	list_free(indexlist);
}

/* Utility function to handle split points */
static void
CreateIndexHandleSplitOptions(YBCPgStatement handle,
                              TupleDesc desc,
                              OptSplit *split_options,
                              int16 * coloptions)
{
	/* Address both types of split options */
	switch (split_options->split_type)
	{
	case NUM_TABLETS:
		/* Make sure we have HASH columns */
		if (!(coloptions[0] & INDOPTION_HASH))
			ereport(ERROR, (errmsg("HASH columns must be present to split by number of tablets")));

		HandleYBStatus(YBCPgCreateIndexSetNumTablets(handle, split_options->num_tablets));
		break;
	case SPLIT_POINTS:
		{
			/*
			 * Get the type information on each column of the index key,
			 * and verify none are HASH columns
			 */
			Oid *col_attrtypes = palloc(sizeof(Oid) * desc->natts);
			int32 *col_attrtypmods = palloc(sizeof(int32) * desc->natts);
			YBCPgTypeEntity **type_entities = palloc(sizeof(YBCPgTypeEntity *) * desc->natts);

			for (int col_num = 0; col_num < desc->natts; ++col_num)
			{
				Form_pg_attribute att = TupleDescAttr(desc, col_num);
				/* Prohibit the use of HASH columns */
				if (coloptions[col_num] & INDOPTION_HASH)
					ereport(ERROR, (errmsg("HASH columns cannot be used for split points")));

				/* Record information on the attribute */
				col_attrtypes[col_num] = att->atttypid;
				col_attrtypmods[col_num] = att->atttypmod;
				type_entities[col_num] = (YBCPgTypeEntity*)YBCDataTypeFromOidMod(
					att->attnum, att->atttypid);
			}

			/* Parser state for type conversion and validation */
			ParseState *pstate = make_parsestate(NULL);

			ListCell *cell1;
			foreach(cell1, split_options->split_points)
			{
				List *split_point = (List *)lfirst(cell1);
				int split_columns = list_length(split_point);

				if (split_columns > desc->natts)
					ereport(ERROR, (errmsg("Split points cannot be more than the number of "
					                       "index columns")));

				Datum *datums = CreateSplitPointDatums(
					pstate, split_point, col_attrtypes, col_attrtypmods);

				HandleYBStatus(YBCPgCreateIndexAddSplitRow(
					handle, split_columns, type_entities, (uint64_t *)datums));
			}
		}
		break;
	default:
		ereport(ERROR, (errmsg("Illegal memory state for SPLIT options")));
		break;
	}
}

void
YBCCreateIndex(const char *indexName,
			   IndexInfo *indexInfo,
			   TupleDesc indexTupleDesc,
			   int16 *coloptions,
			   Datum reloptions,
			   Oid indexId,
			   Relation rel,
			   OptSplit *split_options,
			   const bool skip_index_backfill,
			   Oid tablegroupId)
{
	char *db_name	  = get_database_name(MyDatabaseId);
	char *schema_name = get_namespace_name(RelationGetNamespace(rel));

	if (!IsBootstrapProcessingMode())
		YBC_LOG_INFO("Creating index %s.%s.%s",
					 db_name,
					 schema_name,
					 indexName);

	/* Check reloptions. */
	ListCell	*opt_cell;
	foreach(opt_cell, untransformRelOptions(reloptions))
	{
		DefElem *def = (DefElem *) lfirst(opt_cell);

		if (strcmp(def->defname, "colocated") == 0)
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot set option \"%s\" on index",
							def->defname)));
		}
	}

	YBCPgStatement handle = NULL;

	HandleYBStatus(YBCPgNewCreateIndex(db_name,
									   schema_name,
									   indexName,
									   MyDatabaseId,
									   indexId,
									   RelationGetRelid(rel),
									   rel->rd_rel->relisshared,
									   indexInfo->ii_Unique,
									   skip_index_backfill,
									   false, /* if_not_exists */
									   tablegroupId,
									   &handle));

	for (int i = 0; i < indexTupleDesc->natts; i++)
	{
		Form_pg_attribute     att         = TupleDescAttr(indexTupleDesc, i);
		char                  *attname    = NameStr(att->attname);
		AttrNumber            attnum      = att->attnum;
		const YBCPgTypeEntity *col_type   = YBCDataTypeFromOidMod(attnum, att->atttypid);
		const bool            is_key      = (i < indexInfo->ii_NumIndexKeyAttrs);

		if (is_key)
		{
			if (!YBCDataTypeIsValidForKey(att->atttypid))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("INDEX on column of type '%s' not yet supported",
								YBPgTypeOidToStr(att->atttypid))));
		}

		const int16 options        = coloptions[i];
		const bool  is_hash        = options & INDOPTION_HASH;
		const bool  is_desc        = options & INDOPTION_DESC;
		const bool  is_nulls_first = options & INDOPTION_NULLS_FIRST;

		HandleYBStatus(YBCPgCreateIndexAddColumn(handle,
																						 attname,
																						 attnum,
																						 col_type,
																						 is_hash,
																						 is_key,
																						 is_desc,
																						 is_nulls_first));
	}

	/* Handle SPLIT statement, if present */
	if (split_options)
		CreateIndexHandleSplitOptions(handle, indexTupleDesc, split_options, coloptions);

	/* Create the index. */
	HandleYBStatus(YBCPgExecCreateIndex(handle));
}

YBCPgStatement
YBCPrepareAlterTable(AlterTableStmt *stmt, Relation rel, Oid relationId)
{
	YBCPgStatement handle = NULL;
	HandleYBStatus(YBCPgNewAlterTable(MyDatabaseId,
									  relationId,
									  &handle));

	ListCell *lcmd;
	int col = 1;
	bool needsYBAlter = false;

	foreach(lcmd, stmt->cmds)
	{
		AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lcmd);
		switch (cmd->subtype)
		{
			case AT_AddColumn:
			{
				ColumnDef* colDef = (ColumnDef *) cmd->def;
				Oid			typeOid;
				int32		typmod;
				HeapTuple	typeTuple;
				int order;

				/* Skip yb alter for IF NOT EXISTS with existing column */
				if (cmd->missing_ok)
				{
					HeapTuple tuple = SearchSysCacheAttName(RelationGetRelid(rel), colDef->colname);
					if (HeapTupleIsValid(tuple)) {
						ReleaseSysCache(tuple);
						break;
					}
				}

				typeTuple = typenameType(NULL, colDef->typeName, &typmod);
				typeOid = HeapTupleGetOid(typeTuple);
				order = RelationGetNumberOfAttributes(rel) + col;
				const YBCPgTypeEntity *col_type = YBCDataTypeFromOidMod(order, typeOid);

				HandleYBStatus(YBCPgAlterTableAddColumn(handle, colDef->colname,
																										order, col_type,
																										colDef->is_not_null));
				++col;
				ReleaseSysCache(typeTuple);
				needsYBAlter = true;

				break;
			}
			case AT_DropColumn:
			{
				/* Skip yb alter for IF EXISTS with non-existent column */
				if (cmd->missing_ok)
				{
					HeapTuple tuple = SearchSysCacheAttName(RelationGetRelid(rel), cmd->name);
					if (!HeapTupleIsValid(tuple))
						break;
					ReleaseSysCache(tuple);
				}

				HandleYBStatus(YBCPgAlterTableDropColumn(handle, cmd->name));
				needsYBAlter = true;

				break;
			}

			case AT_AddIndex:
			case AT_AddIndexConstraint:
			{
				IndexStmt *index = (IndexStmt *) cmd->def;
				/* Only allow adding indexes when it is a unique non-primary-key constraint */
				if (!index->unique || index->primary || !index->isconstraint)
				{
					ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("This ALTER TABLE command is not yet supported.")));
				}

				break;
			}

			case AT_AlterColumnType:
			{
				/*
				 * Only supports variants that don't require on-disk changes.
				 * For now, that is just varchar and varbit.
				 */
				ColumnDef*			colDef = (ColumnDef *) cmd->def;
				HeapTuple			typeTuple;
				Form_pg_attribute	attTup;
				Oid					curTypId;
				Oid					newTypId;
				int32				curTypMod;
				int32				newTypMod;

				/* Get current typid and typmod of the column. */
				typeTuple = SearchSysCacheAttName(RelationGetRelid(rel), cmd->name);
				if (!HeapTupleIsValid(typeTuple))
				{
					ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN),
							errmsg("column \"%s\" of relation \"%s\" does not exist",
									cmd->name, RelationGetRelationName(rel))));
				}
				attTup = (Form_pg_attribute) GETSTRUCT(typeTuple);
				curTypId = attTup->atttypid;
				curTypMod = attTup->atttypmod;
				ReleaseSysCache(typeTuple);

				/* Get the new typid and typmod of the column. */
				typenameTypeIdAndMod(NULL, colDef->typeName, &newTypId, &newTypMod);

				/* Only varbit and varchar don't cause on-disk changes. */
				switch (newTypId)
				{
					case VARCHAROID:
					case VARBITOID:
					{
						/*
						* Check for type equality, and that the new size is greater than or equal
						* to the old size, unless the current size is infinite (-1).
						*/
						if (newTypId != curTypId ||
							(newTypMod < curTypMod && newTypMod != -1) ||
							(newTypMod > curTypMod && curTypMod == -1))
						{
							ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									errmsg("This ALTER TABLE command is not yet supported.")));
						}
						break;
					}

					default:
					{
						if (newTypId == curTypId && newTypMod == curTypMod)
						{
							/* Types are the same, no changes will occur. */
							break;
						}
						ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								errmsg("This ALTER TABLE command is not yet supported.")));
					}
				}
				break;
			}

			case AT_AddConstraint:
			case AT_DropConstraint:
			case AT_DropOids:
			case AT_EnableTrig:
			case AT_EnableAlwaysTrig:
			case AT_EnableReplicaTrig:
			case AT_EnableTrigAll:
			case AT_EnableTrigUser:
			case AT_DisableTrig:
			case AT_DisableTrigAll:
			case AT_DisableTrigUser:
			case AT_ChangeOwner:
			case AT_ColumnDefault:
			case AT_DropNotNull:
			case AT_SetNotNull:
			case AT_AddIdentity:
			case AT_SetIdentity:
			case AT_DropIdentity:
			case AT_EnableRowSecurity:
			case AT_DisableRowSecurity:
			case AT_ForceRowSecurity:
			case AT_NoForceRowSecurity:
			case AT_AttachPartition:
			case AT_DetachPartition:
				/* For these cases a YugaByte alter isn't required, so we do nothing. */
				break;

			default:
				ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("This ALTER TABLE command is not yet supported.")));
				break;
		}
	}

	if (!needsYBAlter)
	{
		return NULL;
	}

	return handle;
}

void
YBCExecAlterTable(YBCPgStatement handle, Oid relationId)
{
	if (handle)
	{
		if (IsYBRelationById(relationId)) {
			HandleYBStatus(YBCPgExecAlterTable(handle));
		}
	}
}

void
YBCRename(RenameStmt *stmt, Oid relationId)
{
	YBCPgStatement handle = NULL;
	char *db_name	  = get_database_name(MyDatabaseId);

	switch (stmt->renameType)
	{
		case OBJECT_TABLE:
			HandleYBStatus(YBCPgNewAlterTable(MyDatabaseId,
											  relationId,
											  &handle));
			HandleYBStatus(YBCPgAlterTableRenameTable(handle, db_name, stmt->newname));
			break;

		case OBJECT_COLUMN:
		case OBJECT_ATTRIBUTE:

			HandleYBStatus(YBCPgNewAlterTable(MyDatabaseId,
											  relationId,
											  &handle));

			HandleYBStatus(YBCPgAlterTableRenameColumn(handle, stmt->subname, stmt->newname));
			break;

		default:
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("Renaming this object is not yet supported.")));

	}

	YBCExecAlterTable(handle, relationId);
}

void
YBCDropIndex(Oid relationId)
{
	YBCPgStatement	handle;
	bool			colocated = false;

	/* Determine if table is colocated */
	if (MyDatabaseColocated)
	{
		bool not_found = false;
		HandleYBStatusIgnoreNotFound(YBCPgIsTableColocated(MyDatabaseId,
																											 relationId,
																											 &colocated),
																 &not_found);
	}

	/* Create table-level tombstone for colocated tables / tables in a tablegroup */
	Oid tablegroupId = InvalidOid;
	if (TablegroupCatalogExists)
		tablegroupId = get_tablegroup_oid_by_table_oid(relationId);
	if (colocated || tablegroupId != InvalidOid)
	{
		bool not_found = false;
		HandleYBStatusIgnoreNotFound(YBCPgNewTruncateColocated(MyDatabaseId,
																													 relationId,
																													 false,
																													 &handle),
																 &not_found);
		const bool valid_handle = !not_found;
		if (valid_handle) {
			HandleYBStatusIgnoreNotFound(YBCPgDmlBindTable(handle), &not_found);
			int rows_affected_count = 0;
			HandleYBStatusIgnoreNotFound(YBCPgDmlExecWriteOp(handle, &rows_affected_count), &not_found);
		}
	}

	/* Drop the index table */
	{
		bool not_found = false;
		HandleYBStatusIgnoreNotFound(YBCPgNewDropIndex(MyDatabaseId,
																									 relationId,
																									 false, /* if_exists */
																									 &handle),
																 &not_found);
		const bool valid_handle = !not_found;
		if (valid_handle) {
			HandleYBStatusIgnoreNotFound(YBCPgExecDropIndex(handle), &not_found);
		}
	}
}
