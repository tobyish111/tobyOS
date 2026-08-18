shopt -u strict_arith || true

undef[2]=x
undef[3]=y
x='bad'
# bad gets coerced to zero, but this is part of the RECURSIVE arithmetic
# behavior, which we want to disallow.  Consider disallowing in OSH.

undef[$x]=zzz
argv.py "${undef[@]}"
