/*-------------------------------------------------------------------------
 *
 * pg_tablegroup.h
 *	  definition of the "tablegroup" system catalog (pg_tablegroup)
 *
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_tablegroup.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TABLEGROUP_H
#define PG_TABLEGROUP_H

#include "catalog/genbki.h"
#include "catalog/pg_tablegroup_d.h"

/* ----------------
 *		pg_tablegroup definition.  cpp turns this into
 *		typedef struct FormData_pg_tablegroup
 * ----------------
 */
CATALOG(pg_tablegroup,8000,TableGroupRelationId) BKI_ROWTYPE_OID(8002,TablegroupRelation_Rowtype_Id) BKI_SCHEMA_MACRO
{
	NameData	grpname;		/* tablegroup name */
	Oid			grpowner;		/* owner of tablegroup */

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	aclitem		grpacl[1];		/* access permissions */
	text		grpoptions[1];	/* per-tablegroup options */
#endif
} FormData_pg_tablegroup;

/* ----------------
 *		Form_pg_tablegroup corresponds to a pointer to a tuple with
 *		the format of pg_tablegroup relation.
 * ----------------
 */
typedef FormData_pg_tablegroup *Form_pg_tablegroup;

#endif							/* PG_TABLEGROUP_H */
