# dash, ash and zsh do not implement read -N
# mksh treats -N exactly the same as -n
case $SH in dash|ash|zsh) exit ;; esac

# bash docs: https://www.gnu.org/software/bash/manual/html_node/Bash-Builtins.html

echo 'a b c' > $TMP/readn.txt

echo 'read -n'
read -n 5 A B C < $TMP/readn.txt; echo "'$A' '$B' '$C'"
read -n 4 A B C < $TMP/readn.txt; echo "'$A' '$B' '$C'"
echo

echo 'read -N'
read -N 5 A B C < $TMP/readn.txt; echo "'$A' '$B' '$C'"
read -N 4 A B C < $TMP/readn.txt; echo "'$A' '$B' '$C'"
