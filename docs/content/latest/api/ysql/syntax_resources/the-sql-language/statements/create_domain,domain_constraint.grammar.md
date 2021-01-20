```
create_domain ::= CREATE DOMAIN name [ AS ] data_type 
                  [ DEFAULT expression ] 
                  [ [ domain_constraint [ ... ] ] ]

domain_constraint ::= [ CONSTRAINT constraint_name ] 
                      { NOT NULL | NULL | CHECK ( expression ) }
```
