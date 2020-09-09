```
select_start ::= SELECT [ ALL | 
                          DISTINCT [ ON { ( expression [ , ... ] ) } ] ] 
                 [ * | { { expression | fn_over_window } 
                       [ [ AS ] name ] } [ , ... ] ]

window_clause ::= [ WINDOW { { name AS window_definition } [ , ... ] } ]

fn_over_window ::= fn_invocation 
                   [ FILTER ( WHERE { boolean_expression [ , ... ] } ) ] 
                   OVER { window_definition | name }
```
