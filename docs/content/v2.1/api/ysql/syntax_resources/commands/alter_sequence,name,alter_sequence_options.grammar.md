```
alter_sequence ::= ALTER SEQUENCE [ IF EXISTS ] name 
                   alter_sequence_options

name ::= '<Text Literal>'

alter_sequence_options ::= [ AS seq_data_type ] 
                           [ INCREMENT [ BY ] increment ] 
                           [ MINVALUE minvalue | NO MINVALUE ] 
                           [ MAXVALUE maxvalue | NO MAXVALUE ] 
                           [ START [ WITH ] start ] 
                           [ RESTART [ [ WITH ] restart ] ] 
                           [ CACHE cache ] 
                           [ OWNED BY table_name.table_column | NONE ]
```
