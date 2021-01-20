```
drop_function ::= DROP { FUNCTION | PROCEDURE } [ IF EXISTS ]  
                  { name [ ( [ argtype_decl [ , ... ] ] ) ] } 
                  [ , ... ] [ CASCADE | RESTRICT ]

argtype_decl ::= [ argmode ] [ argname ] argtype
```
