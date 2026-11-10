unlocal() { unset -v "$1"; }

f5() {
  echo "[$1] v: ${v-(unset)}"
  local v
  echo "[$1,local] v: ${v-(unset)}"
  ( unset v
    echo "[$1,local+unset] v: ${v-(unset)}" )
  ( unlocal v
    echo "[$1,local+unlocal] v: ${v-(unset)}" )
}
v=global
f5 'global'
v=tempenv f5 'global,tempenv'
v=tempenv eval 'f5 "global,tempenv,(eval)"'


# Note on bug in bash 4.3 to bash 5.0
# [global] v: global
# [global,local] v: (unset)
# [global,local+unset] v: (unset)
# [global,local+unlocal] v: global
# [global,tempenv] v: tempenv
# [global,tempenv,local] v: tempenv
# [global,tempenv,local+unset] v: global
# [global,tempenv,local+unlocal] v: global
# [global,tempenv,(eval)] v: tempenv
# [global,tempenv,(eval),local] v: tempenv
# [global,tempenv,(eval),local+unset] v: (unset)
# [global,tempenv,(eval),local+unlocal] v: tempenv

# always-value-unset x init.unset
[global] v: global
[global,local] v: (unset)
[global,local+unset] v: (unset)
[global,local+unlocal] v: (unset)
[global,tempenv] v: tempenv
[global,tempenv,local] v: tempenv
[global,tempenv,local+unset] v: (unset)
[global,tempenv,local+unlocal] v: (unset)
[global,tempenv,(eval)] v: tempenv
[global,tempenv,(eval),local] v: (unset)
[global,tempenv,(eval),local+unset] v: (unset)
[global,tempenv,(eval),local+unlocal] v: (unset)

# always-value-unset x init.empty
[global] v: global
[global,local] v: 
[global,local+unset] v: (unset)
[global,local+unlocal] v: (unset)
[global,tempenv] v: tempenv
[global,tempenv,local] v: 
[global,tempenv,local+unset] v: (unset)
[global,tempenv,local+unlocal] v: (unset)
[global,tempenv,(eval)] v: tempenv
[global,tempenv,(eval),local] v: 
[global,tempenv,(eval),local+unset] v: (unset)
[global,tempenv,(eval),local+unlocal] v: (unset)

# always-value-unset x init.inherit
[global] v: global
[global,local] v: global
[global,local+unset] v: (unset)
[global,local+unlocal] v: (unset)
[global,tempenv] v: tempenv
[global,tempenv,local] v: tempenv
[global,tempenv,local+unset] v: (unset)
[global,tempenv,local+unlocal] v: (unset)
[global,tempenv,(eval)] v: tempenv
[global,tempenv,(eval),local] v: tempenv
[global,tempenv,(eval),local+unset] v: (unset)
[global,tempenv,(eval),local+unlocal] v: (unset)
