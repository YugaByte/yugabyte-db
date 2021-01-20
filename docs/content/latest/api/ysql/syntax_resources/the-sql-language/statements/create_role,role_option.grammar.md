```
create_role ::= CREATE ROLE role_name 
                [ [ WITH ] role_option [ , ... ] ]

role_option ::= SUPERUSER
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
                | IN ROLE role_name [ , ... ]
                | IN GROUP role_name [ , ... ]
                | ROLE role_name [ , ... ]
                | ADMIN role_name [ , ... ]
                | USER role_name [ , ... ]
                | SYSID uid
```
