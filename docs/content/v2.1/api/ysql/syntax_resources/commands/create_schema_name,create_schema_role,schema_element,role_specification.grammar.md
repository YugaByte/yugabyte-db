```
create_schema_name ::= CREATE SCHEMA [ IF NOT EXISTS ] schema_name 
                       [ AUTHORIZATION role_specification ] 
                       [ schema_element [ ... ] ]

create_schema_role ::= CREATE SCHEMA [ IF NOT EXISTS ] AUTHORIZATION 
                       role_specification [ schema_element [ ... ] ]

schema_element ::= '<YSQL DDL statement>'

role_specification ::= role_name | CURRENT_USER | SESSION_USER
```
