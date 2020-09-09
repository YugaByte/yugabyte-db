```
alter_group ::= ALTER GROUP role_specification { ADD | DROP } USER 
                role_name [ , ... ]

role_specification ::= role_name | CURRENT_USER | SESSION_USER

alter_group_rename ::= ALTER GROUP role_name RENAME TO new_role_name
```
