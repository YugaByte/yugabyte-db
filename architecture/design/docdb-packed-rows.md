# DocDB Packed Rows

Packed rows store all non-key columns of a row as a single RocksDB key-value pair instead of one
key-value pair per column. The key is the document key without any column id subkey, and the
value is a packed blob whose layout is described by a `SchemaPacking` built from the table
schema. This reduces space, write amplification, and read costs for wide rows.

Main source files:

- `src/yb/dockv/packed_row.h` / `packed_row.cc` - `RowPackerV1`, `RowPackerV2`, wire formats.
- `src/yb/dockv/schema_packing.h` / `schema_packing.cc` - `SchemaPacking`,
  `SchemaPackingStorage`, `SchemaPackingRegistry`, decoders.
- `src/yb/dockv/dockv.proto` - `SchemaPackingPB`, `ColumnPackingPB`.
- `src/yb/docdb/doc_read_context.h` / `.cc` - per-table schema + packing storage container.
- `src/yb/docdb/pgsql_operation.cc` - YSQL write path packing.
- `src/yb/docdb/cql_operation.cc` - YCQL write path packing.
- `src/yb/docdb/docdb_compaction_context.cc` - repacking during compaction.
- `src/yb/docdb/doc_reader.cc` - read path decoding, TTL/expiration handling.

## Flags

YSQL flags, defined in `src/yb/docdb/pgsql_operation.cc`:

- `ysql_enable_packed_row` - AutoFlag (class kExternal), runtime bool. Enabled by default in
  release builds, disabled in debug builds. Master switch for YSQL packed rows.
- `ysql_enable_packed_row_for_colocated_table` - runtime bool, default `true`. Allows packing
  for colocated tables.
- `ysql_packed_row_size_limit` - uint64, default `0`. Packed row size limit in bytes; 0 means
  use the SSTable block size (`db_block_size_bytes`, 32 KB).
- `ysql_enable_pack_full_row_update` - runtime bool, default `false`. Pack UPDATE statements
  that rewrite every non-key column.
- `ysql_mark_update_packed_row` - runtime bool, default `false`. Sets the V2 "is update" flag on
  packed rows produced by UPDATE so CDC can distinguish them from INSERT.
- `ysql_use_packed_row_v2` - AutoFlag (class kExternal), runtime bool, `false` -> `true`. Use
  packed row V2 encoding instead of V1.

YCQL flags, defined in `src/yb/docdb/cql_operation.cc`:

- `ycql_enable_packed_row` - runtime bool. Enabled by default in debug builds, disabled in
  release builds; note the convention is inverted vs YSQL. Master switch for YCQL packed rows.
- `ycql_packed_row_size_limit` - uint64, default `0`. Same semantics as the YSQL size limit.

Related flags:

- `db_block_size_bytes` - int64, 32 KB. Effective size limit when `*_packed_row_size_limit` is 0
  (`PackedSizeLimit`, `packed_row.cc:343`).
- `enable_schema_packing_gc` - bool, default `true`. Garbage collect unused schema packings
  after compaction (`tablet.cc:218`).
- `xcluster_enable_packed_rows_support` - runtime bool, default `true`. Rewrite packed rows with
  the xCluster consumer schema version (`xcluster_output_client.cc:60`).

The YSQL packing decision is `ShouldYsqlPackRow(is_colocated)` (`pgsql_operation.cc:167`):
`FLAGS_ysql_enable_packed_row && (!is_colocated ||
FLAGS_ysql_enable_packed_row_for_colocated_table)`.

## Insert path

### YSQL (`PgsqlWriteOperation`)

`RowPackContext` (`pgsql_operation.cc:1188`) wraps a `RowPackerVariant` (V1 or V2, chosen by
`ysql_use_packed_row_v2`) plus an `IntraTxnWriteId` reserved via
`DocWriteBatch::ReserveWriteId()`. The reserved write id makes the packed row sort before any
columns of the same operation written individually.

`ApplyInsert` (`pgsql_operation.cc:1517`) has two branches:

1. Packing enabled: a `RowPackContext` is created and columns are added in ascending column id
   order. `Complete()` encodes the packed value and calls `DocWriteBatch::SetPrimitive` with
   `DocPath(encoded_doc_key)` - no column id subkey.
2. Packing disabled: a liveness column (`kNullLow` under the system column id) is written,
   followed by one key-value pair per column.

Per-column fallback: `DoInsertColumn` (`pgsql_operation.cc:1497`) first tries
`pack_context->Add(column_id, value)`. If the packer rejects the value (varlen value does not
fit into the remaining size budget), the column is written as a separate key-value pair via
`InsertSubDocument` and the packed row keeps a NULL in that position; the separate cell shadows
the packed NULL on read.

UPDATE packs only when all of the following hold (`pgsql_operation.cc:1737`):
`ysql_enable_pack_full_row_update` is on, `ShouldYsqlPackRow` is true, and the update assigns
every non-key column. Partial updates always write individual columns.

### YCQL (`QLWriteOperation`)

Packing is attempted only for INSERT statements (`cql_operation.cc:1102`). It is then disabled
when:

- the write carries a user timestamp (`USING TIMESTAMP`), or
- `SchemaPacking::CouldPack` fails - the insert must supply exactly the set of packable columns
  (`schema_packing.cc:620`).

YCQL always uses `RowPackerV1`. Static columns and non-primitive values (collections,
subscripted writes) are never packed and go through `InsertSubDocument`
(`cql_operation.cc:865`). When packing is off, a liveness column is written instead.

## Compaction

`DocDBCompactionFeed` (`docdb_compaction_context.cc:661`) owns a `PackedRowData` object that
merges packed rows and the individual column updates on top of them into a single packed row
encoded with the latest schema packing.

Repacking triggers:

- A packed row with `schema_version < new_packing_.schema_version` starts repacking
  (`ProcessPackedRow`, `docdb_compaction_context.cc:182`).
- Individual column updates encountered before/over a packed row are absorbed into the packer
  (`ProcessColumn`, `docdb_compaction_context.cc:232`). A brand-new packed row can also be
  started from a liveness column when no packed row exists yet.

Target schema selection: on each coprefix (cotable/colocation id) change, the feed asks the
`SchemaPackingProvider` for `kLatestSchemaVersion` at the key's history cutoff. The returned
version is clamped by `SchemaPackingRegistry::MinActiveVersion` so compaction never packs with a
version concurrent readers cannot handle (`tablet_metadata.cc:441`).

Repack window: repacking is allowed only for entries whose hybrid time lies below the history
cutoff and inside `(repack_range_min, repack_range_max)` - a range shrunk to avoid overlap with
files and memtables not participating in this compaction and with the transaction participant's
`MinRunningHybridTime` (`HandleOtherRange`, `docdb_compaction_context.cc:1585`;
`tablet.cc:5410`). This guarantees the compaction sees the row's full history before rewriting
it.

Deleted columns: `CompactionSchemaInfo::deleted_cols` contains columns whose drop time is below
the history cutoff (`tablet_metadata.cc:470`). Standalone cells for such columns are discarded,
and repacking simply omits columns absent from the target packing.

TTL: expiration is evaluated before packed row processing (`Feed`,
`docdb_compaction_context.cc:1304`). An expired packed row is dropped on major compaction, or
rewritten as a tombstone on minor compaction. YCQL preserves original column write times during
repack (`keep_write_time()`, `docdb_compaction_context.cc:1491`) by injecting them as user
timestamps into per-column control fields.

Schema packing GC: while compacting, `PackedRowData` records the min/max schema versions
actually emitted per cotable into the output file's `ConsensusFrontier`. After a compaction or
flush, `Tablet::OldSchemaGC` (`tablet.cc:607`) computes the global minimum referenced version
across all live SST files and the memtable (further lowered by versions xCluster still needs),
and drops all older packings from `SchemaPackingStorage`. So a full compaction is what allows
old schema packings to be removed from tablet metadata.

## Schema packing management

A `SchemaPacking` (`schema_packing.h:77`) describes the packed value layout for one schema
version; the layout itself is covered in the Encoding versions section. A packed row references
its schema version, so every version that may still have packed rows on disk must keep its
packing available. This section covers how that list of known packings is maintained: where it
is stored, how it grows on schema changes, and how it shrinks via garbage collection.

### Persistence

Packings are serialized as `SchemaPackingPB` (`dockv.proto:34`): `schema_version`, repeated
`ColumnPackingPB` (`dockv.proto:21`), and `skipped_column_ids` for columns present in the schema
but excluded from packing.

They are persisted in the tablet superblock: `TableInfoPB.old_schema_packings`
(`tablet/metadata.proto:83`), next to the table's current `SchemaPB` and `schema_version`. The
current version's packing is deliberately not persisted: `DocReadContext::ToPB`
(`doc_read_context.h:64`) calls `SchemaPackingStorage::ToPB(current_version, ...)`, which skips
that version, and on load `LoadFromPB(old_schema_packings)` is followed by
`AddSchema(schema_version, schema)` (`doc_read_context.h:47`), rebuilding the current packing
from the schema itself. This keeps the two representations from diverging.

### SchemaPackingStorage lifecycle

`SchemaPackingStorage` (`schema_packing.h:186`) maps `SchemaVersion -> SchemaPacking` for one
table and lives inside the table's `DocReadContext`. Key operations
(`schema_packing.cc:863-1039`):

- `GetPacking(SchemaVersion)` - lookup; returns NotFound listing available versions if the
  packing was GCed or never existed.
- `GetPacking(Slice* packed_row)` - decodes the varint schema version from a packed value
  (after the value type byte) and looks it up. This is how readers and compaction resolve the
  packing for a row they encounter.
- `AddSchema(version, schema)` - registers the packing for a new schema version. On ALTER the
  tablet builds a new `DocReadContext` by copying the previous one and calling `AddSchema` with
  the bumped version (`doc_read_context.cc:50`), so packings of all still-visible old versions
  are retained.
- `GetSchemaPackingVersion(table_type, schema)` - reverse lookup: returns the highest version
  whose packing matches the given schema. Used to map schemas coming from outside (e.g. CDC and
  xCluster producers) onto local versions.
- `MergeWithRestored(version, schema, schemas, overwrite)` (`schema_packing.cc:926`) - used when
  restoring a snapshot: merges the snapshot's packings into the current ones (duplicates are
  tolerated), optionally overwriting local entries (`OverwriteSchemaPacking::kTrue`, used on the
  xCluster target where the same version number may describe a different schema), and
  guarantees the restored current version has a packing by deriving it from the restored
  `SchemaPB` if absent.

### Schema version retention and garbage collection

A packing may be dropped only when nothing can reference its schema version anymore. After each
flush or compaction, `Tablet::OldSchemaGC` (`tablet.cc:607`) computes the minimum still-used
version per table over all of the sources below, and `RaftGroupMetadata::OldSchemaGC`
(`tablet_metadata.cc:2017`) rebuilds the storage without older packings via the
`SchemaPackingStorage(rhs, min_schema_version)` copy constructor, guarded by `HasVersionBelow`.

The sources of the minimum used schema version:

- Every write stamps the schema versions it was prepared against into the write batch
  (`AddTableSchemaVersion` in `WriteQuery::CqlPrepareExecute` / `PgsqlPrepareExecute`,
  `write_query.cc:587,606,639`). On apply these land in the `ConsensusFrontier` of the write
  (`tablet.cc:1915`), so the frontier data below reflects what was actually written.
- Each RocksDB instance (the regular DB and the intents DB) is a source of used schema
  versions: `FillMinSchemaVersion` (`tablet.cc:672`) folds per-table versions from the smallest
  frontier of every live SST file and from the memtable frontier. Compaction refreshes the SST
  bounds with the versions it actually emitted (`PackedRowData::UpdateMeta`), which is how the
  minimum rises over time. Schema versions used by running transactions are covered by the
  intents DB, where their provisional records live until applied and removed.
- In-flight operations via `SchemaPackingRegistry` (`schema_packing.h:151`), shared per tablet:
  every live `SchemaPackingStorage` instance keeps its max schema version registered in the
  multiset (`UpdateMaxSchemaVersion`, `schema_packing.cc:852`) and removes it on destruction.
  Since each schema change creates a new `DocReadContext` generation, and reads, compactions,
  and running writes hold a reference to the generation they started with, an old generation
  stays alive - and its version registered - for as long as any such operation runs.
  `MinActiveVersion` over the multiset is the floor respected both by GC (do not drop packings
  an operation still uses, `tablet_metadata.cc:2035`) and by compaction (do not repack to a
  version newer than every active reader can decode, `tablet_metadata.cc:441`).
- xCluster: `FillMinXClusterSchemaVersion` (`tablet.cc:637`) lowers the minimum to the oldest
  version the xCluster consumer may still receive from the producer for this table.

In addition, `DisableSchemaGC` (`tablet_metadata.h:913`) suspends GC entirely for the duration
of operations that temporarily break the invariants above, e.g. restoring a tablet snapshot
(`tablet_snapshots.cc:269`).

## Encoding versions

Value entry types: `kPackedRowV1` ('z') and `kPackedRowV2` ('|') (`value_type.h`), enum
`PackedRowVersion { kV1, kV2 }` (`value_type.h:346`). Both formats start with the value type
byte followed by a varint schema version, and rely on the row's `SchemaPacking` to describe the
columns; only the column data layout differs.

### Column layout metadata

Per column, `ColumnPackingData` (`schema_packing.h:43`) stores: the column `id`,
`num_varlen_columns_before`, `offset_after_prev_varlen_column`, fixed `size` (0 for varlen),
`nullable`, `data_type` (persisted only in recent packings, required for V2 -
`SchemaPacking::HasDataType()`), and `missing_value` (see below).

Columns are split into fixed-length and variable-length. A column is varlen when it is nullable,
has a variable-length type, or - for YCQL - always, because CQL columns may carry an individual
TTL prefix (`IsVarlenColumn`, `schema_packing.cc:43`).

In practice the fixed-length representation is almost never used. YSQL creates every non-key
column as nullable in the DocDB schema regardless of SQL `NOT NULL` constraints, which are
enforced at the postgres layer only (`PgCreateTable::AddColumn` never calls `NotNull()`,
`pg_create_table.cc:258`; nullable is the `YBColumnSpec` default, `client/schema.h:160`). And
YCQL columns are varlen unconditionally. So effectively all packed columns are varlen.

### V1 (`packed_row.h:35`, `RowPackerV1`)

```
kPackedRowV1
varint:  schema_version
uint32:  end offset of data for the 1st varlen column
...
uint32:  end offset of data for the last varlen column
bytes:   data for the 1st column
...
bytes:   data for the last column
```

The header is an offset array of `varlen_columns_count * 4` bytes (`prefix_len()`), one `uint32`
end offset per varlen column. This gives O(1) random access to any column: its start is the end
of the previous varlen column plus `offset_after_prev_varlen_column`, and its end is either
`start + size` (fixed) or its own stored offset (varlen).

Each column value keeps the standalone DocDB value encoding, including the value type byte,
optionally preceded by encoded `ValueControlFields` (YCQL per-column TTL). NULL is a zero-length
column; the always-present value type byte makes it distinguishable from an empty value.

Since effectively all columns are varlen (see above), a V1 packed row pays 4 bytes of offset per
column plus the value type byte per value, and every NULL still occupies an offset slot. This
overhead is one of the main motivations for V2.

### V2 (`packed_row.cc:494`, `RowPackerV2`)

```
kPackedRowV2
varint:  schema_version
uint8:   flags (kHasNullsFlag = 1, kIsUpdateFlag = 2)
bytes:   null mask, one bit per column (present only when kHasNullsFlag is set)
bytes:   data for each non-null column
```

The V2 format was inspired by the way PostgreSQL stores rows on disk: like a heap tuple, a
packed row V2 has a null bitmap in the header and stores attribute data back to back, with the
layout driven by the column data types (fixed-size values raw, variable-size values behind a
length header).

V2 replaces both the offset array and the per-column value type byte:

- NULLs are tracked by a null mask of `NullMaskSize() = ceil(columns / 8)` bytes, one bit per
  column, so a NULL costs one bit instead of an offset slot. If no column is NULL the mask is
  elided entirely and `kHasNullsFlag` is cleared.
- Values are encoded according to the `data_type` from the packing, with no value type byte.
  Fixed-size data types are stored as raw little-endian bytes without any prefix, even when the
  column is classified varlen due to nullability. Variable-size data types (strings, binary)
  get a field length prefix: a single byte `len << 1` for lengths below 128, otherwise 4-byte
  little-endian `(len << 1) | 1` (`EncodeFieldLength`, `fast_varint.cc:358`). This reliance on
  data types is why V2 requires packings with persisted data types, and why compaction falls
  back to V1 when repacking rows whose packing predates them
  (`docdb_compaction_context.cc:337`).
- Without an offset array, locating a column means skipping the preceding columns (fixed sizes
  from the packing, varlen via length prefixes, NULLs via the mask), so V2 decoding is geared
  towards sequential full-row decode rather than O(1) random access.
- `kIsUpdateFlag` is set for packed rows produced by UPDATE when `ysql_mark_update_packed_row`
  is on, which lets CDC tell INSERT and UPDATE packed rows apart (`PackedRowV2Header`,
  `packed_row.h:234`).

V2 is YSQL-only: YCQL always uses V1 because V2 has no place for per-column control fields
(per-column TTL and write time).

### Example

For a YSQL table with schema version 1:

```sql
CREATE TABLE t (k INT PRIMARY KEY, v1 INT, v2 TEXT, v3 BIGINT);
INSERT INTO t VALUES (1, 42, 'hello', 1000000);
```

The value columns v1, v2, v3 are all nullable in the DocDB schema, hence all varlen in the
packing, in column id order. The RocksDB key is the document key for k = 1 with no column id
subkey; the packed RocksDB values are shown below byte by byte (produced by `RowPackerV1` /
`RowPackerV2` directly).

V1:

```
7A                           kPackedRowV1 ('z')
01                           schema version 1 (varint)
05 00 00 00                  end offset of v1 data: 5 (uint32, little-endian)
0B 00 00 00                  end offset of v2 data: 11
14 00 00 00                  end offset of v3 data: 20
48 00 00 00 2A               v1: kInt32 ('H'), int32 42 (big-endian)
53 68 65 6C 6C 6F            v2: kString ('S'), "hello"
49 00 00 00 00 00 0F 42 40   v3: kInt64 ('I'), int64 1000000 (big-endian)
```

Offsets are relative to the end of the offset array. Each value keeps the standalone DocDB
encoding: a value type byte followed by big-endian data.

V2:

```
7C                           kPackedRowV2 ('|')
01                           schema version 1 (varint)
00                           flags: no nulls, null mask elided
2A 00 00 00                  v1: int32 42 (little-endian, fixed size - no prefix)
0A                           v2 field length: 5 (encoded as len << 1)
68 65 6C 6C 6F               v2: "hello"
40 42 0F 00 00 00 00 00      v3: int64 1000000 (little-endian)
```

The same row with `INSERT INTO t VALUES (1, 42, NULL, NULL)`:

V1 - each NULL still costs a 4-byte offset slot (zero-length data):

```
7A 01                        kPackedRowV1, schema version 1
05 00 00 00                  end offset of v1 data: 5
05 00 00 00                  end offset of v2 data: 5 (zero length = NULL)
05 00 00 00                  end offset of v3 data: 5 (zero length = NULL)
48 00 00 00 2A               v1: kInt32, int32 42
```

V2 - the NULLs cost one bit each in the null mask:

```
7C 01                        kPackedRowV2, schema version 1
01                           flags: kHasNullsFlag
06                           null mask 00000110: v2 and v3 are NULL
2A 00 00 00                  v1: int32 42 (little-endian)
```

### Decoding

`PackedRowDecoderV1` / `PackedRowDecoderV2` (`schema_packing.h:282`, `schema_packing.h:308`)
provide `FetchValue` by packed index or column id on top of a `SchemaPacking`. For bulk row
decoding the read path builds a chain of per-column decoder functions
(`PackedColumnDecoderEntry`, produced by a `PackedRowDecoderFactory`, `schema_packing.h:349`);
YSQL rows are decoded this way straight into `PgTableRow` (`src/yb/dockv/pg_row.cc`).

For xCluster, `ReplaceSchemaVersionInPackedValue` (`packed_row.h:73`) rewrites the schema
version inside a packed value through a `SchemaVersionMapper`, so producer-packed rows can be
applied on a consumer whose version numbers differ.

### Missing columns

When a packed row is decoded with a packing that contains columns added after the row was
written, the decoder substitutes the column's `missing_value` (the evaluated default of
`ADD COLUMN ... DEFAULT`) or NULL if there is none. Symmetrically, `CompleteColumns`
(`packed_row.cc:319`) back-fills trailing columns the writer did not supply. This is what makes
`ALTER TABLE ADD COLUMN` instant: existing packed rows are never rewritten.

## YCQL specifics: TTL and value timestamp

Control fields (`ValueControlFields`, `value.h:27`) are an optional prefix encoded before a
DocDB value: merge flags, TTL (signed varint, milliseconds), and user timestamp (big-endian
uint64 microseconds). They interact with packed rows as follows.

TTL (`USING TTL`): does not prevent packing. The row-level control fields (including TTL) are
encoded by `RowPackerBase` as a prefix before the `kPackedRowV1` marker, and the same TTL is
also prepended to every packed column value (`cql_operation.cc:1112`). Because every YCQL column
is varlen, each packed column can carry its own control-fields prefix, so a later
`UPDATE ... USING TTL` of a single column simply writes a standalone cell that overrides just
that column while the rest of the packed row keeps the original TTL.

For example, for a YCQL table with schema version 1:

```sql
CREATE TABLE t (k INT PRIMARY KEY, v1 INT, v2 TEXT);
INSERT INTO t (k, v1, v2) VALUES (1, 42, 'hello') USING TTL 3600;
```

the packed RocksDB value is:

```
74 F0 36 EE 80               row control fields: kTtl ('t'), signed varint 3600000 ms
7A                           kPackedRowV1 ('z')
01                           schema version 1 (varint)
0A 00 00 00                  end offset of v1 data: 10 (uint32, little-endian)
15 00 00 00                  end offset of v2 data: 21
74 F0 36 EE 80               v1 control fields: kTtl, 3600000 ms
48 00 00 00 2A               v1: kInt32 ('H'), int32 42 (big-endian)
74 F0 36 EE 80               v2 control fields: kTtl, 3600000 ms
53 68 65 6C 6C 6F            v2: kString ('S'), "hello"
```

The row-level control fields precede the `kPackedRowV1` marker and apply to the liveness column,
while each packed column carries its own control-fields prefix inside its data slot - note that
the varlen end offsets (10 and 21) include the per-column control fields.

Value timestamp (`USING TIMESTAMP`): disables packing for the whole statement
(`cql_operation.cc:1105`). A user-supplied timestamp may be older than data already present, and
a packed row cannot lose to a per-column comparison, so the write falls back to per-column
cells.

Read path (`doc_reader.cc`): for the liveness column the row-level control fields apply; for
other V1 columns per-column control fields are decoded from the value prefix
(`ObtainControlFields`, `doc_reader.cc:296`). Per-column expiration is computed by merging the
column TTL with the row and table defaults (`GetNewExpiration`); expired packed columns are
treated as absent. `WRITETIME()` and `TTL()` are served from the user timestamp or the row write
time and the remaining TTL. YCQL always performs TTL checks on packed rows
(`TtlCheckRequired`, `doc_reader.cc:1238`); YSQL skips them.

Compaction: an expired packed row is removed (major) or replaced by a tombstone (minor). During
repack, YCQL preserves original write times: columns merged into a new packed row get their old
hybrid time injected as a user timestamp in the per-column control fields when they do not
already carry one (`docdb_compaction_context.cc:276`), so `WRITETIME()` stays correct after
rewriting.

Other YCQL limitations: only INSERT packs (never UPDATE or DELETE), collections and static
columns are never packed (frozen values pack as primitives), counters are updates and therefore
never packed, and an insert that does not supply every packable column falls back to per-column
writes (`CouldPack`, `schema_packing.cc:620`).
