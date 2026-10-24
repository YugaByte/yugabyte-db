---
title: Vector LSM for vector indexes
headerTitle: Vector LSM
linkTitle: Vector LSM
description: How YugabyteDB uses a specialized log-structured merge tree for managing vector indexes and approximate nearest neighbor search.
headcontent: Learn how Vector LSM manages vector indexes and handles compaction
menu:
  stable:
    identifier: docdb-vector-lsm
    parent: docdb
    weight: 350
type: docs
---

Vector LSM is a specialized log-structured merge (LSM) tree implementation designed specifically for managing vector indexes in YugabyteDB. Unlike the traditional [RocksDB LSM](../lsm-sst/) which stores key-value pairs, Vector LSM manages collections of vectors optimized for approximate nearest neighbor (ANN) search and high-dimensional similarity queries.

## Overview

As vector data is inserted into tables with vector indexes, YugabyteDB's Vector LSM automatically organizes and maintains the vectors for efficient search. The system maintains multiple vector chunks (similar to SST files in RocksDB) that are periodically merged through a compaction process.

The key motivation for Vector LSM is to provide:

- Automatic index maintenance. Vectors are kept in sync with table changes (inserts, updates, deletes) without manual rebuilding.
- Scalability. Vectors are automatically distributed across nodes using the same sharding strategy as tables.
- Performance. Optimized for both write throughput during bulk inserts and query performance during searches.
- Memory efficiency. Controlled memory consumption during compaction to prevent out-of-memory (OOM) failures.

## Comparison to RocksDB LSM

While RocksDB LSM and Vector LSM share the same overall philosophy of separating writes and reads, they differ significantly:

| Aspect | RocksDB LSM | Vector LSM |
|--------|-------------|-----------|
| Data Type | Key-value pairs | Vectors (high-dimensional data) |
| Primary Operation | Range scans and lookups | Nearest neighbor search |
| Index Structure | Sorted by key with bloom filters | Algorithm-specific (HNSW, USearch, FAISS, and others.) |
| Merge Strategy | Simple concatenation and sorting | Complex vector merge (reconstructs nearest neighbor graph) |
| Memory Constraints | Less critical during merge | Critical – OOM during merge is a major concern |
| Concurrency | Single-threaded compaction typical | Multi-threaded merge operations |

## Vector chunk lifecycle

### Chunk creation

When vectors are inserted into a table with a vector index:

1. Accumulation: Vectors are buffered in memory (memtable-like structure).
2. Chunk formation: Once a threshold is reached, the vector batch is built into an _immutable chunk_ using the configured vector index algorithm (for example, HNSW).
3. Persistent storage: The chunk is written to disk as a chunk file.

### Chunk search

Queries perform approximate nearest neighbor search across all chunks:

1. Multi-chunk search. The search algorithm queries each chunk independently.
2. Result merging. Results from all chunks are merged to find the overall nearest neighbors.
3. Ranking. Final results are sorted by distance to the query vector.

This approach is simple and works well when chunk counts are manageable, but performance degrades as chunks accumulate.

## Compaction

As chunks accumulate over time, compaction merges multiple chunks into fewer, larger chunks. This maintains query performance by reducing the number of chunks that must be searched.

### Compaction process

1. Selection: The system selects a set of chunks to compact (typically those under a size threshold).
2. Input reading: All vectors from selected chunks are read into memory.
3. Rebuilding: A new vector index is built from the combined vectors using the configured algorithm.
4. Output writing: The merged result is written as a new chunk file(s).
5. Cleanup: Original input chunks are deleted.

### Compaction triggers

Compaction may be triggered by:

- Background compaction: Periodic automatic compaction when too many chunks exist.
- Manual compaction: Explicitly triggered via `compact_table` commands.
- Maintenance compaction: During tablet operations like splitting or rebalancing.

## Memory management and chunked compaction

### The OOM problem

The biggest challenge during Vector LSM compaction is _memory consumption_. When multiple chunks are merged:

- All vectors from input chunks must be loaded into memory.
- The new vector index is built entirely in memory before being written to disk.
- For large compactions (merging hundreds of millions of vectors), this can exceed available memory.

### Chunked compaction solution

To prevent OOM errors, Vector LSM supports chunked compaction, breaking the output of a single compaction into multiple output chunks, each bounded by a configurable memory limit.

Instead of:

```text
Input Chunks [A, B, C] → [Single Large Output Chunk]
```

Chunked compaction produces:

```text
Input Chunks [A, B, C] → [Output Chunk 1] [Output Chunk 2] [Output Chunk 3]
```

Each output chunk respects the memory limit while still combining data from multiple input chunks, which improves performance compared to having many small input chunks.

### Memory limit configuration

Two flags control the memory limits during chunked compaction:

1. **Absolute limit** ([`--vector_index_compaction_chunk_max_mem_store_size_mb`](../../reference/configuration/yb-tserver/#vector-index-compaction-chunk-max-mem-store-size-mb))
   - Specifies a hard memory limit in MB for each output chunk
   - When set to non-zero, takes priority over the percentage-based limit
   - Default: `0` (unlimited – produces single output chunk)

2. **Percentage-based limit** ([`--vector_index_compaction_chunk_max_mem_store_size_percentage`](../../reference/configuration/yb-tserver/#vector-index-compaction-chunk-max-mem-store-size-percentage))
   - Specifies the limit as a percentage of the vector index block cache capacity
   - Ignored when the MB flag is set or when concurrent compactions are allowed
   - Default: `60%` of block cache capacity
   - Ensures chunked compaction is enabled by default, reducing OOM risk

### Default behavior

With default settings, chunked compaction is **enabled automatically**:

- Output chunks are capped at 60% of the vector index block cache
- This prevents runaway memory growth while still allowing reasonable chunk merging
- If you experience OOM during compaction, you can lower the percentage or set an absolute MB limit

### Concurrent compaction interaction

When multiple compactions can run in parallel (`vector_index_num_compactions_limit` > 1):

- The percentage-based limit is ignored (because multiple concurrent merges in memory could still exceed capacity)
- You must set an absolute MB limit to prevent OOM
- Consider the total memory: if N compactions can run simultaneously, ensure N × compaction_memory < available_memory

## Filtering during compaction

During compaction, vectors may be filtered out (for example, soft-deleted rows or filtered by time-based constraints). The system uses a merge filter to determine which vectors should be included in the output chunk. This helps reclaim space from obsolete vectors, similar to how RocksDB removes tombstones during compaction.

## Monitoring and tuning

### Key metrics

Monitor these metrics to understand Vector LSM behavior:

| Metric | Description |
| :-- | :-- |
| Chunk count | Number of chunks per tablet (lower is better for query performance) |
| Compaction frequency | How often compactions occur |
| Compaction duration | Time spent in compaction (indicates memory pressure if high) |
| Memory usage during compaction | Peak memory used by the merge process |

### Tuning guidelines

- Increase memory limit if compactions are failing with OOM errors.
- Decrease memory limit if memory usage is consistently high and available memory is constrained.
- Increase `vector_index_num_compactions_limit` to allow more parallelism, but ensure sufficient memory.
- Monitor chunk count if it's growing unbounded, increase compaction frequency.

## Related configuration

Beyond the two chunked compaction flags, several other flags control Vector LSM behavior:

- [`--vector_index_num_compactions_limit`](../../../reference/configuration/yb-tserver/#vector-index-num-compactions-limit) – Number of concurrent compactions per tserver
- [`--vector_index_files_number_compaction_trigger`](../../../reference/configuration/yb-tserver/#vector-index-files-number-compaction-trigger) – Number of files to trigger compaction
- [`--vector_index_compaction_always_include_size_threshold`](../../../reference/configuration/yb-tserver/#vector-index-compaction-always-include-size-threshold) – Always include small chunks in compaction by size ratio

## Learn more

- Blog on [YugabyteDB Vector Indexing Architecture](https://www.yugabyte.com/blog/yugabytedb-vector-indexing-architecture/)
- [Gen AI Apps Guide](../../../explore/gen-ai-apps/): Building AI applications with YugabyteDB vectors
- [pgvector Extension](../../../additional-features/pg-extensions/extension-pgvector/): SQL interface for vector operations
