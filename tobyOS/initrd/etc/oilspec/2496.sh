# https://oilshell.zulipchat.com/#narrow/stream/121540-oil-discuss/topic/.24.7Barr.5B.40.5D.3A.3A.7D.20in.20bash.20-.20is.20it.20documented.3F

array=(1 2 3)
argv.py ${array[@]::}
argv.py ${array[@]:0:}

echo

set -- 1 2 3
argv.py ${@::}
argv.py ${@:0:}
