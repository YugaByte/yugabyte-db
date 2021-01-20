```
create_table ::= CREATE [ TEMPORARY | TEMP ] TABLE [ IF NOT EXISTS ] 
                 table_name ( [ table_elem [ , ... ] ] ) 
                 [ WITH ( { COLOCATED = { 'true' | 'false' }
                            | storage_parameters } )
                   | WITHOUT OIDS ] 
                 [ SPLIT { INTO integer TABLETS
                           | AT VALUES ( split_row [ , ... ] ) } ]

table_elem ::= column_name data_type [ column_constraint [ ... ] ]
               | table_constraint

column_constraint ::= [ CONSTRAINT constraint_name ] 
                      { NOT NULL
                        | NULL
                        | CHECK ( expression )
                        | DEFAULT expression
                        | UNIQUE index_parameters
                        | PRIMARY KEY
                        | references_clause } 
                      [ DEFERRABLE | NOT DEFERRABLE ] 
                      [ INITIALLY DEFERRED | INITIALLY IMMEDIATE ]

table_constraint ::= [ CONSTRAINT constraint_name ] 
                     { CHECK ( expression )
                       | UNIQUE ( column_names ) index_parameters
                       | PRIMARY KEY ( key_columns )
                       | FOREIGN KEY ( column_names ) 
                         references_clause } 
                     [ DEFERRABLE | NOT DEFERRABLE ] 
                     [ INITIALLY DEFERRED | INITIALLY IMMEDIATE ]

key_columns ::= hash_columns [ , range_columns ] | range_columns

hash_columns ::= column_name [ HASH ] | ( column_name [ , ... ] ) HASH

range_columns ::= { column_name { ASC | DESC } } [ , ... ]

storage_parameters ::= storage_parameter [ , ... ]

storage_parameter ::= param_name [ = param_value ]

index_parameters ::= [ INCLUDE ( column_names ) ] 
                     [ WITH ( storage_parameters ) ]

references_clause ::= REFERENCES table_name [ column_name [ , ... ] ] 
                      [ MATCH FULL | MATCH PARTIAL | MATCH SIMPLE ]  
                      [ ON DELETE key_action ] 
                      [ ON UPDATE key_action ]

split_row ::= ( column_value [ , ... ] )
```
