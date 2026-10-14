# `help PATTERN...`: a name, a glob, or else a prefix; the "Shell commands
# matching keyword" heading for a glob first word; "no help topics match"
# only when nothing matched at all, naming the last pattern; the options.
# -m's IMPLEMENTATION names the version and MACHTYPE, tsh's own: left out.
h() {
  echo "== help $*"
  out=$(help "$@" 2>&1); rc=$?
  printf '%s\n' "$out" | while IFS= read -r l; do
    case $l in *'GNU bash, version'*) ;; *) printf '%s\n' "$l" ;; esac
  done
  echo "rc=$rc"
}
h -s ec
h -s e
h -s 'e*'
h -s 'c?'
h -s '[ab]*'
h -s 'e*' 'p*'
h -s ec 'p*'
h -s for
h -s 'for*'
h -s '{'
h -s '(('
h -s '[['
h -s '%'
h -s '*]'
h -s 'a\*'
h -s '+(x)'
h -s 'zz*'
h zz
h zz echo
h echo zz
h zz yy
h -x
h -sx echo
h -- -s
h -- echo
h -
h -dm echo
h -ms echo
h -sd echo
h -d -s -m true
h --help
h -s --help
h -s -- --help
shopt -s extglob
h -s '@(echo|true)'
h -s '!(*e*)'
shopt -u extglob
h -s '@(echo|true)'
