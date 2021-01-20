```
explain ::= EXPLAIN [ [ ANALYZE ] [ VERBOSE ] | ( option [ , ... ] ) ] 
            statement

option ::= ANALYZE [ boolean ]
           | VERBOSE [ boolean ]
           | COSTS [ boolean ]
           | BUFFERS [ boolean ]
           | TIMING [ boolean ]
           | SUMMARY [ boolean ]
           | FORMAT { TEXT | XML | JSON | YAML }
```
