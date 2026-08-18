set -e

x=$($SH -u -c 'echo prev=$_')
echo status=$?

# bash and mksh set $_ to $0 at first; zsh is empty
#echo "$x"
