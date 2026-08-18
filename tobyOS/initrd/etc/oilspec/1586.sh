shopt -s extglob
mkdir -p extglob2
touch extglob2/{foo,bar}.cc extglob2/{foo,bar,baz}.h \
      extglob2/{foo,bar,baz}.py
echo extglob2/!(*.h|*.cc)
