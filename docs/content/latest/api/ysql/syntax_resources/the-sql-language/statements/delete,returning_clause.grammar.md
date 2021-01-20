```
delete ::= [ WITH [ RECURSIVE ] with_clause_substatement_defn 
             [ , ... ] ]  DELETE FROM [ ONLY ] table_name [ * ] 
           [ [ AS ] alias ]  [ WHERE boolean_expression
                               | WHERE CURRENT OF cursor_name ]  
           [ returning_clause ]

returning_clause ::= RETURNING { * | { output_expression 
                                     [ [ AS ] output_name ] } 
                                     [ , ... ] }
```
