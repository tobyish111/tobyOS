mkdir -p _tmp
touch _tmp/foo.gg _tmp/bar.gg _tmp/foo.hh
pat='_tmp/*.hh _tmp/*.gg'
argv.py $pat
