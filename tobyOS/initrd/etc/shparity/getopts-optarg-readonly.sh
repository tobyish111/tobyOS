# getopts' status is NAME's: 2 when it cannot set NAME, and an OPTARG it
# cannot set changes nothing. At the end of the options, and with a message
# for a bad option or a missing argument, OPTARG is taken away -- a
# readonly one too, quietly; after a plain option it is declared with no
# value (finding 318). tsh gave 1, and its unset said "cannot unset".
{
( set -- -a bb; readonly OPTARG; getopts :x x; echo "rc=$? [$OPTARG] x=$x"
  getopts x x; echo "rc=$? ${OPTARG-unset} x=$x"; typeset -n OPTARG=RO; echo "rc=$?" )
( readonly o=keep; getopts ab o -a; echo "rc=$? o=$o $OPTIND"; OPTIND=1
  getopts ab o x; echo "rc=$? o=$o"; OPTIND=1; getopts :b: o -b; echo "rc=$? o=$o $OPTARG" )
( set -- -a bb; typeset -r RO=foo; typeset -n OPTARG=RO
  getopts :x x; echo "rc=$?"; declare -p RO; getopts x x; echo "rc=$?"; declare -p RO )
( readonly OPTARG; getopts b: o -bx; echo "rc=$? o=$o"; OPTIND=1; getopts :b: o -b; echo "rc=$? o=$o"
  OPTIND=1; getopts b: o -b; echo "rc=$? o=$o" )
( readonly OPTARG=keep; getopts a o x; echo "rc=$? [${OPTARG-unset}]"; OPTARG=new; echo "[$OPTARG]" )
( readonly OPTARG=keep; getopts a o -a; echo "rc=$? [${OPTARG-unset}]"; readonly -p | grep -c OPTARG )
( readonly OPTARG=keep; getopts a o -z; echo "rc=$? [${OPTARG-unset}]"; readonly -p | grep -c OPTARG )
( readonly OPTARG=keep; getopts b: o -b; echo "rc=$? [${OPTARG-unset}]"; readonly -p | grep -c OPTARG )
( OPTARG=zz; getopts a o -a; echo "rc=$? ${OPTARG-unset}"; declare -p OPTARG )
( T=1; typeset -n OPTARG=T; getopts a o -a; echo "rc=$?"; declare -p OPTARG T )
( readonly R=1; typeset -n OPTARG=R; getopts a o -a; echo "rc=$?"; declare -p OPTARG R )
( T=1; typeset -n OPTARG=T; getopts a o x; echo "rc=$?"; declare -p T; declare -p OPTARG )
( OPTARG=(a b); getopts a o x; declare -p OPTARG )
} 2>&1
