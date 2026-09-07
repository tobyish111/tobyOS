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

# ---- 2026-09-06: the regex engine's fragment-exit rewrite ----------------
# EVERY case below produced the wrong answer before it, because a group
# whose last element was quantified had its loop edge overwritten by the
# group-end pointer. They are here because they FAILED, not because they
# looked interesting.

printf 'default via 192.168.68.1 dev eth0 proto dhcp\n' > route
printf 'aaab\nabc\nxyz\n' > gq
printf 'ab1cd\nA1B2\n' > cls
printf 'a+b\na?b\na)b\na|b\n' > lit
printf 'cat concat cats\n' > w

echo @@rx_route;     sed -n 's/^default via \([0-9.]*\) .*/\1/p' route
echo @@rx_grp_star;  sed 's/\(a*\)b/<\1>/' gq
echo @@rx_grp_plus;  sed -E 's/(a+)b/<\1>/' gq
echo @@rx_grp_ere;   sed -E 's/^([a-z]*)([0-9]*)/<\1><\2>/' cls
echo @@rx_grp_bre;   sed 's/^\([a-z]*\)/<\1>/' cls
echo @@rx_grp_rep;   sed -E 's/(ab)*/</' gq
echo @@rx_grp_intv;  sed -E 's/([0-9]{1,3})/<\1>/' route

echo @@rx_cls_alpha; sed 's/[[:alpha:]]*/</' cls
echo @@rx_cls_digit; sed 's/[[:digit:]]/D/g' cls
echo @@rx_cls_upper; sed 's/[[:upper:]]/U/g' cls
echo @@rx_cls_neg;   sed 's/[^[:digit:]]/./g' cls
echo @@rx_cls_punct; sed 's/[[:punct:]]/P/g' lit

echo @@rx_lit_plus;  sed 's/a+/X/' lit
echo @@rx_lit_quest; sed 's/a?/X/' lit
echo @@rx_lit_paren; sed 's/)/X/' lit
echo @@rx_lit_pipe;  sed 's/|/X/' lit
echo @@rx_gnu_plus;  sed 's/a\+/X/' gq
echo @@rx_gnu_quest; sed 's/xa\?b/X/' gq

echo @@rx_word_lt;   sed 's/\<cat/X/g' w
echo @@rx_word_gt;   sed 's/cat\>/X/g' w
echo @@rx_word_b;    sed 's/\bcat\b/X/g' w

echo @@rx_longest;   sed -E 's/a|ab/</' gq
echo @@rx_alt_grp;   sed -E 's/(a|ab)(c|bc)/<\1|\2>/' gq

# NOT `$(seq 1 3000)`: that substitution is ~14 KB and tsh caps command
# substitution output, so the guest printed a truncation warning ahead of
# the answer. Doubling a variable needs no subshell at all.
LONGA=aaaaaaaaaa
for i in 1 2 3 4 5 6 7 8 9; do LONGA=$LONGA$LONGA; done
printf '%sX\n' "$LONGA" > big
echo @@rx_long_line; sed 's/.*X/OK/' big
echo @@rx_long_star;  sed 's/a*X/OK/' big

echo @@rx_backref;   sed -E 's/(a)\1/D/' gq
echo @@rx_nested;    sed -E 's/((a)*b)/<\1>/' gq

echo @@sed_END
