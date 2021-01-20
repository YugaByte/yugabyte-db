```
select_start ::= SELECT [ ALL | 
                          DISTINCT [ ON { ( expression [ , ... ] ) } ] ] 
                 [ * | { { expression
                           | fn_over_window
                           | ordinary_aggregate_fn_invocation
                           | within_group_aggregate_fn_invocation } 
                       [ [ AS ] name ] } [ , ... ] ]

ordinary_aggregate_fn_invocation ::= name  ( 
                                     { [ ALL | DISTINCT ] expression 
                                       [ , ... ]
                                       | * } 
                                     [ ORDER BY order_expr [ , ... ] ] 
                                     )  [ FILTER ( WHERE 
                                          boolean_expression ) ]

within_group_aggregate_fn_invocation ::= name  ( 
                                         { expression [ , ... ] } )  
                                         WITHIN GROUP ( ORDER BY 
                                         order_expr [ , ... ] )  
                                         [ FILTER ( WHERE 
                                           boolean_expression ) ]
```
