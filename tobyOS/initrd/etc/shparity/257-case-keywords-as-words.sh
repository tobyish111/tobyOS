# `case` and `esac` are reserved only where a command may start: in
# `x) echo case;;` and `x) echo esac;;` they are echo's arguments. tsh
# counted them, nesting a case that was never opened ("case: expected
# 'esac'", status 2) or closing this one early.
# (batch 12; probe p118 in C:\t\b12)
case x in x) echo case;; esac
case x in x) echo esac;; esac
case x in x) echo in case esac;; y) echo y;; esac
case a in a) case b in b) echo nested;; esac; echo after;; esac
case a in (esac) echo pat;; a) echo a;; esac
case esac in esac) echo kw;; esac
for w in case esac; do case $w in case) echo "w=case";; esac) echo "w=esac";; esac; done
if true; then case z in z) echo z;; esac; fi
echo end
