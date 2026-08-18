# 2024-06 - tickled by Samuel testing Gentoo

if test -v SHELLOPTS; then
  echo 'shellopts is set'
fi
if test -v BASHOPTS; then
	echo 'bashopts is set'
fi

# bash: braceexpand:hashall etc.

echo shellopts ${SHELLOPTS:?} > /dev/null
echo bashopts ${BASHOPTS:?} > /dev/null
