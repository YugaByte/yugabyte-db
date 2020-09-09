```
select ::= [ WITH [ RECURSIVE ] { with_query [ , ... ] } ]  SELECT 
           [ ALL | DISTINCT [ ON { ( expression [ , ... ] ) } ] ] 
           [ * | { { expression | fn_over_window } [ [ AS ] name ] } 
                 [ , ... ] ]  [ FROM { from_item [ , ... ] } ]  
           [ WHERE condition ]  
           [ GROUP BY { grouping_element [ , ... ] } ]  
           [ HAVING { condition [ , ... ] } ]  
           [ WINDOW { { name AS window_definition } [ , ... ] } ]  
           [ { UNION | INTERSECT | EXCEPT } [ ALL | DISTINCT ] select ] 
            [ ORDER BY { order_expr [ , ... ] } ]  
           [ LIMIT [ integer | ALL ] ]  
           [ OFFSET integer [ ROW | ROWS ] ]

order_expr ::= expression [ ASC | DESC | USING operator_name ] 
               [ NULLS { FIRST | LAST } ]
```
