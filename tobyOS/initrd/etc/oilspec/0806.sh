set -o errexit
cd /
pushd /tmp >/dev/null
echo --
dirs -v
pushd /dev >/dev/null
echo --
dirs -v
#
#  zsh uses tabs
