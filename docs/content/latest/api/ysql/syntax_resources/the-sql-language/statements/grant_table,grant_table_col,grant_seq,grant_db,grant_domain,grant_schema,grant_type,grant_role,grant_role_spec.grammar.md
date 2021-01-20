```
grant_table ::= GRANT 
                { { SELECT
                    | INSERT
                    | UPDATE
                    | DELETE
                    | TRUNCATE
                    | REFERENCES
                    | TRIGGER } [ , ... ]
                  | ALL [ PRIVILEGES ] } ON 
                { [ TABLE ] table_name [ , ... ]
                  | ALL TABLES IN SCHEMA schema_name [ , ... ] } TO 
                grant_role_spec [ , ... ] [ WITH GRANT OPTION ]

grant_table_col ::= GRANT 
                    { { SELECT | INSERT | UPDATE | REFERENCES } ( 
                      column_names ) [ ,(column_names ... ]
                      | ALL [ PRIVILEGES ] ( column_names ) } ON 
                    { [ TABLE ] table_name [ , ... ] } TO 
                    grant_role_spec [ , ... ] [ WITH GRANT OPTION ]

grant_seq ::= GRANT { { USAGE | SELECT | UPDATE } [ , ... ]
                      | ALL [ PRIVILEGES ] } ON 
              { SEQUENCE sequence_name [ , ... ]
                | ALL SEQUENCES IN SCHEMA schema_name 
                  [ , sequence_name [ ... ] ] } TO grant_role_spec 
              [ , ... ] [ WITH GRANT OPTION ]

grant_db ::= GRANT { { CREATE | CONNECT | TEMPORARY | TEMP } [ , ... ]
                     | ALL [ PRIVILEGES ] } ON DATABASE database_name 
             [ , ... ] TO grant_role_spec [ , ... ] 
             [ WITH GRANT OPTION ]

grant_domain ::= GRANT { USAGE | ALL [ PRIVILEGES ] } ON DOMAIN 
                 domain_name [ , ... ] TO grant_role_spec [ , ... ] 
                 [ WITH GRANT OPTION ]

grant_schema ::= GRANT { { CREATE | USAGE } [ , ... ]
                         | ALL [ PRIVILEGES ] } ON SCHEMA schema_name 
                 [ , ... ] TO grant_role_spec [ , ... ] 
                 [ WITH GRANT OPTION ]

grant_type ::= GRANT { USAGE | ALL [ PRIVILEGES ] } ON TYPE type_name 
               [ , ... ] TO grant_role_spec [ , ... ] 
               [ WITH GRANT OPTION ]

grant_role ::= GRANT role_name [ , ... ] TO role_name [ , ... ] 
               [ WITH ADMIN OPTION ]

grant_role_spec ::= [ GROUP ] role_name
                    | PUBLIC
                    | CURRENT_USER
                    | SESSION_USER
```
