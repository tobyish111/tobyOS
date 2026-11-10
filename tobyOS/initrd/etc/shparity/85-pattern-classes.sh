# XCU 2.13.1 character classes: [[:alpha:]] and friends must work in case
# patterns and in pathname expansion. Globs run inside a subdirectory this
# case creates, so leftovers from earlier cases cannot enter the result.
case b in [[:alpha:]]) echo "1=alpha";; *) echo "1=no";; esac
case 7 in [[:alpha:]]) echo "2=alpha";; [[:digit:]]) echo "2=digit";; esac
case R in [[:upper:]]) echo "3=upper";; *) echo "3=no";; esac
case _ in [[:alnum:]]) echo "4=alnum";; *) echo "4=other";; esac
case q in [![:digit:]]) echo "5=nondigit";; *) echo "5=no";; esac
case 5 in [a[:digit:]z]) echo "6=mixed";; *) echo "6=no";; esac
case + in [[:alpha:]]) echo "7=alpha";; *) echo "7=other";; esac
mkdir -p pat85 && cd pat85 || exit 9
: > a1; : > b2; : > C3; : > 9z
echo "8=" [[:alpha:]]*
echo "9=" [[:digit:]]*
echo "10=" [![:alpha:]]*
echo "11=" *[[:digit:]]
echo end
