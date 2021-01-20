```
create_operator_class ::= CREATE OPERATOR CLASS operator_class_name 
                          [ DEFAULT ] FOR TYPE data_type USING 
                          index_method AS operator_class_as [ , ... ]

operator_class_as ::= OPERATOR strategy_number operator_name 
                      [ ( operator_signature ) ] [ FOR SEARCH ]
                      | FUNCTION support_number 
                        [ ( op_type [ , ... ] ) ] function_name ( 
                        function_signature )
                      | STORAGE storage_type
```
