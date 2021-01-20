/*-------------------------------------------------------------------------
 *
 * pg_yb_catalog_version.h
 *
 *	  definition of the "YSQL catalog version" system catalog (pg_yb_catalog_version)
 *
 * Portions Copyright (c) YugaByte, Inc.
 *
 * src/include/catalog/pg_yb_catalog_version.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_YB_CATALOG_VERSION_H
#define PG_YB_CATALOG_VERSION_H

#include "catalog/genbki.h"

#include "catalog/pg_yb_catalog_version_d.h"

/* ----------------
 *		pg_yb_catalog_version definition.  cpp turns this into
 *		typedef struct FormData_yb_pg_catalog_version
 *      The YBCatalogVersionRelationId value (8010) is also used in docdb
 *      (kPgYbCatalogVersionTableOid in entity_ids.cc) so this value here
 *      should match kPgYbCatalogVersionTableOid.
 * ----------------
 */
CATALOG(pg_yb_catalog_version,8010,YBCatalogVersionRelationId) BKI_SHARED_RELATION BKI_WITHOUT_OIDS BKI_ROWTYPE_OID(8011,YBCatalogVersionRelation_Rowtype_Id) BKI_SCHEMA_MACRO
{
	/* Oid of the database this applies to. */
	Oid        db_oid;

	/* Current version of the catalog. */
	int64      current_version;

	/* Last version (change) that invalidated ongoing transactions. */
	int64      last_breaking_version;

} FormData_yb_pg_catalog_version;

/* ----------------
 *		Form_yb_pg_catalog_version corresponds to a pointer to a tuple with
 *		the format of pg_yb_catalog_version relation.
 * ----------------
 */
typedef FormData_yb_pg_catalog_version *Form_yb_pg_catalog_version;

#endif							/* PG_YB_CATALOG_VERSION_H */
