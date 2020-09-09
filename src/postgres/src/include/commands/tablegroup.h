// tablegroup.h
//	  Commands to manipulate table groups
// src/include/commands/tablegroup.h
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

#ifndef TABLEGROUP_H
#define TABLEGROUP_H

#include "catalog/objectaddress.h"
#include "lib/stringinfo.h"
#include "nodes/parsenodes.h"

extern Oid	CreateTableGroup(CreateTableGroupStmt *stmt);
extern void DropTableGroup(DropTableGroupStmt *stmt);

extern Oid	get_tablegroup_oid(const char *tablegroupname, bool missing_ok);
extern Oid  get_tablegroup_oid_by_table_oid(Oid table_oid);
extern char *get_tablegroup_name(Oid grp_oid);

extern void RemoveTableGroupById(Oid grp_oid);

extern ObjectAddress RenameTablegroup(const char *oldname, const char *newname);
extern ObjectAddress AlterTablegroupOwner(const char *grpname, Oid newOwnerId);

#endif							/* TABLEGROUP_H */
