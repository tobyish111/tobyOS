unlocal() { unset -v "$1"; }

f4_unlocal() {
  v=tempenv2 eval '
    v=tempenv3 eval "
      echo \"[\$1,tempenv2,tempenv3] v: \${v-(unset)}\"
      unlocal v
      echo \"[\$1,tempenv2,tempenv3] v: \${v-(unset)} (unlocal 1)\"
      unlocal v
      echo \"[\$1,tempenv2,tempenv3] v: \${v-(unset)} (unlocal 2)\"
      unlocal v
      echo \"[\$1,tempenv2,tempenv3] v: \${v-(unset)} (unlocal 3)\"
      unlocal v
      echo \"[\$1,tempenv2,tempenv3] v: \${v-(unset)} (unlocal 4)\"
    "
  '
}
v=global
v=tempenv1 f4_unlocal global,tempenv1



# cell-unset
[global,tempenv1,tempenv2,tempenv3] v: tempenv3
[global,tempenv1,tempenv2,tempenv3] v: tempenv1 (unlocal 1)
[global,tempenv1,tempenv2,tempenv3] v: global (unlocal 2)
[global,tempenv1,tempenv2,tempenv3] v: (unset) (unlocal 3)
[global,tempenv1,tempenv2,tempenv3] v: (unset) (unlocal 4)

# remove all tempenv3
[global,tempenv1,tempenv2,tempenv3] v: tempenv3
[global,tempenv1,tempenv2,tempenv3] v: (unset) (unlocal 1)
[global,tempenv1,tempenv2,tempenv3] v: (unset) (unlocal 2)
[global,tempenv1,tempenv2,tempenv3] v: (unset) (unlocal 3)
[global,tempenv1,tempenv2,tempenv3] v: (unset) (unlocal 4)
