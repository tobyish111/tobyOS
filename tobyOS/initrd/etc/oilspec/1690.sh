cd $REPO_ROOT
touch _tmp/spec-tmp/a.zz _tmp/spec-tmp/b.zz
echo _tmp/spec-tmp/*.zz
set -o noglob
echo _tmp/spec-tmp/*.zz
