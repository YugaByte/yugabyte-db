```
frame_clause ::= [ { RANGE | ROWS | GROUPS } frame_bounds ] 
                 [ frame_exclusion ]

frame_bounds ::= frame_start | BETWEEN frame_start AND frame_end

frame_start ::= frame_bound

frame_end ::= frame_bound

frame_bound ::= UNBOUNDED PRECEDING
                | offset PRECEDING
                | CURRENT ROW
                | offset FOLLOWING
                | UNBOUNDED FOLLOWING

frame_exclusion ::= EXCLUDE CURRENT ROW
                    | EXCLUDE GROUP
                    | EXCLUDE TIES
                    | EXCLUDE NO OTHERS
```
