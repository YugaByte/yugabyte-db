```
create_database ::= CREATE DATABASE name [ create_database_options ]

create_database_options ::= [ WITH ] [ OWNER [ = ] user_name ]  
                            [ TEMPLATE [ = ] template ]  
                            [ ENCODING [ = ] encoding ]  
                            [ LC_COLLATE [ = ] lc_collate ]  
                            [ LC_CTYPE [ = ] lc_ctype ]  
                            [ ALLOW_CONNECTIONS [ = ] allowconn ]  
                            [ CONNECTION LIMIT [ = ] connlimit ]  
                            [ IS_TEMPLATE [ = ] istemplate ]  
                            [ COLOCATED [ = ] { 'true' | 'false' } ]
```
