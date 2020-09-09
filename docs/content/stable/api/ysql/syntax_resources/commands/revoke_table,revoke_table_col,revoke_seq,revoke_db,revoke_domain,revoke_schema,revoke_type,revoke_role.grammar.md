```
revoke_table ::= REVOKE [ GRANT OPTION FOR ] 
                 { { SELECT
                     | INSERT
                     | UPDATE
                     | DELETE
                     | TRUNCATE
                     | REFERENCES
                     | TRIGGER } [ , ... ]
                   | ALL [ PRIVILEGES ] } ON 
                 { [ TABLE ] table_name [ , ... ]
                   | ALL TABLES IN SCHEMA schema_name [ , ... ] } FROM 
                 { [ GROUP ] role_name | PUBLIC } [ , ... ] 
                 [ CASCADE | RESTRICT ]

revoke_table_col ::= REVOKE [ GRANT OPTION FOR ] 
                     { { SELECT | INSERT | UPDATE | REFERENCES } ( 
                       column_names ) [ ,(column_names ... ]
                       | ALL [ PRIVILEGES ] ( column_names ) } ON 
                     [ TABLE ] table_name [ , ... ] FROM 
                     { [ GROUP ] role_name | PUBLIC } [ , ... ] 
                     [ CASCADE | RESTRICT ]

revoke_seq ::= REVOKE [ GRANT OPTION FOR ] 
               { { USAGE | SELECT | UPDATE } [ , ... ]
                 | ALL [ PRIVILEGES ] } ON 
               { SEQUENCE sequence_name [ , ... ]
                 | ALL SEQUENCES IN SCHEMA schema_name [ , ... ] } 
               FROM { [ GROUP ] role_name | PUBLIC } [ , ... ] 
               [ CASCADE | RESTRICT ]

revoke_db ::= REVOKE [ GRANT OPTION FOR ] 
              { { CREATE | CONNECT | TEMPORARY | TEMP } [ , ... ]
                | ALL [ PRIVILEGES ] } ON DATABASE database_name 
              [ , ... ] FROM { [ GROUP ] role_name | PUBLIC } 
              [ , ... ] [ CASCADE | RESTRICT ]

revoke_domain ::= REVOKE [ GRANT OPTION FOR ] 
                  { USAGE | ALL [ PRIVILEGES ] } ON DOMAIN domain_name 
                  [ , ... ] FROM { [ GROUP ] role_name | PUBLIC } 
                  [ , ... ] [ CASCADE | RESTRICT ]

revoke_schema ::= REVOKE [ GRANT OPTION FOR ] 
                  { { CREATE | USAGE } [ , ... ]
                    | ALL [ PRIVILEGES ] } ON SCHEMA schema_name 
                  [ , ... ] FROM { [ GROUP ] role_name | PUBLIC } 
                  [ , ... ] [ CASCADE | RESTRICT ]

revoke_type ::= REVOKE [ GRANT OPTION FOR ] 
                { USAGE | ALL [ PRIVILEGES ] } ON TYPE type_name 
                [ , ... ] FROM { [ GROUP ] role_name | PUBLIC } 
                [ , ... ] [ CASCADE | RESTRICT ]

revoke_role ::= REVOKE [ ADMIN OPTION FOR ] role_name [ , ... ] FROM 
                role_name [ , ... ] [ CASCADE | RESTRICT ]
```
