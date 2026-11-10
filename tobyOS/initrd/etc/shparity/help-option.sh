# `NAME --help` for every builtin: bash's builtin_help and status 2 where
# the builtin's option reading meets `--help` -- the first word only for
# some, any option word for the rest, never after `--`, an operand or as an
# option's argument. echo, test, true, false and : have none; bind's is its
# usage line; local's is caught before the function test.
b() { echo "== $*"; ( "$@" ) 2>&1 < /dev/null; echo "rc=$?"; }
for n in . : [ alias bg break builtin caller cd command compgen complete \
  compopt continue declare dirs disown echo enable eval exec exit export \
  false fc fg getopts hash help history jobs kill let local logout mapfile \
  popd printf pushd pwd read readarray readonly return set shift shopt \
  source suspend test times trap true type typeset ulimit umask unalias \
  unset wait; do
  b "$n" --help
done
b cd -L --help
b cd -- --help
b cd /tmp --help
b cd -x --help
b read -r --help
b read -d --help
b read -rd , --help
b printf -v --help
b printf -v v --help
b set -e --help
b set -o --help
b set -- --help
b set +x --help
b declare -a --help
b declare +x --help
b typeset -i --help
b export -n --help
b readonly -p --help
b kill -s TERM --help
b kill -l --help
b exit 3 --help
b return 3 --help
b shift --help 1
b let 1 --help
b ulimit -c --help
b ulimit -t 5 --help
b ulimit -b --help
b type -t --help
b type --help --help
b hash -p --help
b jobs -l --help
b wait -n --help
b umask -S --help
b trap -l --help
b unset -v --help
b shopt -s --help
b history -d --help
b mapfile -t --help
b compgen -W x --help
b complete -p --help
b compopt -o --help
b enable -n --help
b exec -a --help
b alias -p --help
b getopts --help x
b getopts a --help
b local -a --help
f() { local --help; echo "rc=$?"; local -i --help; echo "rc=$?"; local x --help; echo "rc=$?"; }
f 2>&1
builtin cd --help; echo "rc=$?"
command pwd --help; echo "rc=$?"
builtin printf --help; echo "rc=$?"
