# sed exercise. Run by /bin/tsh in the guest AND by bash on the host, where
# `sed` is real GNU sed -- the two transcripts are compared section by
# section, so every expectation here is GNU's actual output rather than
# something asserted from memory.
#
# Keep every case's output on its own line and free of "@@" at the start of
# a line, which is what separates the sections.

cd "${WORKDIR:-/tmp}"

printf 'alpha\nbeta\ngamma\ndelta\nepsilon\n' > f
printf 'one two three\nfour five six\n' > g
printf 'aaa\nbbb\nab\nabab\nxyz\n' > r

echo @@s_basic;      sed 's/a/X/' f
echo @@s_global;     sed 's/a/X/g' f
echo @@s_nth;        sed 's/a/X/2' f
# NOT [&]: a line starting with '[' looks like a kernel diagnostic to
# the gate's transcript filter and gets stripped.
echo @@s_amp;        sed 's/a/<&>/g' f
echo @@s_group;      sed -E 's/(al)(pha)/\2\1/' f
echo @@s_bre_group;  sed 's/\(al\)\(pha\)/\2\1/' f
echo @@s_upper;      sed 's/.*/\U&/' f
echo @@s_ucfirst;    sed 's/^./\u&/' f
echo @@s_icase;      sed 's/ALPHA/X/I' f
echo @@s_newline;    sed 's/ /\n/g' g
echo @@s_altdelim;   sed 's|a|X|g' f
echo @@s_anchors;    sed 's/^a/START/' f
echo @@s_endanchor;  sed 's/a$/END/' f

echo @@re_star;      sed -E 's/b*/X/' r
echo @@re_plus;      sed -E 's/[a-z]+/W/' r
echo @@re_alt;       sed -E 's/alpha|beta/Z/' f
echo @@re_interval;  sed -E 's/[a-z]{3}/#/' f
echo @@re_int_bre;   sed 's/[a-z]\{3\}/#/' f
echo @@re_int_range; sed -E 's/a{2,3}/#/' r
echo @@re_int_open;  sed -E 's/a{2,}/#/' r
echo @@re_backref;   sed -E 's/(a)\1/DUP/' r
echo @@re_tab;       printf 'a\tb\n' | sed 's/\t/TAB/'

echo @@a_line;       sed -n '2p' f
echo @@a_range;      sed -n '2,4p' f
echo @@a_regex;      sed -n '/et/p' f
echo @@a_regexrange; sed -n '/beta/,/delta/p' f
echo @@a_last;       sed -n '$p' f
echo @@a_negate;     sed -n '2!p' f
echo @@a_step;       sed -n '1~2p' f
echo @@a_plus;       sed -n '2,+2p' f
echo @@a_zero;       sed -n '0,/gamma/p' f

echo @@c_delete;     sed '2d' f
echo @@c_delrange;   sed '2,4d' f
echo @@c_equals;     sed -n '=' f
echo @@c_y;          sed 'y/abc/xyz/' f
echo @@c_append;     sed '2a HELLO' f
echo @@c_insert;     sed '2i HELLO' f
echo @@c_change;     sed '2c HELLO' f
echo @@c_quit;       sed '3q' f
echo @@c_list;       sed -n '1l' f

echo @@h_hold;       sed -n '1h;$ {G;p}' f
echo @@h_exchange;   sed -n '1h;2{x;p}' f
echo @@h_tac;        sed -n '1!G;h;$p' f
echo @@h_double;     sed G f

echo @@b_block;      sed -n '/beta/{p;p}' f
echo @@b_nested;     sed -n '1,4{/a/{/l/p}}' f
echo @@b_branch;     sed -n -e '/beta/b end' -e p -e ':end' f
echo @@b_tloop;      sed ':a;s/a/b/;ta' f
echo @@b_njoin;      sed -n 'N;s/\n/+/;p' f
echo @@b_dloop;      sed -n '$!{N;D};p' f
echo @@b_pfirst;     sed -n 'N;P' f
echo @@b_slurp;      sed -n '1{:a;N;$!ba};s/\n/,/g;p' f

echo @@o_multi_e;    sed -e '1d' -e 's/a/Z/' f
echo @@o_semicolon;  sed -n '1p;3p;5p' f
echo @@o_scriptfile; printf 's/a/Z/\ns/b/Y/\n' > sc.sed; sed -f sc.sed f
echo @@o_wfile;      sed -n '2w out.w' f; cat out.w
echo @@o_rfile;      printf 'INSERTED\n' > ins.txt; sed '2r ins.txt' f
echo @@o_inplace;    cp f ip.txt; sed -i 's/a/Q/' ip.txt; cat ip.txt
echo @@o_inplacebak; cp f ib.txt; sed -i.bak 's/a/R/' ib.txt; cat ib.txt; cat ib.txt.bak
echo @@o_exitcode;   sed '2q5' f; echo rc=$?
echo @@o_nonl;       printf 'no trailing newline' > nn.txt
sed 's/no/NO/' nn.txt > nn.out
echo "bytes: $(wc -c < nn.out)"
sed -n 'p' nn.txt > nn2.out
echo "p bytes: $(wc -c < nn2.out)"

echo @@sed_END
