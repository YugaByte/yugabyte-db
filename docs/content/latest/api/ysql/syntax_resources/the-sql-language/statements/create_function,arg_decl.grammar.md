```
create_function ::= CREATE [ OR REPLACE ] FUNCTION name ( 
                    [ arg_decl [ , ... ] ] )  
                    [ RETURNS type_name
                      | RETURNS TABLE ( { column_name type_name } 
                        [ , ... ] ) ]  
                    { LANGUAGE lang_name
                      | TRANSFORM { FOR TYPE type_name } [ , ... ]
                      | WINDOW
                      | IMMUTABLE
                      | STABLE
                      | VOLATILE
                      | [ NOT ] LEAKPROOF
                      | CALLED ON NULL INPUT
                      | RETURNS NULL ON NULL INPUT
                      | STRICT
                      | [ EXTERNAL ] SECURITY INVOKER
                      | [ EXTERNAL ] SECURITY DEFINER
                      | PARALLEL { UNSAFE | RESTRICTED | SAFE }
                      | COST int_literal
                      | ROWS int_literal
                      | SET configuration_parameter 
                        { TO value | = value | FROM CURRENT }
                      | AS 'definition'
                      | AS 'obj_file' 'link_symbol' } [ ... ]

arg_decl ::= [ argmode ] [ argname ] argtype 
             [ { DEFAULT | = } expression ]
```
