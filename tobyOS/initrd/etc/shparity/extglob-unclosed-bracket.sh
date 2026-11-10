# Inside an extglob group a bracket expression runs to its `]`; one with no
# `]` leaves the group open, and the pattern is no extglob at all: `!([*)*`
# matches nothing and stays, `case '*(a|b[)' in *(a|b[))` matches -- tsh
# took the `)` in `[)` for the group's (finding 457).
exec 2>&1
shopt -s extglob
d=$(mktemp -d); cd "$d" || exit; touch abcx abcz bbc
echo !([*)*
echo +(a|b[)*
case foo in *(a|b[)) echo yes;; *) echo no;; esac
case foo in *(a|b[)|f*) echo yes;; *) echo no;; esac
case '*(a|b[)' in *(a|b[)) echo yes;; *) echo no;; esac
case 'x|y' in @([|]|x)*) echo bar-in-bracket;; *) echo no;; esac
cd /; rm -rf "$d"
