# XCU 2.8.1 + 2.6.2: the ${var?word} failing form EXITS a non-interactive
# shell, so each probe runs in a SUBSHELL and the parent reads the status.
# Messages go to stderr, which the gate does not compare.
u98=
( echo "${u98:?empty and told you}" )
echo "1=$?"
( echo "${never_set_98?}" )
echo "2=$?"
( echo "${u98:?}"; echo not-reached )
echo "3=$?"
echo "${u98:-fallback}"
echo "${u98=kept-empty}[$u98]"
echo "${never_set_98=assigned}[$never_set_98]"
echo end
