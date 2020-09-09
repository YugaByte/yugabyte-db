/*-------------------------------------------------------------------------
 *
 * catalog.h
 *	  prototypes for functions in backend/catalog/catalog.c
 *
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/catalog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CATALOG_H
#define CATALOG_H

#include "catalog/pg_class.h"
#include "utils/relcache.h"

/*
 * This OID corresponds to the next unused OID in the block of OIDs that are used
 * by YB specific catalog additions, starting at 8000. When making changes to the catalog
 * by adding a new OID in 'pg_*.h', 'toasting.h', or 'indexing.h', make sure to increment
 * this value. Additionally, the script 'catalog/unused_oids' will help by outputting
 * the blocks of unused OIDs to validate that this value is up to date.
 */
#define YB_MIN_UNUSED_OID 8003

extern bool IsSystemRelation(Relation relation);
extern bool IsToastRelation(Relation relation);
extern bool IsCatalogRelation(Relation relation);

extern bool IsSystemClass(Oid relid, Form_pg_class reltuple);
extern bool IsToastClass(Form_pg_class reltuple);
extern bool IsCatalogClass(Oid relid, Form_pg_class reltuple);

extern bool IsSystemNamespace(Oid namespaceId);
extern bool IsToastNamespace(Oid namespaceId);

extern bool IsReservedName(const char *name);

extern bool IsSharedRelation(Oid relationId);

extern Oid	GetNewOid(Relation relation);
extern Oid GetNewOidWithIndex(Relation relation, Oid indexId,
				   AttrNumber oidcolumn);
extern Oid GetNewRelFileNode(Oid reltablespace, Relation pg_class,
				  char relpersistence);

#endif							/* CATALOG_H */
