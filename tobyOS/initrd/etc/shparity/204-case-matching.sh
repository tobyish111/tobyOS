# case: `;&` runs the next body without testing it, `;;&` tests the next
# patterns; `shopt -s nocasematch` applies to case, [[ == ]] (quoted too)
# and ${x/pat/rep}; an escaped character in a pattern is literal. tsh knew
# only `;;`, ignored nocasematch, and dropped a pattern's backslashes --
# `*\(\)` became the extglob `*()` and `\*` matched everything.
exec 2>&1
case aa in aa) echo one ;& bb) echo two ;; cc) echo three;; esac
case aa in a*) echo x1 ;;& *a) echo x2 ;;& b*) echo x3;; esac
shopt -s nocasematch
case ABC in abc) echo ci-case;; esac
[[ ABC == abc ]] && echo ci-dbr; [[ ABC == "abc" ]] && echo ci-dbr-q
x=ABC; echo "${x/b/-}" "${x#a}"
shopt -u nocasematch
s='foo()'; case $s in *\(\)) echo esc;; esac
case abc in \*) echo star;; *) echo other;; esac
case 'a*' in a\*) echo lit;; esac
