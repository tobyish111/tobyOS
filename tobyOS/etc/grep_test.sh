# grep exercise. Run by /bin/tsh in the guest AND by bash on the host,
# where `grep` is real GNU grep -- the two transcripts are compared section
# by section, so every expectation here is GNU's actual output.
#
# Keep every case's output on its own line and free of "@@" at the start of
# a line. A line that STARTS with '[' is invisible to the gate's transcript
# filter, so bracket things with a letter in front.
#
# The -m, -e, -f, -A/-B/-C and -s sections are new on 2026-09-06 and every
# one of them was a usage error before. The anchor cases are older than
# that and had never worked: grep matched the pattern against the buffer
# fgets() returned, newline included, so `$` sat one character short and
# every anchored pattern in the tree quietly found nothing.

cd "${WORKDIR:-/tmp}"

printf 'alpha\nbeta\ngamma\ndelta\nepsilon\nzeta\neta\ntheta\niota\n' > gwords
printf 'one 1\ntwo 22\nthree 333\nfour 4444\nfive 5\n' > gnums
printf 'cat\nconcat cat\ncats\nthe cat sat\ndog\n' > gcats
printf 'AAA\naaa\nAaA\n' > gcase
printf 'alpha\nbeta\n' > gplist
rm -rf gdir; mkdir -p gdir/sub
printf 'hit here\n' > gdir/a.txt
printf 'nothing\n' > gdir/sub/b.txt

echo @@gr_m1;       grep -m 1 'eta' gwords
echo @@gr_m2;       grep -m 2 'eta' gwords
echo @@gr_mmore;    grep -m 99 'eta' gwords
echo @@gr_m0;       grep -m 0 'eta' gwords; echo "rc=$?"
echo @@gr_mc;       grep -c -m 2 'eta' gwords
echo @@gr_mn;       grep -n -m 2 'eta' gwords
echo @@gr_mv;       grep -v -m 3 'eta' gwords

echo @@gr_etwo;     grep -e alpha -e zeta gwords
echo @@gr_eone;     grep -e alpha gwords
echo @@gr_ecount;   grep -c -e alpha -e zeta -e nomatch gwords
echo @@gr_ffile;    grep -f gplist gwords
echo @@gr_efmix;    grep -e alpha -f gplist gwords
echo @@gr_eere;     grep -E -e 'a(lpha|)' -e 'z.ta' gwords

echo @@gr_a1;       grep -A1 'gamma' gwords
echo @@gr_b1;       grep -B1 'gamma' gwords
echo @@gr_c1;       grep -C1 'gamma' gwords
echo @@gr_a2multi;  grep -A2 -e alpha -e eta gwords
echo @@gr_c1multi;  grep -C1 -e beta -e theta gwords
echo @@gr_c1n;      grep -n -C1 'gamma' gwords
echo @@gr_a1last;   grep -A1 'iota' gwords
echo @@gr_b2first;  grep -B2 'alpha' gwords
echo @@gr_c0;       grep -C0 'gamma' gwords

echo @@gr_omulti;   printf 'aXbXc\n' | grep -o 'X'
echo @@gr_ore;      printf 'a1b22c333\n' | grep -o -E '[0-9]+'
echo @@gr_oword;    printf 'foo bar foo\n' | grep -o 'foo'
echo @@gr_oempty;   printf 'abc\n' | grep -o -E 'x*'; echo "rc=$?"
echo @@gr_odigits;  printf 'v1.2.3\n' | grep -o -E '[0-9]+'
echo @@gr_on;       grep -o -n -E '[a-z]+a' gwords

echo @@gr_wcat;     grep -w cat gcats
echo @@gr_wcount;   grep -c -w cat gcats
echo @@gr_xexact;   grep -x cat gcats
echo @@gr_wlater;   printf 'xcat cat\n' | grep -w cat
echo @@gr_wnone;    printf 'xcatx\n' | grep -w cat; echo "rc=$?"

echo @@gr_plain;    grep 'eta' gwords
echo @@gr_count;    grep -c 'eta' gwords
echo @@gr_ic;       grep -i 'AAA' gcase
echo @@gr_inv;      grep -v 'eta' gwords
echo @@gr_lineno;   grep -n 'eta' gwords
echo @@gr_quiet;    grep -q 'eta' gwords; echo "rc=$?"
echo @@gr_quietno;  grep -q 'nosuch' gwords; echo "rc=$?"
echo @@gr_lfiles;   grep -l 'eta' gwords gnums
echo @@gr_Lfiles;   grep -L 'eta' gwords gnums
echo @@gr_ere;      grep -E 'a(lph|)a' gwords
echo @@gr_fixed;    grep -F 'a.b' gwords; echo "rc=$?"
echo @@gr_multi;    grep 'a' gwords gnums
echo @@gr_noname;   grep -h 'a' gwords gnums
echo @@gr_withname; grep -H 'eta' gwords
echo @@gr_recurse;  grep -r 'hit' gdir

echo @@gr_anchor;   grep '^eta$' gwords
echo @@gr_dollar;   grep '5$' gnums
echo @@gr_bre;      grep '^[a-z]*a$' gwords
echo @@gr_breplus;  grep 'a\+' gwords
echo @@gr_class;    grep -o '[[:digit:]][[:digit:]]*' gnums
echo @@gr_word_lt;  grep -o '\<cat' gcats
echo @@gr_nomatch;  grep 'zzzz' gwords; echo "rc=$?"
echo @@gr_missing;  grep -s 'x' /nonexistent/file; echo "rc=$?"

echo @@grep_END
