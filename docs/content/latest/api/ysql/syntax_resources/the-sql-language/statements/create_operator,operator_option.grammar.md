```
create_operator ::= CREATE OPERATOR operator_name ( 
                    { FUNCTION = function_name
                      | PROCEDURE = procedure_name } 
                    [ , operator_option [ ... ] ] )

operator_option ::= LEFTARG = left_type
                    | RIGHTARG = right_type
                    | COMMUTATOR = com_op
                    | NEGATOR = neg_op
                    | RESTRICT = res_proc
                    | JOIN = join_proc
                    | HASHES
                    | MERGES
```
