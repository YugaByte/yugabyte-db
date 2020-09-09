```
create_policy ::= CREATE POLICY name ON table_name 
                  [ AS { PERMISSIVE | RESTRICTIVE } ] 
                  [ FOR { ALL | SELECT | INSERT | UPDATE | DELETE } ] 
                  [ TO { role_name
                         | PUBLIC
                         | CURRENT_USER
                         | SESSION_USER } [ , ... ] ] 
                  [ USING ( using_expression ) ] 
                  [ WITH CHECK ( check_expression ) ]
```
