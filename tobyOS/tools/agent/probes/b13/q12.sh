exec 2>&1
cd /tmp
( pushd / >/dev/null ); dirs
( unset DIRSTACK ); pushd / >/dev/null; echo "ds=[${DIRSTACK[*]}]"; popd >/dev/null
hash -r; ( hash cat ); hash 2>&1
hash cat; ( hash -r ); hash -t cat
( enable -n echo ); type -t echo
( complete -W 'a b' zz ); complete -p zz 2>&1
x=$(alias q=1; echo); alias q 2>&1
( shopt -s extglob ); shopt extglob
( set -o noglob ); echo "$-" | tr -d 'hB'
echo end
