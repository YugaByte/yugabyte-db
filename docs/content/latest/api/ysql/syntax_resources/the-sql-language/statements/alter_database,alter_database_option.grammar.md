```
alter_database ::= ALTER DATABASE name 
                   [ [ WITH ] alter_database_option [ ... ]
                     | RENAME TO name
                     | OWNER TO { new_owner
                                  | CURRENT_USER
                                  | SESSION_USER }
                     | SET configuration_parameter { TO | = } 
                       { value | DEFAULT }
                     | SET configuration_parameter FROM CURRENT
                     | RESET configuration_parameter
                     | RESET ALL ]

alter_database_option ::= ALLOW_CONNECTIONS allowconn
                          | CONNECTION LIMIT connlimit
                          | IS_TEMPLATE istemplate
```
