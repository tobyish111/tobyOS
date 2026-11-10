# GAP 7: quoted case patterns, and the empty pattern in particular.
# `"")` is the idiomatic empty-string arm and it never matched: the pattern
# was compared as the two literal characters `""`. Quotes in a pattern
# suppress globbing of what they enclose; they are not part of the text.
for v in "" "a b" abc 42 "x|y" "*"; do
    case "$v" in
        "")     echo "empty" ;;
        "a b")  echo "spaced" ;;
        [0-9]*) echo "num" ;;
        "x|y")  echo "pipe-literal" ;;
        "*")    echo "star-literal" ;;
        a*)     echo "starts-a" ;;
        *)      echo "other" ;;
    esac
done

case abc in
    'abc') echo "sq-match" ;;
    *)     echo "sq-miss" ;;
esac

case "" in
    ""|none) echo "alt-empty" ;;
    *)       echo "alt-miss" ;;
esac

case none in
    ""|none) echo "alt-second" ;;
esac

# An unquoted * still globs the way a pattern should.
case anything in
    *) echo "wildcard" ;;
esac
echo casequote-done
