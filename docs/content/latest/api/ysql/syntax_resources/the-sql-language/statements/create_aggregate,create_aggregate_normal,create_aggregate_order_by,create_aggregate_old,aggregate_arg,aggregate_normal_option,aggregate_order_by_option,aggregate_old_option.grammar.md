```
create_aggregate ::= create_aggregate_normal
                     | create_aggregate_order_by
                     | create_aggregate_old

create_aggregate_normal ::= CREATE AGGREGATE aggregate_name ( 
                            { aggregate_arg [ , ... ] | * } ) ( SFUNC 
                            = sfunc , STYPE = state_data_type 
                            [ , aggregate_normal_option [ ... ] ] )

create_aggregate_order_by ::= CREATE AGGREGATE aggregate_name ( 
                              [ aggregate_arg [ , ... ] ] ORDER BY 
                              aggregate_arg [ , ... ] ) ( SFUNC = 
                              sfunc , STYPE = state_data_type 
                              [ , aggregate_order_by_option [ ... ] ] 
                              )

create_aggregate_old ::= CREATE AGGREGATE aggregate_name ( BASETYPE = 
                         base_type , SFUNC = sfunc , STYPE = 
                         state_data_type 
                         [ , aggregate_old_option [ ... ] ] )

aggregate_arg ::= [ aggregate_argmode ] [ argname ] argtype

aggregate_normal_option ::= SSPACE = state_data_size
                            | FINALFUNC = ffunc
                            | FINALFUNC_EXTRA
                            | FINALFUNC_MODIFY = 
                              { READ_ONLY | SHAREABLE | READ_WRITE }
                            | COMBINEFUNC = combinefunc
                            | SERIALFUNC = serialfunc
                            | DESERIALFUNC = deserialfunc
                            | INITCOND = initial_condition
                            | MSFUNC = msfunc
                            | MINVFUNC = minvfunc
                            | MSTYPE = mstate_data_type
                            | MSSPACE = mstate_data_size
                            | MFINALFUNC = mffunc
                            | MFINALFUNC_EXTRA
                            | MFINALFUNC_MODIFY = 
                              { READ_ONLY | SHAREABLE | READ_WRITE }
                            | MINITCOND = minitial_condition
                            | SORTOP = sort_operator
                            | PARALLEL = 
                              { SAFE | RESTRICTED | UNSAFE }

aggregate_order_by_option ::= SSPACE = state_data_size
                              | FINALFUNC = ffunc
                              | FINALFUNC_EXTRA
                              | FINALFUNC_MODIFY = 
                                { READ_ONLY | SHAREABLE | READ_WRITE }
                              | INITCOND = initial_condition
                              | PARALLEL = 
                                { SAFE | RESTRICTED | UNSAFE }
                              | HYPOTHETICAL

aggregate_old_option ::= SSPACE = state_data_size
                         | FINALFUNC = ffunc
                         | FINALFUNC_EXTRA
                         | FINALFUNC_MODIFY = 
                           { READ_ONLY | SHAREABLE | READ_WRITE }
                         | COMBINEFUNC = combinefunc
                         | SERIALFUNC = serialfunc
                         | DESERIALFUNC = deserialfunc
                         | INITCOND = initial_condition
                         | MSFUNC = msfunc
                         | MINVFUNC = minvfunc
                         | MSTYPE = mstate_data_type
                         | MSSPACE = mstate_data_size
                         | MFINALFUNC = mffunc
                         | MFINALFUNC_EXTRA
                         | MFINALFUNC_MODIFY = 
                           { READ_ONLY | SHAREABLE | READ_WRITE }
                         | MINITCOND = minitial_condition
                         | SORTOP = sort_operator
```
