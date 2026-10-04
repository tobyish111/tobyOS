exec 2>&1 </dev/null
cd /tmp
t() { "$@" >/dev/null 2>/tmp/q54.err; r=$?; sed 's/^[^ ]*: line [0-9]*: //' /tmp/q54.err | head -2; echo "[$*] -> $r"; }
t alias -p
t unalias -a
t bind -l
t bind -v
t bind -p
t bind -V
t bind -P
t bind -s
t bind -S
t bind -X
t bind -q forward-char
t bind -u forward-char
t bind -m emacs -r '\C-a'
t bind -x '"\C-t": true'
t cd -e /tmp
t cd -L /tmp
t cd -P /tmp
t cd -@ /tmp
t command -p true
t command -v true
t command -V true
t complete -p
t complete -D -o default
t complete -E -W 'a b'
t complete -I -W 'a b'
t compgen -A builtin ech
t compgen -A keyword fo
t compgen -A setopt err
t compgen -A shopt ext
t compgen -A signal SIGT
t compgen -A variable HOM
t compgen -A export PAT
t compgen -A function x
t compgen -A hostname local
t compgen -A helptopic ech
t compgen -A directory /tm
t compgen -A file /tmp/q54.e
t compgen -A arrayvar BASH_VERS
t compgen -A binding forward-c
t compgen -A enabled ech
t compgen -A disabled ech
t compgen -A group roo
t compgen -A user roo
t compgen -A service ss
t compgen -A job x
t compgen -A running x
t compgen -A stopped x
t compgen -A alias x
t compopt -o nospace
t enable -a
t enable -d nope
t enable -p
t enable -s
t fc -l
t fc -ln -1
t fc -lr
t hash -d nope
t hash -l
t hash -p /bin/true tru
t hash -r
t hash -t tru
t help -d echo
t help -m echo
t help -s echo
t history -c
t history -a
t history -n
t history -r /dev/null
t history -w /dev/null
t history -s foo
t history -p '!!'
t history -d 1
t mapfile -d , -u 0 -n 1 -O 0 -t -C : -c 1 -s 0 arr
t printf -v var x
t read -r -s -a arr -d x -i y -n 1 -p p -t 0 -u 0
t read -N 1 x
t shopt -p extglob
t shopt -s -o errexit
t shopt -u -o errexit
t shopt -q extglob
t trap -l
t trap -p
t type -a true
t type -f true
t type -p true
t type -t true
t type -P true
t umask -S
t umask -p
t wait -f
t wait -n
t wait -p v
t declare -aAfFgiIlnrtux
t declare -p HOME
t typeset -i n=1
t export -fnp
t readonly -aAfp
t ulimit -a
t ulimit -S -n
t ulimit -H -n
t ulimit -b
t ulimit -c
t ulimit -d
t ulimit -e
t ulimit -f
t ulimit -i
t ulimit -k
t ulimit -l
t ulimit -m
t ulimit -p
t ulimit -q
t ulimit -r
t ulimit -s
t ulimit -t
t ulimit -u
t ulimit -v
t ulimit -x
t ulimit -P
t ulimit -R
t ulimit -T
t kill -l
t kill -L
t kill -s 0 $$
t kill -n 0 $$
t echo -neE x
t dirs -c
t dirs -l
t dirs -p
t dirs -v
t pushd -n /tmp
t popd -n
t caller
t times
t let x=1
t getopts ab: o -a
t source /dev/null
t . /dev/null
t logout
t suspend
t suspend x
t set -o
t set +o
t builtin true
t test -v HOME
t '[' -R HOME ']'
(exec -c true); echo "exec -c $?"
(exec -l true); echo "exec -l $?"
(exec -a name true); echo "exec -a $?"
echo end
