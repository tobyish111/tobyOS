# https://lobste.rs/s/lplim1/design_self_compiling_c_transpiler#c_km2ywc

[ $((-42)) -le 0 ]
echo status=$?

[ $((-1)) -le 0 ]
echo status=$?

echo

[ -1 -le 0 ]
echo status=$?

[ -42 -le 0 ]
echo status=$?

echo

test -1 -le 0
echo status=$?

test -42 -le 0
echo status=$?
