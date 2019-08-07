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
#include "catalog/pg_attribute.h"
#include "access/sysattr.h"
#include "catalog/pg_class.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "catalog/pg_database.h"
#include "commands/ybccmds.h"
#include "catalog/ybctype.h"

#include "catalog/catalog.h"
#include "catalog/index.h"
#include "access/htup_details.h"
#include "utils/lsyscache.h"
#include "utils/relcache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "executor/tuptable.h"
#include "executor/ybcExpr.h"

#include "yb/yql/pggate/ybc_pggate.h"
#include "pg_yb_utils.h"

#include "parser/parser.h"
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
YBCCreateDatabase(Oid dboid, const char *dbname, Oid src_dboid, Oid next_oid)
{
	YBCPgStatement handle;

	HandleYBStatus(YBCPgNewCreateDatabase(ybc_pg_session,
										  dbname,
										  dboid,
										  src_dboid,
										  next_oid,
										  &handle));
	HandleYBStmtStatus(YBCPgExecCreateDatabase(handle), handle);
	HandleYBStatus(YBCPgDeleteStatement(handle));
}

void
YBCDropDatabase(Oid dboid, const char *dbname)
{
	YBCPgStatement handle;

	HandleYBStatus(YBCPgNewDropDatabase(ybc_pg_session,
	                                    dbname,
																			dboid,
	                                    &handle));
	HandleYBStmtStatus(YBCPgExecDropDatabase(handle), handle);
	HandleYBStatus(YBCPgDeleteStatement(handle));
}

void
YBCReserveOids(Oid dboid, Oid next_oid, uint32 count, Oid *begin_oid, Oid *end_oid)
{
	HandleYBStatus(YBCPgReserveOids(ybc_pg_session,
	                                dboid,
	                                next_oid,
	                                count,
	                                begin_oid,
	                                end_oid));
}

/* ---------------------------------------------------------------------------------------------- */
/*  Table Functions. */

/* Utility function to add columns to the YB create statement */
static void CreateTableAddColumns(YBCPgStatement handle,
								  TupleDesc desc,
								  Constraint *primary_key,
								  bool include_hash,
								  bool include_primary)
{
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att            = TupleDescAttr(desc, i);
		char              *attname       = NameStr(att->attname);
		AttrNumber        attnum         = att->attnum;
		bool              is_hash        = false;
		bool              is_primary     = false;
		bool              is_desc        = false;
		bool              is_nulls_first = false;


		if (primary_key != NULL)
		{
			ListCell *cell;

			int key_col_idx = 0;
			foreach(cell, primary_key->yb_index_params)
			{
				IndexElem *index_elem = (IndexElem *)lfirst(cell);

				if (strcmp(attname, index_elem->name) == 0)
				{
					SortByDir order = index_elem->ordering;
					/* In YB mode first column defaults to HASH if not set */
					is_hash = (order == SORTBY_HASH) ||
					          (key_col_idx == 0 && order == SORTBY_DEFAULT);
					is_primary = true;
          ColumnSortingOptions(order, index_elem->nulls_ordering, &is_desc, &is_nulls_first);
					break;
				}
				key_col_idx++;
			}
		}

		if (include_hash == is_hash && include_primary == is_primary)
		{
			if (is_primary && !YBCDataTypeIsValidForKey(att->atttypid)) {
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("PRIMARY KEY containing column of type '%s' not yet supported",
								YBPgTypeOidToStr(att->atttypid))));
			}
			const YBCPgTypeEntity *col_type = YBCDataTypeFromOidMod(attnum, att->atttypid);
			HandleYBStmtStatus(YBCPgCreateTableAddColumn(handle,
			                                             attname,
			                                             attnum,
			                                             col_type,
			                                             is_hash,
			                                             is_primary,
			                                             is_desc,
			                                             is_nulls_first), handle);
		}
	}
}

void
YBCCreateTable(CreateStmt *stmt, char relkind, TupleDesc desc, Oid relationId, Oid namespaceId)
{
	if (relkind != RELKIND_RELATION)
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

	HandleYBStatus(YBCPgNewCreateTable(ybc_pg_session,
	                                   db_name,
	                                   schema_name,
	                                   stmt->relation->relname,
	                                   MyDatabaseId,
	                                   relationId,
	                                   false, /* is_shared_table */
	                                   false, /* if_not_exists */
	                                   primary_key == NULL /* add_primary_key */,
	                                   &handle));

	/*
	 * Process the table columns. They need to be sent in order, first hash
	 * columns, then rest of primary key columns, then regular columns. If
	 * no primary key is specified, an internal primary key is added above.
	 */
	if (primary_key != NULL)
	{
		CreateTableAddColumns(handle,
							  desc,
							  primary_key,
							  true /* is_hash */,
							  true /* is_primary */);

		CreateTableAddColumns(handle,
							  desc,
							  primary_key,
							  false /* is_hash */,
							  true /* is_primary */);
	}

    CreateTableAddColumns(handle,
                          desc,
                          primary_key,
                          false /* is_hash */,
                          false /* is_primary */);

	/* Create the table. */
	HandleYBStmtStatus(YBCPgExecCreateTable(handle), handle);

	HandleYBStatus(YBCPgDeleteStatement(handle));
}

void
YBCDropTable(Oid relationId)
{
	YBCPgStatement handle;

	HandleYBStatus(YBCPgNewDropTable(ybc_pg_session,
									 MyDatabaseId,
									 relationId,
	                                 false,    /* if_exists */
	                                 &handle));
	HandleYBStmtStatus(YBCPgExecDropTable(handle), handle);
	HandleYBStatus(YBCPgDeleteStatement(handle));
}

void
YBCTruncateTable(Relation rel) {
	YBCPgStatement handle;
	Oid relationId = RelationGetRelid(rel);

	/* Truncate the base table */
	HandleYBStatus(YBCPgNewTruncateTable(ybc_pg_session, MyDatabaseId, relationId, &handle));
	HandleYBStmtStatus(YBCPgExecTruncateTable(handle), handle);
	HandleYBStatus(YBCPgDeleteStatement(handle));

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

		HandleYBStatus(YBCPgNewTruncateTable(ybc_pg_session, MyDatabaseId, indexId, &handle));
		HandleYBStmtStatus(YBCPgExecTruncateTable(handle), handle);
		HandleYBStatus(YBCPgDeleteStatement(handle));
	}

	list_free(indexlist);
}

void
YBCCreateIndex(const char *indexName,
			   IndexInfo *indexInfo,			   
			   TupleDesc indexTupleDesc,
			   int16 *coloptions,
			   Oid indexId,
			   Relation rel)
{
	char *db_name	  = get_database_name(MyDatabaseId);
	char *schema_name = get_namespace_name(RelationGetNamespace(rel));

	if (!IsBootstrapProcessingMode())
		YBC_LOG_INFO("Creating index %s.%s.%s",
					 db_name,
					 schema_name,
					 indexName);

	YBCPgStatement handle = NULL;

	HandleYBStatus(YBCPgNewCreateIndex(ybc_pg_session,
									   db_name,
									   schema_name,
									   indexName,
									   MyDatabaseId,
									   indexId,
									   RelationGetRelid(rel),
									   rel->rd_rel->relisshared,
									   indexInfo->ii_Unique,
									   false, /* if_not_exists */
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

		HandleYBStmtStatus(YBCPgCreateIndexAddColumn(handle,
		                                             attname,
		                                             attnum,
		                                             col_type,
		                                             is_hash,
		                                             is_key,
		                                             is_desc,
		                                             is_nulls_first), handle);
	}

	/* Create the index. */
	HandleYBStmtStatus(YBCPgExecCreateIndex(handle), handle);

	HandleYBStatus(YBCPgDeleteStatement(handle));
}

YBCPgStatement
YBCPrepareAlterTable(AlterTableStmt *stmt, Relation rel, Oid relationId)
{
	YBCPgStatement handle = NULL;
	HandleYBStatus(YBCPgNewAlterTable(ybc_pg_session,
									  MyDatabaseId,
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

				typeTuple = typenameType(NULL, colDef->typeName, &typmod);
				typeOid = HeapTupleGetOid(typeTuple);
				order = RelationGetNumberOfAttributes(rel) + col;
				const YBCPgTypeEntity *col_type = YBCDataTypeFromOidMod(order, typeOid);

				HandleYBStmtStatus(YBCPgAlterTableAddColumn(handle, colDef->colname,
															order, col_type,
															colDef->is_not_null), handle);

				++col;
				ReleaseSysCache(typeTuple);
				needsYBAlter = true;

				break;
			}
			case AT_DropColumn:
			{

				HandleYBStmtStatus(YBCPgAlterTableDropColumn(handle, cmd->name), handle);
				needsYBAlter = true;

				break;

			}

			case AT_AddIndex:
			case AT_AddIndexConstraint: {
				IndexStmt *index = (IndexStmt *) cmd->def;
				// Only allow adding indexes when it is a unique non-primary-key constraint
				if (!index->unique || index->primary || !index->isconstraint) {
					ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("This ALTER TABLE command is not yet supported.")));
				}

				break;
			}

			case AT_AddConstraint:
			case AT_DropConstraint:
			case AT_EnableTrig:
			case AT_EnableAlwaysTrig:
			case AT_EnableReplicaTrig:
			case AT_EnableTrigAll:
			case AT_EnableTrigUser:
			case AT_DisableTrig:
			case AT_DisableTrigAll:
			case AT_DisableTrigUser:
			case AT_ChangeOwner:
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
		HandleYBStatus(YBCPgDeleteStatement(handle));
		return NULL;
	}

	return handle;
}

void
YBCExecAlterTable(YBCPgStatement handle)
{
	if (handle)
	{
		HandleYBStmtStatus(YBCPgExecAlterTable(handle), handle);
		HandleYBStatus(YBCPgDeleteStatement(handle));
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
			HandleYBStatus(YBCPgNewAlterTable(ybc_pg_session,
											  MyDatabaseId,
											  relationId,
											  &handle));
			HandleYBStmtStatus(YBCPgAlterTableRenameTable(handle, db_name, stmt->newname), handle);
			break;

		case OBJECT_COLUMN:
		case OBJECT_ATTRIBUTE:

			HandleYBStatus(YBCPgNewAlterTable(ybc_pg_session,
											  MyDatabaseId,
											  relationId,
											  &handle));

			HandleYBStmtStatus(YBCPgAlterTableRenameColumn(handle,
							   stmt->subname, stmt->newname), handle);
			break;

		default:
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("Renaming this object is not yet supported.")));

	}
}

void
YBCDropIndex(Oid relationId)
{
	YBCPgStatement handle;

	HandleYBStatus(YBCPgNewDropIndex(ybc_pg_session,
									 MyDatabaseId,
									 relationId,
									 false,	   /* if_exists */
									 &handle));
	HandleYBStmtStatus(YBCPgExecDropIndex(handle), handle);
	HandleYBStatus(YBCPgDeleteStatement(handle));
}
