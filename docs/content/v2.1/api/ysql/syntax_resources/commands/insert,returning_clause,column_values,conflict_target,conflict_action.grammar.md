```
insert ::= [ WITH [ RECURSIVE ] with_query [ , ... ] ]  INSERT INTO 
           table_name [ AS alias ] [ ( column_names ) ]  
           { DEFAULT VALUES
             | VALUES ( column_values ) [ ,(column_values ... ]
             | subquery }  
           [ ON CONFLICT [ conflict_target ] conflict_action ]  
           [ returning_clause ]

returning_clause ::= RETURNING { * | { output_expression 
                                     [ [ AS ] output_name ] } 
                                     [ , ... ] }

column_values ::= { expression | DEFAULT } [ , ... ]

conflict_target ::= ( { column_name | expression } [ , ... ] ) 
                    [ WHERE condition ]
                    | ON CONSTRAINT constraint_name

conflict_action ::= DO NOTHING
                    | DO UPDATE SET update_item [ , ... ] 
                      [ WHERE condition ]
```
