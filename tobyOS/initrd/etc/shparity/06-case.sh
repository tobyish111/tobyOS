# case with glob patterns, alternation, character classes and the empty
# pattern. case patterns are globs, NOT regexes -- a shell that reaches for a
# regex engine here will match different strings.
for v in apple banana cherry 42 zz ""; do
    case "$v" in
        a*)     echo "$v: starts-a" ;;
        b*|c*)  echo "$v: b-or-c" ;;
        [0-9]*) echo "$v: numeric" ;;
        "")     echo "(empty)" ;;
        *)      echo "$v: other" ;;
    esac
done
f=script.tar.gz
case "$f" in
    *.tar.gz) echo "compound-suffix" ;;
    *.gz)     echo "wrong" ;;
esac
case abc in
    ???) echo "three-chars" ;;
esac
