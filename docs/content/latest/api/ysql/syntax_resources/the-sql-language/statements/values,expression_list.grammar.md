```
values ::= VALUES ( expression_list ) [ ,(expression_list ... ]  
           [ ORDER BY { order_expr [ , ... ] } ]  
           [ LIMIT { integer | ALL } ]  
           [ OFFSET integer [ ROW | ROWS ] ]  
           [ FETCH { FIRST | NEXT } integer { ROW | ROWS } ONLY ]

expression_list ::= expression [ , ... ]
```
