```
select_start ::= SELECT [ ALL | 
                          DISTINCT [ ON { ( expression [ , ... ] ) } ] ] 
                 [ * | { { expression
                           | fn_over_window
                           | ordinary_aggregate_fn_invocation
                           | within_group_aggregate_fn_invocation } 
                       [ [ AS ] name ] } [ , ... ] ]

window_clause ::= WINDOW { { name AS window_definition } [ , ... ] }

fn_over_window ::= name  ( [ expression [ , ... ] | * ]  
                   [ FILTER ( WHERE boolean_expression ) ] OVER 
                   { window_definition | name }
```
