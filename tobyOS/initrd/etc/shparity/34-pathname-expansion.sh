# POSIX 2.6.6 + 2.13 -- pathname expansion and pattern matching notation.
# Results are sorted; a pattern that matches nothing is left alone; a leading
# dot is never matched by a wildcard unless the pattern has one too.
count() { echo "n=$#"; }
> b.txt
> a.txt
> ab.txt
> c.log
> .hidden

echo *.txt
echo *.log
echo ?.txt
echo ??.txt
echo [ab].txt
echo [!a]*.txt
echo [a-b].txt
echo *
echo *.none
echo "*.txt"
count *.txt
count *
count *.none
count .*

for f in *.txt; do echo "for:$f"; done
for f in *; do echo "all:$f"; done
case a.txt in *.txt) echo "case-star" ;; esac
case a.txt in ?.txt) echo "case-quest" ;; esac
case a.txt in [ab].txt) echo "case-class" ;; esac
case c.log in *.txt) echo no ;; *) echo "case-default" ;; esac
echo end
