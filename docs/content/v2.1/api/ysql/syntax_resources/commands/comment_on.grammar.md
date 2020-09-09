```
comment_on ::= COMMENT ON 
               { ACCESS METHOD object_name
                 | AGGREGATE aggregate_name ( aggregate_signature )
                 | CAST ( source_type AS target_type )
                 | COLLATION object_name
                 | COLUMN relation_name . column_name
                 | CONSTRAINT constraint_name ON table_name
                 | CONSTRAINT constraint_name ON DOMAIN domain_name
                 | CONVERSION object_name
                 | DATABASE object_name
                 | DOMAIN object_name
                 | EXTENSION object_name
                 | EVENT TRIGGER object_name
                 | FOREIGN DATA WRAPPER object_name
                 | FOREIGN TABLE object_name
                 | FUNCTION function_name [ ( function_signature ) ]
                 | INDEX object_name
                 | LARGE OBJECT large_object_oid
                 | MATERIALIZED VIEW object_name
                 | OPERATOR operator_name ( operator_signature )
                 | OPERATOR CLASS object_name USING index_method
                 | OPERATOR FAMILY object_name USING index_method
                 | POLICY policy_name ON table_name
                 | [ PROCEDURAL ] LANGUAGE object_name
                 | PROCEDURE procedure_name 
                   [ ( [ [ argmode ] [ argname ] argtype [ , ... ] ] ) ]
                 | PUBLICATION object_name
                 | ROLE object_name
                 | ROUTINE routine_name 
                   [ ( [ [ argmode ] [ argname ] argtype [ , ... ] ] ) ]
                 | RULE rule_name ON table_name
                 | SCHEMA object_name
                 | SEQUENCE object_name
                 | SERVER object_name
                 | STATISTICS object_name
                 | SUBSCRIPTION object_name
                 | TABLE object_name
                 | TABLESPACE object_name
                 | TEXT SEARCH CONFIGURATION object_name
                 | TEXT SEARCH DICTIONARY object_name
                 | TEXT SEARCH PARSER object_name
                 | TEXT SEARCH TEMPLATE object_name
                 | TRANSFORM FOR type_name LANGUAGE lang_name
                 | TRIGGER trigger_name ON table_name
                 | TYPE object_name
                 | VIEW object_name } IS { '<Text Literal>' | NULL }
```
