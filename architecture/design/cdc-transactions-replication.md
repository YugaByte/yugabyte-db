# CDC - Replication of Transactional Writes

This document outlines the design for replicating transactions by using [Change Data Capture (CDC)](https://github.com/yugabyte/yugabyte-db/blob/master/architecture/design/docdb-change-data-capture.md).

## CDC and Transactions in the WAL files


Currently CDC does not replicate any writes that are part of a transaction. CDC replication happens at the WAL entry level, and we assume that the CDC consumer will not implement some kind of transaction manager to keep track of pending transactions and abort them if necessary, so when CDC encounters a WRITE entry that is part of a transaction, there are three possible scenarios:
1. The WRITE is part of a pending transaction, therefore the data hasn’t been written to RocksDB since it could still be aborted. This data resides in a provisional writes DB that we call intentsDB. We know when a transaction is pending by querying the transaction manager.
2. The transaction is not pending, but it was aborted. An aborted transaction doesn’t generate a WAL entry with the abort operation.
3. The transaction is not pending, and it was committed. A committed transaction generates a WAL entry with the commit operation.

For case (1), we shouldn’t replicate the entry because we don’t know if it will be aborted or committed, and also because we don’t have a time at which this write should be visible.

For case (2),We should skip  the write entry.

For case (3), the write should be replicated, but we need to know the commit time so that we reorder this write to happen at the commit time.

Since we only want to replicate writes that have been committed, the next question is how to find the commit entry (if it exists) in the WAL files. A naive solution is to scan the entries following a transactional write entry, but this would be very inefficient because we would have to read a lot of data from disk, and we could potentially read all the subsequent WAL files if the transaction was aborted since abort operations don’t generate WAL entries.

If we wanted to decide what to do with a write entry upon reading it, we could create a transaction WAL index or hash table where we would use the transaction id to quickly find a commit entry in the WAL files if it exists. But we have decided against any solution that requires us to look ahead in the WAL files.

## Design

Instead of looking forward for a commit entry every time we find a transactional write entry, we want to be able to get all the writes that are part of a transaction every time we see a commit entry so that we can send al those write entries to the CDC consumer. This means that commit entries are never replicated because the consumer only sees the final state of a transaction. Currently, the intentsDB cleans up provisional records when the transaction is committed, and the records have been copied to the RocksDB database. So we want to modify the intentsDB so that these records are not cleaned when these two events happen. Instead, we want the CDC producer to inform the intentsDB when it’s safe to remove the provisional writes. Also, we need to modify the intentsDB so that for each provisional write, it can provide a WAL entry (in the same format write entries are written to the WAL).

So the new flow should be as follows:
1. CDC producer reads a WRITE record that is part of a transaction and it ignores it.
2. When a COMMIT record is received by the intentsDB, it copies the provisional records to rocksdb just like before, but it doesn’t clean up those records from its database.
3. When the CDC producer reads a COMMIT record from the WAL file, it queries the intentsdb to get all the writes that are part of this transaction as WAL entries.
4. Once the CDC producer has replicated all the WRITE entries that are part of the transaction, it will inform the intentsDB that it is safe to remove those provisional records from its database.

A side effect of this new workflow is that the CDC producer doesn’t have to reorder log entries anymore since the COMMIT entries will be in the correct order in the log files.


[![Analytics](https://yugabyte.appspot.com/UA-104956980-4/architecture/design/cdc-transactions-replication.md?pixel&useReferer)](https://github.com/yugabyte/ga-beacon)
