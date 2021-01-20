```
create_trigger ::= CREATE TRIGGER name { BEFORE | AFTER | INSTEAD OF } 
                   { event [ OR ... ] } ON table_name 
                   [ FROM table_name ]  [ NOT DEFERRABLE ] 
                   [ FOR [ EACH ] { ROW | STATEMENT } ] 
                   [ WHEN ( boolean_expression ) ]  EXECUTE 
                   { FUNCTION | PROCEDURE } function_name ( 
                   function_arguments )

event ::= INSERT
          | UPDATE [ OF column_name [ , ... ] ]
          | DELETE
          | TRUNCATE
```
