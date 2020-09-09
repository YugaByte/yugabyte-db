```
alter_table ::= ALTER TABLE [ ONLY ] name [ * ] alter_table_action 
                [ , ... ]

alter_table_action ::= ADD [ COLUMN ] column_name data_type
                       | RENAME TO table_name
                       | DROP [ COLUMN ] column_name 
                         [ RESTRICT | CASCADE ]
                       | ADD alter_table_constraint
                       | DROP CONSTRAINT constraint_name 
                         [ RESTRICT | CASCADE ]
                       | RENAME [ COLUMN ] column_name TO column_name
                       | DISABLE ROW LEVEL SECURITY
                       | ENABLE ROW LEVEL SECURITY
                       | FORCE ROW LEVEL SECURITY
                       | NO FORCE ROW LEVEL SECURITY

alter_table_constraint ::= [ CONSTRAINT constraint_name ] 
                           { CHECK ( expression )
                             | UNIQUE ( column_names ) 
                               index_parameters
                             | FOREIGN KEY ( column_names ) 
                               references_clause }
```
