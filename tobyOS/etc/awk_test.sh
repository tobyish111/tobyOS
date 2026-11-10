# awk exercise. Run by /bin/tsh in the guest AND by bash on the host, where
# `awk` is real GNU awk -- the two transcripts are compared section by
# section, so every expectation here is GNU's actual output rather than
# something asserted from memory.
#
# Keep every case's output on its own line and free of "@@" at the start of
# a line, which is what separates the sections. A line that STARTS with '['
# is invisible to the gate's transcript filter (it looks like a kernel
# diagnostic), so bracket anything with a letter in front.
#
# Almost everything here failed outright before 2026-09-06: the previous
# awk was a 313-line pattern matcher with no expression grammar, so $NF,
# ?:, sub(), gsub(), split(), arrays, user functions and even a plain
# `$1 == "x"` pattern either errored or silently produced the wrong text.

cd "${WORKDIR:-/tmp}"

printf 'alpha beta gamma\ndelta epsilon zeta\neta theta iota\n' > a3
printf '3 apples\n1 pear\n2 figs\n' > aitems
printf 'name:uid:shell\nroot:0:/bin/sh\ntoby:1000:/bin/tsh\n' > apw
printf 'a,b,c\nd,e,f\n' > acsv
printf 'one\n\ntwo\nthree\n\nfour\n' > apara

echo @@aw_nf;        awk '{print $NF}' a3
echo @@aw_nfminus;   awk '{print $(NF-1)}' a3
echo @@aw_ternary;   awk '{print (NR%2 ? "odd" : "even")}' a3
echo @@aw_sub;       awk '{sub(/a/,"X"); print}' a3
echo @@aw_gsub;      awk '{n=gsub(/a/,"X"); print n, $0}' a3
echo @@aw_pateq;     awk '$1=="delta"' a3
echo @@aw_pateq2;    awk '$1 == "eta" {print "found", $2}' a3

echo @@aw_fields;    awk '{print $1, $3}' a3
echo @@aw_setfield;  awk '{$2="X"; print}' a3
echo @@aw_set0;      awk '{$0="p q r"; print NF, $2}' a3
echo @@aw_nfdown;    awk '{NF=2; print; print NF}' a3
echo @@aw_nfup;      awk '{NF=5; print NF; print $0 "|"}' a3
echo @@aw_newfield;  awk '{$5="E"; print NF, $0}' a3
echo @@aw_ofsrebuild; awk 'BEGIN{OFS="-"} {$1=$1; print}' a3
echo @@aw_ofsnotouch; awk 'BEGIN{OFS="-"} {print $1, $2; print $0}' a3
echo @@aw_dollarexpr; awk '{i=2; print $i, $(i+1)}' a3
echo @@aw_nrfnr;     awk '{print NR, FNR, FILENAME}' a3 aitems

echo @@aw_fscomma;   awk -F, '{print $2}' acsv
echo @@aw_fscolon;   awk -F: '{print $1, $3}' apw
echo @@aw_fsvar;     awk 'BEGIN{FS=":"} NR>1{print $2}' apw
echo @@aw_fsregex;   awk -F'[,:]' '{print $2}' acsv
# The trailing `echo` matters: ORS=";" means the last record has no
# newline, and without one the NEXT @@ marker lands on this line.
echo @@aw_ofsors;    awk 'BEGIN{OFS="|";ORS=";"} {print $1,$2}' a3; echo
echo @@aw_rspara;    awk 'BEGIN{RS=""} {print NR": "$1"/"$NF"/"NF}' apara
echo @@aw_rschar;    awk 'BEGIN{RS=","} {print NR, $0}' acsv

echo @@aw_concatminus; awk 'BEGIN{print "x" - 1}'
echo @@aw_negpow;    awk 'BEGIN{print -2^2}'
echo @@aw_powright;  awk 'BEGIN{print 2^3^2}'
echo @@aw_concat;    awk 'BEGIN{x="a"; y="b"; print x y "c"}'
echo @@aw_mod;       awk 'BEGIN{print 7%3, -7%3}'
echo @@aw_incdec;    awk 'BEGIN{i=5; print i++, i, ++i, i--, i}'
echo @@aw_aug;       awk 'BEGIN{x=10; x+=5; x-=3; x*=2; x/=4; x%=4; x^=2; print x}'
echo @@aw_logic;     awk 'BEGIN{print (1&&0), (1||0), !1, !0, !"", !"x"}'
echo @@aw_uninit;    awk 'BEGIN{print (x==0), (x==""), length(x), x+0}'
echo @@aw_cmpstrnum; printf '10.0\n10x\n' | awk '{print ($1==10), ($1=="10"), ($1<9)}'
echo @@aw_cmpstr;    awk 'BEGIN{print ("abc"<"abd"), ("10"<"9"), (10<9)}'
echo @@aw_leadspace; awk 'BEGIN{print (" 10 "+0), ("  x"+0), ("3x"+0), (".5"+0), ("1e3"+0)}'

echo @@aw_while;     awk 'BEGIN{i=0; while(i<3){print i; i++}}'
echo @@aw_do;        awk 'BEGIN{i=0; do{print i; i++}while(i<3)}'
echo @@aw_for;       awk 'BEGIN{for(i=0;i<3;i++) print i}'
echo @@aw_break;     awk 'BEGIN{for(i=0;i<3;i++){for(j=0;j<3;j++){if(j==1)break; print i,j}}}'
echo @@aw_continue;  awk 'BEGIN{for(i=0;i<5;i++){if(i%2)continue; print i}}'
echo @@aw_next;      awk 'NR==2{next} {print}' a3
echo @@aw_exitcode;  awk '{print} END{exit 3}' a3; echo "rc=$?"
echo @@aw_exitend;   awk 'NR==2{exit} END{print "END", NR}' a3
echo @@aw_range;     awk '/delta/,/eta/' a3
echo @@aw_rangesame; awk '/delta/,/delta/' a3

echo @@aw_array;     awk 'BEGIN{a["x"]=1; a["y"]=2; n=0; for(k in a) n++; print n, a["x"], a["y"]}'
echo @@aw_in;        awk 'BEGIN{a[1]=1; print (1 in a), (2 in a)}'
echo @@aw_delete;    awk 'BEGIN{a[1];a[2];delete a[1]; print (1 in a), (2 in a), length(a)}'
echo @@aw_deleteall; awk 'BEGIN{a[1];a[2];delete a; print length(a)}'
echo @@aw_multisub;  awk 'BEGIN{a[1,2]="x"; print ((1,2) in a), a[1,2]}'
echo @@aw_subsep;    awk 'BEGIN{SUBSEP=":"; a[1,2]=3; for(k in a) print k}'
echo @@aw_count;     awk '{for(i=1;i<=NF;i++) c[$i]++} END{n=0; for(k in c) n+=c[k]; print n}' a3
echo @@aw_increate;  awk 'BEGIN{ if ("k" in a) print "y"; print length(a) }'
echo @@aw_refcreate; awk 'BEGIN{ if (a["k"]=="") print "empty"; print length(a) }'

echo @@aw_func;      awk 'function add(x,y){return x+y} BEGIN{print add(2,3)}'
echo @@aw_recur;     awk 'function f(n){return n<=1?1:n*f(n-1)} BEGIN{print f(6)}'
echo @@aw_funclocal; awk 'function g(a, i,t){for(i=1;i<=3;i++)t=t i; return t} BEGIN{i=99; print g(1), i}'
echo @@aw_arrayref;  awk 'function fill(arr){arr["k"]="v"} BEGIN{fill(a); print a["k"]}'
echo @@aw_byval;     awk 'function s(x){x=99} BEGIN{y=1; s(y); print y}'
echo @@aw_fwdcall;   awk 'BEGIN{print g(3)} function g(n){return n*2}'
echo @@aw_arrnest;   awk 'function p(x){x["q"]=1; q(x)} function q(y){y["r"]=2} BEGIN{p(z); print z["q"], z["r"]}'
echo @@aw_deep;      awk 'function d(n){ return n==0 ? 0 : 1+d(n-1) } BEGIN{ print d(200) }'

echo @@aw_length;    awk 'BEGIN{print length("hello")}'
echo @@aw_substr;    awk 'BEGIN{print substr("hello",2), substr("hello",2,3), substr("hello",0,3), substr("hello",4,99)}'
echo @@aw_index;     awk 'BEGIN{print index("hello","ll"), index("hello","z")}'
echo @@aw_split;     awk 'BEGIN{n=split("a:b:c",p,":"); print n, p[1], p[3]}'
echo @@aw_splitre;   awk 'BEGIN{n=split("a1b22c",p,/[0-9]+/); print n, p[1], p[2], p[3]}'
echo @@aw_splitdef;  awk 'BEGIN{n=split("  a  b  ",p); print n, "x"p[1], "x"p[2]}'
echo @@aw_match;     awk 'BEGIN{print match("hello","l+"), RSTART, RLENGTH}'
echo @@aw_matchnone; awk 'BEGIN{print match("hello","z"), RSTART, RLENGTH}'
echo @@aw_subamp;    awk 'BEGIN{s="abc"; sub(/b/,"<&>",s); print s}'
echo @@aw_gsubempty; awk 'BEGIN{s="abc"; n=gsub(/x*/,"-",s); print n, s}'
echo @@aw_gsubanch;  awk 'BEGIN{s="aaa"; print gsub(/^a/,"X",s), s}'
echo @@aw_sprintf;   awk 'BEGIN{print sprintf("%05.2f|%s|%d", 3.14159, "hi", 42)}'
echo @@aw_case;      awk 'BEGIN{print toupper("aBc"), tolower("aBc")}'

echo @@aw_pfbasic;   awk 'BEGIN{printf "%d %s %c %o %x %X\n", 65, "s", 66, 8, 255, 255}'
echo @@aw_pffloat;   awk 'BEGIN{printf "%f|%.2f|%e|%g\n", 3.14159, 3.14159, 31415.9, 0.0000123}'
echo @@aw_pfwidth;   awk 'BEGIN{printf "x%5dx%-5dx%05dx%5.2sx\n", 42, 42, 42, "abcdef"}'
echo @@aw_pfstar;    awk 'BEGIN{printf "x%*dx%.*fx\n", 6, 42, 3, 3.14159}'
echo @@aw_pfpct;     awk 'BEGIN{printf "100%%\n"}'
echo @@aw_pfparens;  awk 'BEGIN{printf("%s/%s\n","a","b")}'
echo @@aw_pfplus;    awk 'BEGIN{printf "%+d % d %#o %#x\n", 5, 5, 8, 255}'
echo @@aw_printpar;  awk 'BEGIN{print (1), (2)}'
echo @@aw_ofmt;      awk 'BEGIN{OFMT="%.2f"; print 3.14159, 3.0}'
echo @@aw_convfmt;   awk 'BEGIN{CONVFMT="%.2f"; x=3.14159; print x ""}'
echo @@aw_intprint;  awk 'BEGIN{x=1; print x "", 100000000, 1/3}'

echo @@aw_matchop;   awk '$0 ~ /et/ {print $1}' a3
echo @@aw_nomatchop; awk '$0 !~ /a/ {print $1}' a3
echo @@aw_barere;    awk '/eta/{print $1}' a3
echo @@aw_dynre;     awk -v r=et '$0 ~ r {print $1}' a3
echo @@aw_reclass;   printf 'ab1cd\n' | awk '{gsub(/[[:digit:]]/,"D"); print}'
echo @@aw_reescbr;   printf 'a\tb\n' | awk '{ if ($0 ~ /[\t]/) print "TAB"; else print "no" }'
echo @@aw_redotlit;  awk 'BEGIN{s="a.b"; gsub(/\./,"-",s); print s}'
echo @@aw_recaret;   awk 'BEGIN{s="a1b2"; gsub(/[^0-9]/,"",s); print s}'
echo @@aw_rematchgrp; awk 'BEGIN{s="default via 10.0.2.2 dev eth0"; if (match(s,/via [0-9.]+ /)) print substr(s,RSTART+4,RLENGTH-5)}'

echo @@aw_glplain;   awk 'NR==1{getline; print "got", $1}' a3
echo @@aw_glvar;     awk 'NR==1{getline x; print "x="x}' a3
echo @@aw_glfile;    awk 'BEGIN{while((getline line < "a3")>0) n++; print n}'
echo @@aw_gleof;     awk 'BEGIN{print (getline z < "/nonexistent/x")}'
echo @@aw_glreopen;  awk 'BEGIN{getline a < "a3"; close("a3"); getline b < "a3"; print (a==b)}'
echo @@aw_glcmd;     awk 'BEGIN{ while (("cat a3" | getline l) > 0) n++; print n }'

echo @@aw_redirfile; awk '{print $1 > "aout1"} END{while((getline l < "aout1")>0) print "R:"l}' a3
echo @@aw_redirapp;  rm -f aout2; awk '{print $1 >> "aout2"} END{close("aout2"); while((getline l < "aout2")>0) print "A:"l}' a3

echo @@aw_vassign;   awk -v x=5 'BEGIN{print x+1}'
echo @@aw_vescape;   awk -v 'x=a\tb' 'BEGIN{print length(x)}'
echo @@aw_operandas; awk '{print v, $1}' v=1 a3
echo @@aw_argv;      awk 'BEGIN{print ARGC, ARGV[0], ARGV[1]}' a3
echo @@aw_multifile; awk '{print FILENAME, FNR, NR}' a3 aitems
echo @@aw_endonly;   awk 'END{print NR}' a3
echo @@aw_nomain;    awk 'BEGIN{print "only"}' a3
echo @@aw_comment;   awk 'BEGIN{ # a comment
print "ok" }'
echo @@aw_semis;     awk 'BEGIN{;;print "a";;}'
echo @@aw_nrend;     awk '{}END{print NR, $0}' a3

echo @@aw_badprog;   awk 'BEGIN{' >/dev/null 2>&1; echo "rc_nonzero=$([ $? -ne 0 ] && echo yes || echo no)"
echo @@aw_divzero;   awk 'BEGIN{print 1/0}' >/dev/null 2>&1; echo "rc_nonzero=$([ $? -ne 0 ] && echo yes || echo no)"

# A cached number-to-string conversion must not change the value's TYPE.
# Before 2026-09-06 it did: `x=2; s = x ""` made every later comparison of
# x a STRING comparison, so x < 10 was false.
printf '3.14159\n2.5\n' > afloats
echo @@aw_numafterstr;  awk 'BEGIN{x=10; s=x""; print (x<9)}'
echo @@aw_numafterstr2; awk 'BEGIN{x=2;  s=x""; print (x<10)}'
echo @@aw_numafterstr3; awk 'BEGIN{x=10; s=x""; print (x==10), (x=="10")}'
echo @@aw_strafternum;  awk 'BEGIN{x="0abc"; y=x+0; if (x) print "t"; else print "f"}'
echo @@aw_fieldcached;  awk '{ x=$1; y=x""; print (x<10) }' aitems
echo @@aw_ofmtafter;    awk 'BEGIN{OFMT="%.2f"; x=3.14159; s=x ""; print x}'
echo @@aw_ofmtfield;    awk '{ OFMT="%.2f"; print $1 }' afloats

# The record reader's buffer is 8 KiB; these force every RS path through a
# refill, which is where an absolute buffer index goes wrong.
awk 'BEGIN{for(i=0;i<40;i++){s="";for(j=0;j<300;j++)s=s "abcdefghij"; print s}}' > abig
awk 'BEGIN{for(i=0;i<4000;i++) printf "rec%d;", i; print ""}' > asemis
awk 'BEGIN{for(i=0;i<300;i++){for(j=0;j<40;j++) print "line" j; print ""}}' > abigpara
awk 'BEGIN{for(i=0;i<4000;i++) printf "rec%d!!", i; print ""}' > abangs
echo @@aw_biglines;   awk 'END{print NR, length($0)}' abig
echo @@aw_biggetline; awk 'BEGIN{while((getline l < "abig")>0){n++; t+=length(l)} print n, t}'
echo @@aw_bigrschar;  awk 'BEGIN{RS=";"} END{print NR}' asemis
echo @@aw_bigpara;    awk 'BEGIN{RS=""} {n++; c+=length($0)} END{print n, c}' abigpara
echo @@aw_bigrsre;    awk 'BEGIN{RS="!+"} END{print NR}' abangs
echo @@aw_bignf;      awk 'NR==1{print NF}' abig

echo @@awk_END
