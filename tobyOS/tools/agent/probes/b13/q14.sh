S=${SH:-bash}; S=$(command -v $S)
echo "== set"; env -i $S -c 'set' | grep -v '^BASH_VERSINFO\|^BASH_VERSION\|^BASHOPTS\|^SHELLOPTS\|^PPID\|^BASH_ARGV0\|^BASH_EXECUTION_STRING\|^_='
echo "== login"; env -i $S -l -c 'echo "HOME=[${HOME-unset}] SHELL=[$SHELL]"; export -p'
echo "== invalid"; env -i 'A-B=1' 'C.D=2' E=3 $S -c 'env; echo "[${E}]"'
echo "== pwd"; env -i PWD=/nonexist $S -c 'echo "$PWD"; env'
echo "== oldpwd"; env -i OLDPWD=/bin $S -c 'echo "[$OLDPWD]"; env'
echo end
