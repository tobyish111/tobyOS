# stable temp dir
dir=/tmp/oils-spec/builtin-dirs

mkdir -p $dir
cd $dir

pushd /tmp >/dev/null
echo pushd=$?

dirs

cd /
echo cd=$?

dirs

popd >/dev/null
echo popd=$?

popd >/dev/null
echo popd=$?
