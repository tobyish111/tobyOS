case $SH in zsh|ash) exit ;; esac

a=(0 1 2)
b=(3 4 5)

#declare -p a b

HOME=/home/spec-test

# empty string, and tilde sub
a[0 + 1]=  b[2 + 0]=~/src

typeset -p a b

echo ---

# In bash, this bad prefix binding prints an error, but nothing fails
a[0 + 1]='foo' argv.py b[2 + 0]='bar'
echo status=$?

typeset -p a b
