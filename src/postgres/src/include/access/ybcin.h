/*--------------------------------------------------------------------------------------------------
 *
 * ybcin.h
 *	  prototypes for ybcin.c
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
 * src/include/access/ybcin.h
 *
 *--------------------------------------------------------------------------------------------------
 */

#ifndef YBCIN_H
#define YBCIN_H

#include "access/amapi.h"

/*
 * external entry points for YugaByte indexes in ybcin.c
 */
extern IndexBuildResult *ybcinbuild(Relation heap, Relation index, struct IndexInfo *indexInfo);
extern void ybcinbuildempty(Relation index);
extern bool ybcininsert(Relation rel, Datum *values, bool *isnull, Datum ybctid, Relation heapRel,
						IndexUniqueCheck checkUnique, struct IndexInfo *indexInfo);
extern void ybcindelete(Relation rel, Datum *values, bool *isnull, Datum ybctid, Relation heapRel,
						struct IndexInfo *indexInfo);
extern IndexBuildResult *ybcinbackfill(Relation heap,
									   Relation index,
									   struct IndexInfo *indexInfo,
									   uint64_t *read_time,
									   RowBounds *row_bounds);
extern IndexBulkDeleteResult *ybcinbulkdelete(IndexVacuumInfo *info,
											  IndexBulkDeleteResult *stats,
											  IndexBulkDeleteCallback callback,
											  void *callback_state);
extern IndexBulkDeleteResult *ybcinvacuumcleanup(IndexVacuumInfo *info,
												 IndexBulkDeleteResult *stats);

extern bool ybcincanreturn(Relation index, int attno);
extern void ybcincostestimate(struct PlannerInfo *root,
							  struct IndexPath *path,
							  double loop_count,
							  Cost *indexStartupCost,
							  Cost *indexTotalCost,
							  Selectivity *indexSelectivity,
							  double *indexCorrelation,
							  double *indexPages);
extern bytea *ybcinoptions(Datum reloptions, bool validate);
extern bool ybcinproperty(Oid index_oid, int attno,
						  IndexAMProperty prop, const char *propname,
						  bool *res, bool *isnull);
extern bool ybcinvalidate(Oid opclassoid);

extern IndexScanDesc ybcinbeginscan(Relation rel, int nkeys, int norderbys);
extern void ybcinrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
						ScanKey orderbys, int norderbys);
extern bool ybcingettuple(IndexScanDesc scan, ScanDirection dir);
extern void ybcinendscan(IndexScanDesc scan);

#endif							/* YBCINDEX_H */
