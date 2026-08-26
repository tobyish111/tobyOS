# POSIX 2.9.4.3 -- the case construct.
# Patterns are globs, alternation is `|`, the first match wins and nothing
# falls through. An optional `(` may precede the pattern.
for v in apple banana 42 "" "a b" "*" "x|y" ab; do
    case "$v" in
        apple)      echo "1:exact" ;;
        b*)         echo "2:prefix" ;;
        [0-9]*)     echo "3:digit" ;;
        "")         echo "4:empty" ;;
        "a b")      echo "5:spaced" ;;
        "*")        echo "6:star-literal" ;;
        "x|y")      echo "7:pipe-literal" ;;
        a?)         echo "8:two-char" ;;
        *)          echo "9:default" ;;
    esac
done

case abc in
    (abc) echo "10:paren-form" ;;
esac

case xyz in
    a|b|xyz|c) echo "11:alternation" ;;
    *)         echo "11:miss" ;;
esac

case first in
    first) echo "12:first-wins" ;;
    fir*)  echo "12:should-not-reach" ;;
esac

v=nested
case $v in
    nested)
        case inner in
            inner) echo "13:nested-case" ;;
        esac
        ;;
esac

case novalue in
    x) echo "14:no" ;;
esac
echo "15=$?"

case multi in
    multi) echo "16a"; echo "16b" ;;
esac
w=abc
case "$w" in
    a*c) echo "17:middle-glob" ;;
esac
echo end
