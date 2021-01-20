```
alter_user ::= ALTER USER role_specification 
               [ [ WITH ] alter_role_option [ , ... ] ]

alter_role_option ::= SUPERUSER
                      | NOSUPERUSER
                      | CREATEDB
                      | NOCREATEDB
                      | CREATEROLE
                      | NOCREATEROLE
                      | INHERIT
                      | NOINHERIT
                      | LOGIN
                      | NOLOGIN
                      | CONNECTION LIMIT connlimit
                      | [ ENCRYPTED ] PASSWORD  ' password ' 
                      | PASSWORD NULL
                      | VALID UNTIL  ' timestamp ' 

role_specification ::= role_name | CURRENT_USER | SESSION_USER

alter_user_rename ::= ALTER USER role_name RENAME TO new_role_name

alter_user_config ::= ALTER USER { role_specification | ALL } 
                      [ IN DATABASE database_name ] config_setting

config_setting ::= SET config_param { TO | = } 
                   { config_value | DEFAULT }
                   | SET config_param FROM CURRENT
                   | RESET config_param
                   | RESET ALL
```
