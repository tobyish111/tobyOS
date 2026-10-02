# Pattern substitution, substrings and ${@OP}, as bash 5.2 has them:
#  - a pattern that begins with `/` keeps it (${x////-} replaces every /);
#  - a tilde starts the pattern and the replacement as a word;
#  - substring offsets are plain arithmetic, so negative ones count back;
#    an empty slice length is 0;
#  - ${@%a}, ${*/a/b}, ${@^} apply to EACH positional parameter.
exec 2>&1
x=a/b/c; echo "${x////-}" "${x///}" "${x//\//+}"
HOME=/h; p=/h/f; echo "${p//~/H}"; y=abc; echo "${y/b/~}" "${y/#~/T}"
foo=abcdefgh; echo "${foo: -4:3}" "${foo:3:-2}" "${foo: -3}" "${foo:1:0}"
a=(1 2 3); echo "[${a[@]::}]" "[${a[@]:0:}]"
set -- 1a 2a 3a
printf '<%s>' ${@%a}; echo
printf '<%s>' "${*/a/b}"; echo
printf '<%s>' "${@^}"; echo
printf '<%s>' "${@#?}"; echo
