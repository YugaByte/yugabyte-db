```
drop_operator ::= DROP OPERATOR [ IF EXISTS ] 
                  { operator_name ( operator_signature ) } [ , ... ] 
                  [ CASCADE | RESTRICT ]

operator_signature ::= { left_type | NONE } , { right_type | NONE }
```
