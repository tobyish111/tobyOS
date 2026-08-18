# This case popped into my mind after inspecting osh/word_eval.py for calls to
# _EvalWordToParts()

shopt -s extglob

mkdir -p eg8
cd eg8
touch {foo,bar,spam}.py

# regular glob
echo ${undef:-*.py}

# extended glob
echo ${undef:-@(foo|bar).py}
