# An indirection through an unset positional parameter is an unset value --
# ${!9:-d} is d, ${!9} nothing -- and the messages of an indirect expansion
# name the reference, "!NAME": "!5: gone", "!r: parameter null or not set",
# "!foo: unbound variable" (finding 402).
exec 2>&1
set -- a; echo "[${!9:-dflt}] [${!9-d2}] [${!9}] [${!9+alt}] [${!7@Q}] [${!7#x}]"
echo "[${!3:=v}]"; echo "not here"
echo "rc=$?"
( echo "[${!5?gone}]"; echo "not here" ); echo "rc=$?"
( r=x; unset x; echo "${!r?msg}" ); echo "rc=$?"
( r=x; x=; echo "${!r:?}" ); echo "rc=$?"
( set -u; foo=bar; echo "${!foo}" ); echo "rc=$?"
( set -u; set -- a; echo "${!4}" ); echo "rc=$?"
( set -u; r='a[3]'; a=(1); echo "${!r}" ); echo "rc=$?"
( set -u; declare r; echo "${!r}" ); echo "rc=$?"
( set -u; set -- a; echo "${!4#x}" ); echo "rc=$?"
r=x; unset x; echo "[${!r-${x-inner}}]"
