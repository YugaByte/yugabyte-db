```
drop_aggregate ::= DROP AGGREGATE [ IF EXISTS ] 
                   { aggregate_name ( aggregate_signature ) } 
                   [ , ... ] [ CASCADE | RESTRICT ]

aggregate_signature ::= * | aggregate_arg [ , ... ]
                        | [ aggregate_arg [ , ... ] ] ORDER BY 
                          aggregate_arg [ , ... ]
```
