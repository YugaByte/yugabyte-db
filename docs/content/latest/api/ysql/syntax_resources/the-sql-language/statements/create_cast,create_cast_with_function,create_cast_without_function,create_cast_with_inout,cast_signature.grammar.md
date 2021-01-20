```
create_cast ::= create_cast_with_function
                | create_cast_without_function
                | create_cast_with_inout

create_cast_with_function ::= CREATE CAST ( cast_signature ) WITH 
                              FUNCTION function_name 
                              [ ( function_signature ) ] 
                              [ AS ASSIGNMENT | AS IMPLICIT ]

create_cast_without_function ::= CREATE CAST ( cast_signature ) 
                                 WITHOUT FUNCTION 
                                 [ AS ASSIGNMENT | AS IMPLICIT ]

create_cast_with_inout ::= CREATE CAST ( cast_signature ) WITH INOUT 
                           [ AS ASSIGNMENT | AS IMPLICIT ]

cast_signature ::= source_type AS target_type
```
