```
group_by_clause ::= GROUP BY { grouping_element [ , ... ] }

grouping_element ::= ( ) | ( expression [ , ... ] )
                     | ROLLUP ( expression [ , ... ] )
                     | CUBE ( expression [ , ... ] )
                     | GROUPING SETS ( grouping_element [ , ... ] )
```
