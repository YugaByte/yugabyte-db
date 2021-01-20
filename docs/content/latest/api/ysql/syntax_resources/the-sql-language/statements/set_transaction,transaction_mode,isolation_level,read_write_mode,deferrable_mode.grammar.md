```
set_transaction ::= SET TRANSACTION transaction_mode [ ... ]

transaction_mode ::= isolation_level
                     | read_write_mode
                     | deferrable_mode

isolation_level ::= ISOLATION LEVEL { READ UNCOMMITTED
                                      | READ COMMITTED
                                      | REPEATABLE READ
                                      | SERIALIZABLE }

read_write_mode ::= READ ONLY | READ WRITE

deferrable_mode ::= [ NOT ] DEFERRABLE
```
