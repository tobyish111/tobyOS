# A reserved word may follow a compound command's end with no `;` -- a
# `]]`, done, fi, esac, a group's `}` -- and one after then, do or else on
# the same line is reserved too (bash's cond.tests and case.tests; findings
# 291 and 293). tsh's keyword searches took only a `;`, `&`, `|`, `}` or
# `)` before one: `if [[ x ]] then`, `done done`, `done esac` and even
# `if a; then if b; then c; fi; fi` on one line were syntax errors.
if [[ str ]] then [[ str ]] fi
echo after-dbr
if [[ -z x ]] then echo no; elif [[ y ]] then echo elif-yes; fi
i=0; while [[ $i -lt 2 ]] do i=$((i+1)); echo "w$i"; done
for a in 1 2; do for b in x; do echo "$a$b"; done done
if true; then if true; then echo nested-fi; fi fi
case k in k) for i in 1 2; do echo "c$i"; done esac
if true; then case a in a) echo esac-fi; esac fi
for i in 1; do { echo "g$i"; } done
if true; then if false; then echo in; else echo el; fi; echo after-inner; fi
if false; then echo a; else if true; then echo b; fi; fi
x=$(if true; then if true; then echo sub; fi; fi); echo "$x"
f() { if true; then if true; then echo fn; fi; fi; }; f
echo ]] then
