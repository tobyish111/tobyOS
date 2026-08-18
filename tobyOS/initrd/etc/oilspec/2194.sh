f() ( return 42; )
# BUG: OSH raises invalid control flow!  I think we should just allow 'return'
# but maybe not 'break' etc.
g() ( return 42 )
# bash warns here but doesn't cause an error
# g() ( break )

f
echo status=$?
g
echo status=$?
