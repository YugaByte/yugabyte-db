```
update ::= [ WITH [ RECURSIVE ] with_query [ , ... ] ]  UPDATE 
           [ ONLY ] table_name [ * ] [ [ AS ] alias ]  SET update_item 
           [ , ... ] [ WHERE condition
                       | WHERE CURRENT OF cursor_name ]  
           [ returning_clause ]

returning_clause ::= RETURNING { * | { output_expression 
                                     [ [ AS ] output_name ] } 
                                     [ , ... ] }

update_item ::= column_name = column_value
                | ( column_names ) = [ ROW ] ( column_values )
                | ( column_names ) = ( query )

column_values ::= { expression | DEFAULT } [ , ... ]

column_names ::= column_name [ , ... ]
```
