/*--------------------------------------------------------------------------------------------------
 *
 * ybcModifyTable.h
 *	  prototypes for ybcModifyTable.c
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
 * src/include/executor/ybcModifyTable.h
 *
 *--------------------------------------------------------------------------------------------------
 */

#ifndef YBCMODIFYTABLE_H
#define YBCMODIFYTABLE_H

#include "nodes/execnodes.h"
#include "executor/tuptable.h"

//------------------------------------------------------------------------------
// YugaByte modify table API.

/*
 * Insert data into YugaByte table.
 * This function is equivalent to "heap_insert", but it sends data to DocDB (YugaByte storage).
 */
extern Oid YBCHeapInsert(TupleTableSlot *slot,
												 HeapTuple tuple,
												 EState *estate);

/*
 * Insert a tuple into a YugaByte table. Will execute within a distributed
 * transaction if the table is transactional (YSQL default).
 */
extern Oid YBCExecuteInsert(Relation rel,
                            TupleDesc tupleDesc,
                            HeapTuple tuple);

/*
 * Execute the insert outside of a transaction.
 * Assumes the caller checked that it is safe to do so.
 */
extern Oid YBCExecuteNonTxnInsert(Relation rel,
								  TupleDesc tupleDesc,
								  HeapTuple tuple);

/*
 * Insert a tuple into the an index's backing YugaByte index table.
 */
extern void YBCExecuteInsertIndex(Relation rel,
								  Datum *values,
								  bool *isnull,
								  Datum ybctid,
								  bool is_backfill,
								  uint64_t *write_time);

/*
 * Delete a tuple (identified by ybctid) from a YugaByte table.
 * If this is a single row op we will return false in the case that there was
 * no row to delete. This can occur because we do not first perform a scan if
 * it is a single row op.
 */
extern bool YBCExecuteDelete(Relation rel,
							 TupleTableSlot *slot,
							 EState *estate,
							 ModifyTableState *mtstate);
/*
 * Delete a tuple (identified by index columns and base table ybctid) from an
 * index's backing YugaByte index table.
 */
extern void YBCExecuteDeleteIndex(Relation index,
                                  Datum *values,
                                  bool *isnull,
                                  Datum ybctid);

/*
 * Update a row (identified by ybctid) in a YugaByte table.
 * If this is a single row op we will return false in the case that there was
 * no row to update. This can occur because we do not first perform a scan if
 * it is a single row op.
 */
extern bool YBCExecuteUpdate(Relation rel,
							 TupleTableSlot *slot,
							 HeapTuple tuple,
							 EState *estate,
							 ModifyTableState *mtstate,
							 Bitmapset *updatedCols);

/*
 * Replace a row in a YugaByte table by first deleting an existing row
 * (identified by ybctid) and then inserting a tuple to replace it.
 * This allows us to update a row primary key.
 *
 * This will change ybctid of a row within a tuple.
 */
extern Oid YBCExecuteUpdateReplace(Relation rel,
								   TupleTableSlot *slot,
								   HeapTuple tuple,
								   EState *estate,
								   ModifyTableState *mtstate);

//------------------------------------------------------------------------------
// System tables modify-table API.
// For system tables we identify rows to update/delete directly by primary key
// and execute them directly (rather than needing to read ybctid first).
// TODO This should be used for regular tables whenever possible.

extern void YBCDeleteSysCatalogTuple(Relation rel, HeapTuple tuple);

extern void YBCUpdateSysCatalogTuple(Relation rel,
									 HeapTuple oldtuple,
									 HeapTuple tuple);

//------------------------------------------------------------------------------
// Utility methods.

extern bool YBCIsSingleRowTxnCapableRel(ResultRelInfo *resultRelInfo);

extern Datum YBCGetYBTupleIdFromSlot(TupleTableSlot *slot);

extern Datum YBCGetYBTupleIdFromTuple(Relation rel,
									  HeapTuple tuple,
									  TupleDesc tupleDesc);

/*
 * Returns if a table has secondary indices.
 */
extern bool YBCRelInfoHasSecondaryIndices(ResultRelInfo *resultRelInfo);

#endif							/* YBCMODIFYTABLE_H */
