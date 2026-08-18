mkdir -p $TMP/dotglob
cd $TMP/dotglob
touch .foorc other

echo *
shopt -s dotglob
echo * | sort
