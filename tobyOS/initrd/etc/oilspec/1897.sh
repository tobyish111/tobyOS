touch {foo,bar}.txt

shopt -s parse_ysh_expr_sub
echo $["*.txt"]
