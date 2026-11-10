shopt -s extglob
x=foo.py
echo 'strip % ' ${x%.@(py|cc)}
echo 'strip %%' ${x%%.@(py|cc)}
echo 'strip # ' ${x#@(foo)}
echo 'strip ##' ${x##@(foo)}
