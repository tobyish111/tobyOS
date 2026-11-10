# The case keyword `in` is the one after the word: `case in in in) ...`
# matches the word in, and a word holding blanks inside $( ) or quotes is
# one word -- tsh took the first `in` for the keyword (finding 442).
exec 2>&1
case in in in) echo yes;; esac
case in in (in) echo yes2;; esac
case $(echo a b) in "a b") echo ab;; esac
case "$(echo " in ")" in " in ") echo inword;; esac
set -x; case in in in) : ;; esac; set +x
