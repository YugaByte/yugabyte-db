```
create_rule ::= CREATE [ OR REPLACE ] RULE rule_name AS ON rule_event 
                TO table_name [ WHERE condition ] DO 
                [ ALSO | INSTEAD ] { NOTHING
                                     | command
                                     | ( command [ ; ... ] ) }

rule_event ::= SELECT | INSERT | UPDATE | DELETE

command ::= SELECT | INSERT | UPDATE | DELETE | NOTIFY
```
