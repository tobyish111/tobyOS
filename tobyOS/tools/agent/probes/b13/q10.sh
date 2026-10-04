exec 2>&1
alias q=1 r='echo r'
set | grep '^BASH_ALIASES'
echo "n=${#BASH_ALIASES[@]}"
set | grep '^BASH_ALIASES'
declare -p | grep 'BASH_ALIASES'
alias s=2
set | grep '^BASH_ALIASES'
BASH_ALIASES+=([x]='echo x' [y]='echo y'); alias x y
BASH_ALIASES['bad name']=v; echo "rc=$?"
BASH_ALIASES[z/z]=v; echo "rc=$?"
hash -r
BASH_CMDS[nosuch]=/no/such; nosuch; echo "rc=$?"
hash -t nosuch; echo "rc=$?"
BASH_CMDS[ab]=relative/x; hash -t ab
BASH_CMDS=([e]=/bin/echo); hash -t e 2>&1; echo "rc=$? n=${#BASH_CMDS[@]}"
for k in "${!BASH_ALIASES[@]}"; do printf '%s=%s;' "$k" "${BASH_ALIASES[$k]}"; done; echo
f() { local BASH_ALIASES; BASH_ALIASES[w]=1; alias w 2>&1; declare -p BASH_ALIASES; }; f
declare -p BASH_ALIASES | cut -c1-40
readonly BASH_CMDS 2>&1; BASH_CMDS[t]=/bin/true; echo "rc=$?"; hash -t t
unalias -a; echo "n=${#BASH_ALIASES[@]}"; declare -p BASH_ALIASES
for i in $(seq 1 150); do alias "a$i=v$i"; done
echo "${!BASH_ALIASES[@]}"
echo end
