```
create_index ::= CREATE [ UNIQUE ] INDEX [ [ IF NOT EXISTS ] name ]  
                 ON [ ONLY ] table_name ( index_elem [ , ... ] )  
                 [ INCLUDE ( column_name [ , ... ] ) ]  
                 [ WHERE boolean_expression ]

index_elem ::= { column_name | ( expression ) } 
               [ operator_class_name ] [ HASH | ASC | DESC ] 
               [ NULLS { FIRST | LAST } ]
```
