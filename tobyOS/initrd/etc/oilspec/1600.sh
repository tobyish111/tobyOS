shopt -s ysh:all
shopt -s extglob
mkdir -p eg12
cd eg12
touch {foo,bar,spam}.py
builtin write -- x@(fo*|bar).py
builtin write -- @(fo*|bar).py
