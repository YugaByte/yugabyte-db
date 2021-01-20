/*-------------------------------------------------------------------------
 *
 * ybc_catalog_version.h
 *	  utility functions related to the ysql catalog version table.
 *
 * Portions Copyright (c) YugaByte, Inc.
 *
 * src/include/catalog/ybc_catalog_version.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef YBC_CATALOG_VERSION_H
#define YBC_CATALOG_VERSION_H

#include "yb/yql/pggate/ybc_pggate.h"
#include "pg_yb_utils.h"

/*
 * Enum representing how the catalog version is stored on this cluster.
 * Needed for backwards-compatibility with old clusters.
 * Should only be set once per process (first time the catalog version is 
 * requested) and never modified afterwards.
 * TODO: Once cluster/initdb upgrade is supported (#2272) we should use it
 * to upgrade old cluster and remove the now-obsolete protobuf-based paths.
 */
typedef enum YBCatalogVersionType
{
	CATALOG_VERSION_UNSET,           /* Not yet set. */
	CATALOG_VERSION_PROTOBUF_ENTRY,  /* Old protobuf-based version. */
	CATALOG_VERSION_CATALOG_TABLE,   /* New table-based version. */
} YBCatalogVersionType;

extern YBCatalogVersionType yb_catalog_version_type;

extern void YBCSetCatalogVersionType();

/* The latest catalog version from the master leader. */
extern void YBCGetMasterCatalogVersion(uint64_t *version);

/* The catalog version caches by the local tserver. */
extern bool YBCGetLocalTserverCatalogVersion(uint64_t *version);

/* Send a request to increment the master catalog version. */
extern bool YBCIncrementMasterCatalogVersionTableEntry(bool is_breaking_change);

/* Annotate an DML request if it changes the catalog data (if needed). */
bool YBCMarkStatementIfCatalogVersionIncrement(YBCPgStatement ybc_stmt, Relation rel);

#endif							/* YBC_CATALOG_VERSION_H */
