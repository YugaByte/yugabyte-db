// tablegroup.c
//	  Commands to manipulate table groups.
//	  Tablegroups are used to create colocation groups for tables.
//
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

#include "postgres.h"

#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include "access/heapam.h"
#include "access/reloptions.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_tablegroup.h"
#include "commands/comment.h"
#include "commands/seclabel.h"
#include "commands/tablecmds.h"
#include "commands/tablegroup.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "common/file_perm.h"
#include "miscadmin.h"
#include "postmaster/bgwriter.h"
#include "storage/fd.h"
#include "storage/lmgr.h"
#include "storage/standby.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/tqual.h"
#include "utils/varlena.h"

/*  YB includes. */
#include "commands/ybccmds.h"
#include "yb/yql/pggate/ybc_pggate.h"
#include "pg_yb_utils.h"

/*
 * Create a table group.
 */
Oid
CreateTableGroup(CreateTableGroupStmt *stmt)
{
	Relation	rel;
	Datum		values[Natts_pg_tablegroup];
	bool		nulls[Natts_pg_tablegroup];
	HeapTuple	tuple;
	Oid			tablegroupoid;
	Oid			ownerId;
	Acl		   *grpacl = NULL;

	if (!TablegroupCatalogExists)
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("Tablegroup system catalog does not exist.")));
	}

	/* If not superuser check privileges */
	if (!superuser())
	{
		AclResult aclresult;
		// Check that user has create privs on the database to allow creation
		// of a new tablegroup.
		aclresult = pg_database_aclcheck(MyDatabaseId, GetUserId(), ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_DATABASE,
						   get_database_name(MyDatabaseId));
	}

	if (MyDatabaseColocated)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot use tablegroups in a colocated database")));

	/*
	 * Check that there is no other tablegroup by this name.
	 */
	if (OidIsValid(get_tablegroup_oid(stmt->tablegroupname, true)))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("tablegroup \"%s\" already exists",
					 		stmt->tablegroupname)));

	if (stmt->owner)
		ownerId = get_rolespec_oid(stmt->owner, false);
	else
		ownerId = GetUserId();

	/*
	 * Insert tuple into pg_tablegroup.
	 */
	rel = heap_open(TableGroupRelationId, RowExclusiveLock);

	MemSet(nulls, false, sizeof(nulls));

	values[Anum_pg_tablegroup_grpname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->tablegroupname));
	values[Anum_pg_tablegroup_grpowner - 1] = ObjectIdGetDatum(ownerId);

	/* Get default permissions and set up grpacl */
	grpacl = get_user_default_acl(OBJECT_TABLEGROUP, ownerId, InvalidOid);
	if (grpacl != NULL)
		values[Anum_pg_tablegroup_grpacl - 1] = PointerGetDatum(grpacl);
	else
		nulls[Anum_pg_tablegroup_grpacl - 1] = true;

	/* Generate new proposed grpoptions (text array) */
	/* For now no grpoptions. Will be part of Interleaved/Copartitioned */

	nulls[Anum_pg_tablegroup_grpoptions - 1] = true;

	tuple = heap_form_tuple(rel->rd_att, values, nulls);

	tablegroupoid = CatalogTupleInsert(rel, tuple);

	heap_freetuple(tuple);

	if (IsYugaByteEnabled())
	{
		YBCCreateTablegroup(tablegroupoid);
	}

	/* We keep the lock on pg_tablegroup until commit */
	heap_close(rel, NoLock);

	return tablegroupoid;
}

/*
 * Drop a tablegroup
 */
void
DropTableGroup(DropTableGroupStmt *stmt)
{
	char *tablegroupname = stmt->tablegroupname;
	HeapScanDesc	scandesc;
	SysScanDesc		class_scandesc;
	Relation		rel;
	Relation		class_rel;
	HeapTuple		tuple;
	ScanKeyData		entry[1];
	Oid				tablegroupoid;

	if (!TablegroupCatalogExists)
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("Tablegroup system catalog does not exist.")));
	}
	/*
	 * Find the target tuple
	 */
	rel = heap_open(TableGroupRelationId, RowExclusiveLock);

	// Scan pg_tablegroup to find a tuple with a matching name.
	ScanKeyInit(&entry[0],
				Anum_pg_tablegroup_grpname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(tablegroupname));
	scandesc = heap_beginscan_catalog(rel, 1, entry);
	tuple = heap_getnext(scandesc, ForwardScanDirection);

	if (!HeapTupleIsValid(tuple))
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablegroup \"%s\" does not exist",
				 tablegroupname)));
		return;
	}

	tablegroupoid = HeapTupleGetOid(tuple);

	/* If not superuser check ownership to allow drop. */
	if (!superuser())
	{
		if (!pg_tablegroup_ownercheck(tablegroupoid, GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER,
						   OBJECT_TABLEGROUP,
						   tablegroupname);
	}

	/*
	 * Search pg_class for a tuple with a matching tablegroup oid in reloptions.
	 * If found, disallow drop as the tablegroup is non-empty.
	 * Use pg_class oid index for the systable scan.
	 */
	class_rel = heap_open(RelationRelationId, AccessShareLock);
	class_scandesc = systable_beginscan(class_rel,
										ClassOidIndexId,
										true /* indexOk */,
										NULL, 0, NULL);

	/*
	 * This is a clunky search. The alternate option is to use RelationIdGetRelation
	 * to open the relcache entry for every relation and search the rd_options struct
	 * to find the value of the tablegroup option (if not NULL). This seemed potentially
	 * dangerous. The current method is inefficient but DROP TABLEGROUP is a fairly rare
	 * operation.
	 */
	HeapTuple pg_class_tuple;
	TupleDesc pg_class_desc = RelationGetDescr(class_rel);
	while (HeapTupleIsValid(pg_class_tuple = systable_getnext(class_scandesc)))
	{
		bool isnull;
		Datum datum = fastgetattr(pg_class_tuple,
								  Anum_pg_class_reloptions,
								  pg_class_desc,
								  &isnull);

		if (isnull)
			continue;

		List *reloptions = untransformRelOptions(datum);
		ListCell *cell;
		foreach(cell, reloptions)
		{
			DefElem	*defel = (DefElem *) lfirst(cell);
			if (strcmp(defel->defname, "tablegroup") == 0)
			{
				if (tablegroupoid == (Oid) pg_atoi(defGetString(defel), sizeof(Oid), 0))
				{
					// Close pg_class
					systable_endscan(class_scandesc);
					heap_close(class_rel, NoLock);
					// Close pg_tablegroup
					heap_endscan(scandesc);
					heap_close(rel, NoLock);
					ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
				 		 errmsg("tablegroup \"%s\" is not empty",
								tablegroupname)));
					return;
				}
			}
		}
	}
	// Close pg_class
	systable_endscan(class_scandesc);
	heap_close(class_rel, NoLock);

	/* DROP hook for the tablegroup being removed */
	InvokeObjectDropHook(TableGroupRelationId, tablegroupoid, 0);

	/*
	 * Remove the pg_tablegroup tuple.
	 */
	CatalogTupleDelete(rel, tuple);

	heap_endscan(scandesc);

	if (IsYugaByteEnabled())
	{
		YBCDropTablegroup(tablegroupoid);
	}

	/* We keep the lock on pg_tablegroup until commit */
	heap_close(rel, NoLock);
}

/*
 * get_tablegroup_oid - given a tablegroup name, look up the OID
 *
 * If missing_ok is false, throw an error if tablegroup name not found.
 * If true, just return InvalidOid.
 */
Oid
get_tablegroup_oid(const char *tablegroupname, bool missing_ok)
{
	Oid				result;
	Relation		rel;
	HeapScanDesc	scandesc;
	HeapTuple		tuple;
	ScanKeyData		entry[1];

	if (!TablegroupCatalogExists)
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("Tablegroup system catalog does not exist.")));
	}

	/*
	 * Search pg_tablegroup.  We use a heapscan here even though there is an
	 * index on name, on the theory that pg_tablegroup will usually have just
	 * a few entries and so an indexed lookup is a waste of effort.
	 */
	rel = heap_open(TableGroupRelationId, AccessShareLock);

	ScanKeyInit(&entry[0],
				Anum_pg_tablegroup_grpname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(tablegroupname));
	scandesc = heap_beginscan_catalog(rel, 1, entry);
	tuple = heap_getnext(scandesc, ForwardScanDirection);

	/* We assume that there can be at most one matching tuple */
	if (HeapTupleIsValid(tuple))
		result = HeapTupleGetOid(tuple);
	else
		result = InvalidOid;

	heap_endscan(scandesc);
	heap_close(rel, AccessShareLock);

	if (!OidIsValid(result) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablegroup \"%s\" does not exist",
				 		tablegroupname)));

	return result;
}

/*
 * get_tablegroup_oid_by_table_oid - given a table oid, look up its tablegroup oid (if any)
 */
Oid
get_tablegroup_oid_by_table_oid(Oid table_oid)
{
	Oid			result = InvalidOid;
	HeapTuple	tuple;

	// get_tablegroup_oid_by_table_oid will only be called if TablegroupCatalogExists so this point
	// error should not occur here. Added check just in case.
	if (!TablegroupCatalogExists)
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("Tablegroup system catalog does not exist.")));
	}

	/*
	 * Search pg_class using a cache lookup as pg_class can grow large.
	 */
	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(table_oid));

	/* We assume that there can be at most one matching tuple */
	if (HeapTupleIsValid(tuple))
	{
		bool isnull;
		Datum datum = SysCacheGetAttr(RELOID,
									  tuple,
									  Anum_pg_class_reloptions,
									  &isnull);

		if (!isnull) {
			List *reloptions = untransformRelOptions(datum);
			ListCell *cell;
			foreach(cell, reloptions)
			{
				DefElem	*defel = (DefElem *) lfirst(cell);
				// Every node is a string node when untransformed. Need to type cast.
				if (strcmp(defel->defname, "tablegroup") == 0)
				{
					result = (Oid) pg_atoi(defGetString(defel), sizeof(Oid), 0);
				}
			}
		}

	}

	ReleaseSysCache(tuple);

	return result;
}

/*
 * get_tablegroup_name - given a tablegroup OID, look up the name
 *
 * Returns a palloc'd string, or NULL if no such tablegroup.
 */
char *
get_tablegroup_name(Oid grp_oid)
{
	char		   *result;
	HeapTuple		tuple;

	if (!TablegroupCatalogExists)
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("Tablegroup system catalog does not exist.")));
	}

	/*
	 * Search pg_tablegroup using a cache lookup.
	 */
	tuple = SearchSysCache1(TABLEGROUPOID, ObjectIdGetDatum(grp_oid));

	/* We assume that there can be at most one matching tuple */
	if (HeapTupleIsValid(tuple))
		result = pstrdup(NameStr(((Form_pg_tablegroup) GETSTRUCT(tuple))->grpname));
	else
		result = NULL;

	ReleaseSysCache(tuple);

	return result;
}

/*
 * RemoveTableGroupById - remove a tablegroup by its OID.
 * If a tablegroup does not exist with the provided oid, then an error is raised.
 *
 * grp_oid - the oid of the tablegroup.
 */
void
RemoveTableGroupById(Oid grp_oid)
{
	Relation		pg_tblgrp_rel;
	SysScanDesc		class_scandesc;
	HeapScanDesc	scandesc;
	ScanKeyData		skey[1];
	HeapTuple		tuple;
	Relation		class_rel;

	if (!TablegroupCatalogExists)
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("Tablegroup system catalog does not exist.")));
	}

	pg_tblgrp_rel = heap_open(TableGroupRelationId, RowExclusiveLock);

	/*
	 * Find the tablegroup to delete.
	 */
	ScanKeyInit(&skey[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(grp_oid));
	scandesc = heap_beginscan_catalog(pg_tblgrp_rel, 1, skey);
	tuple = heap_getnext(scandesc, ForwardScanDirection);

	/* If the tablegroup exists, then remove it, otherwise raise an error. */
	if (!HeapTupleIsValid(tuple))
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablegroup with oid %u does not exist",
				 		grp_oid)));
	}

	class_rel = heap_open(RelationRelationId, RowExclusiveLock);
	class_scandesc = systable_beginscan(class_rel,
										ClassOidIndexId,
										true /* indexOk */,
										NULL, 0, NULL);

	// Scan pg_class for reloptions. See DropTablegroup for more description.
	HeapTuple pg_class_tuple;
	TupleDesc pg_class_desc = RelationGetDescr(class_rel);
	while (HeapTupleIsValid(pg_class_tuple = systable_getnext(class_scandesc)))
	{
		bool isnull;
		Datum datum = fastgetattr(pg_class_tuple,
								  Anum_pg_class_reloptions,
								  pg_class_desc,
								  &isnull);

		if (isnull)
			continue;

		List *reloptions = untransformRelOptions(datum);
		ListCell *cell;
		foreach(cell, reloptions)
		{
			DefElem	*defel = (DefElem *) lfirst(cell);
			if (strcmp(defel->defname, "tablegroup") == 0)
			{
				if (grp_oid == (Oid) pg_atoi(defGetString(defel), sizeof(Oid), 0))
				{
					// Close pg_class
					systable_endscan(class_scandesc);
					heap_close(class_rel, NoLock);
					// Close pg_tablegroup
					heap_endscan(scandesc);
					heap_close(pg_tblgrp_rel, NoLock);
					ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
				 		 errmsg("tablegroup with oid \"%u\" is not empty",
								grp_oid)));
					return;
				}
			}
		}
	}
	// Close pg_class
	systable_endscan(class_scandesc);
	heap_close(class_rel, NoLock);

	/* DROP hook for the tablegroup being removed */
	InvokeObjectDropHook(TableGroupRelationId, grp_oid, 0);

	/*
	 * Remove the pg_tablegroup tuple
	 */
	CatalogTupleDelete(pg_tblgrp_rel, tuple);

	heap_endscan(scandesc);

	/* We keep the lock on pg_tablegroup until commit */
	heap_close(pg_tblgrp_rel, NoLock);
}

/*
 * Rename tablegroup
 */
ObjectAddress
RenameTablegroup(const char *oldname, const char *newname)
{
	Oid				tablegroupoid;
	HeapTuple		newtup;
	Relation		rel;
	ObjectAddress	address;
	HeapTuple		tuple;
	HeapScanDesc	scandesc;
	ScanKeyData		entry[1];

	if (!TablegroupCatalogExists) {
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("Tablegroup system catalog does not exist.")));
	}

	/*
	 * Look up the target tablegroup's OID, and get exclusive lock on it. We
	 * need this for the same reasons as DROP TABLEGROUP.
	 */
	rel = heap_open(TableGroupRelationId, RowExclusiveLock);
	ScanKeyInit(&entry[0],
				Anum_pg_tablegroup_grpname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(oldname));
	scandesc = heap_beginscan_catalog(rel, 1, entry);
	tuple = heap_getnext(scandesc, ForwardScanDirection);
	heap_endscan(scandesc);

	if (!HeapTupleIsValid(tuple))
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablegroup \"%s\" does not exist",
				 oldname)));
	}

	tablegroupoid = HeapTupleGetOid(tuple);

	/* must be owner or superuser */
	if (!superuser() && !pg_tablegroup_ownercheck(tablegroupoid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLEGROUP, oldname);

	/*
	 * Make sure the new name doesn't exist.
	 */
	if (OidIsValid(get_tablegroup_oid(newname, true)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("tablegroup \"%s\" already exists", newname)));

	/* rename */
	newtup = SearchSysCacheCopy1(TABLEGROUPOID, ObjectIdGetDatum(tablegroupoid));
	if (!HeapTupleIsValid(newtup))
		elog(ERROR, "cache lookup failed for tablegroup %u", tablegroupoid);

	namestrcpy(&(((Form_pg_tablegroup) GETSTRUCT(newtup))->grpname), newname);
	CatalogTupleUpdate(rel, &newtup->t_self, newtup);

	InvokeObjectPostAlterHook(TableGroupRelationId, tablegroupoid, 0);

	ObjectAddressSet(address, TableGroupRelationId, tablegroupoid);

	heap_freetuple(newtup);

	/*
	 * Close pg_tablegroup, but keep lock till commit.
	 */
	heap_close(rel, NoLock);

	return address;
}

/*
 * ALTER TABLEGROUP name OWNER TO newowner
 */
ObjectAddress
AlterTablegroupOwner(const char *grpname, Oid newOwnerId)
{
	Oid					tablegroupoid;
	HeapTuple			tuple;
	Relation			rel;
	ScanKeyData			entry[1];
	HeapScanDesc		scandesc;
	Form_pg_tablegroup	datForm;
	ObjectAddress		address;

	if (!TablegroupCatalogExists) {
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("Tablegroup system catalog does not exist.")));
	}

	rel = heap_open(TableGroupRelationId, RowExclusiveLock);
	ScanKeyInit(&entry[0],
				Anum_pg_tablegroup_grpname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(grpname));
	scandesc = heap_beginscan_catalog(rel, 1, entry);
	tuple = heap_getnext(scandesc, ForwardScanDirection);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablegroup \"%s\" does not exist", grpname)));

	tablegroupoid = HeapTupleGetOid(tuple);
	datForm = (Form_pg_tablegroup) GETSTRUCT(tuple);

	/*
	 * If the new owner is the same as the existing owner, consider the
	 * command to have succeeded.  This is to be consistent with other
	 * objects.
	 */
	if (datForm->grpowner != newOwnerId)
	{
		Datum		repl_val[Natts_pg_tablegroup];
		bool		repl_null[Natts_pg_tablegroup];
		bool		repl_repl[Natts_pg_tablegroup];
		Acl		   *newAcl;
		Datum		aclDatum;
		bool		isNull;
		HeapTuple	newtuple;

		/* Otherwise, must be owner of the existing object or a superuser */
		if (!superuser() && !pg_tablegroup_ownercheck(HeapTupleGetOid(tuple), GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLEGROUP, grpname);

		/* Must be able to become new owner */
		check_is_member_of_role(GetUserId(), newOwnerId);

		memset(repl_null, false, sizeof(repl_null));
		memset(repl_repl, false, sizeof(repl_repl));

		repl_repl[Anum_pg_tablegroup_grpowner - 1] = true;
		repl_val[Anum_pg_tablegroup_grpowner - 1] = ObjectIdGetDatum(newOwnerId);

		/*
		 * Determine the modified ACL for the new owner.  This is only
		 * necessary when the ACL is non-null.
		 */
		aclDatum = heap_getattr(tuple,
								Anum_pg_tablegroup_grpacl,
								RelationGetDescr(rel),
								&isNull);
		if (!isNull)
		{
			newAcl = aclnewowner(DatumGetAclP(aclDatum),
								 datForm->grpowner, newOwnerId);
			repl_repl[Anum_pg_tablegroup_grpacl - 1] = true;
			repl_val[Anum_pg_tablegroup_grpacl - 1] = PointerGetDatum(newAcl);
		}

		newtuple = heap_modify_tuple(tuple, RelationGetDescr(rel), repl_val, repl_null, repl_repl);
		CatalogTupleUpdate(rel, &newtuple->t_self, newtuple);

		heap_freetuple(newtuple);

	}

	InvokeObjectPostAlterHook(TableGroupRelationId, tablegroupoid, 0);

	ObjectAddressSet(address, TableGroupRelationId, tablegroupoid);

	heap_endscan(scandesc);

	/* Close pg_tablegroup, but keep lock till commit */
	heap_close(rel, NoLock);

	return address;
}
