```
alter_policy ::= ALTER POLICY name ON table_name 
                 [ TO { role_name
                        | PUBLIC
                        | CURRENT_USER
                        | SESSION_USER } [ , ... ] ] 
                 [ USING ( using_expression ) ] 
                 [ WITH CHECK ( check_expression ) ]

alter_policy_rename ::= ALTER POLICY name ON table_name RENAME TO 
                        new_name
```
