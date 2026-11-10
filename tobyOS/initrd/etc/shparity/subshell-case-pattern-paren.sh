# Inside a ( ) subshell a case pattern's `)` closes the pattern, not the
# subshell, so a case written over lines there runs -- tsh said "subshell:
# expected ')'" (finding 443).
exec 2>&1
( case foo in foo ) echo a1;; esac
)
( case foo in
  ( $(echo foo | cat )) echo a2;;
  *) echo bad;;
esac
  echo $( echo inside ) outside )
( case foo in foo ) echo a3;; esac; echo x3; )
