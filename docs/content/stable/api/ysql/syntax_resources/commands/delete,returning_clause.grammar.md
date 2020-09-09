```
delete ::= [ WITH [ RECURSIVE ] with_query [ , ... ] ]  DELETE FROM 
           [ ONLY ] table_name [ * ] [ [ AS ] alias ]  
           [ WHERE condition | WHERE CURRENT OF cursor_name ]  
           [ returning_clause ]

returning_clause ::= RETURNING { * | { output_expression 
                                     [ [ AS ] output_name ] } 
                                     [ , ... ] }
```
