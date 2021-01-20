```
create_procedure ::= CREATE [ OR REPLACE ] PROCEDURE name ( 
                     [ arg_decl [ , ... ] ] )  
                     { LANGUAGE lang_name
                       | TRANSFORM { FOR TYPE type_name } [ , ... ]
                       | [ EXTERNAL ] SECURITY INVOKER
                       | [ EXTERNAL ] SECURITY DEFINER
                       | SET configuration_parameter 
                         { TO value | = value | FROM CURRENT }
                       | AS 'definition'
                       | AS 'obj_file' 'link_symbol' } [ ... ]

arg_decl ::= [ argmode ] [ argname ] argtype 
             [ { DEFAULT | = } expression ]
```
