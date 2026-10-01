# Every diagnostic starts where bash's does (S12): "SCRIPT: line N: " in a
# script, a SOURCED file's own name inside it, the file a function was
# DEFINED in inside the function, "$0: line N: " for -c, "environment" in a
# function -c text defined; a usage line has no prefix. tsh wrote "tsh: ",
# "shell: ", "sh: " or nothing. stderr is folded into stdout here so the
# gate compares it; the shell's own name is replaced by SHELL.
me=$(readlink /proc/$$/exe)
nosuch_fx173 2>&1
local lv_fx173 2>&1
f() {
  nosuch_in_f_fx173
}
f 2>&1
printf 'nosuch_src_fx173\ng() { nosuch_in_g_fx173; }\n' > /tmp/fx173src.sh
. /tmp/fx173src.sh 2>&1
g 2>&1
eval 'nosuch_eval_fx173' 2>&1
echo "[$(nosuch_cmdsub_fx173 2>&1)]"
"$me" -c 'nosuch_c_fx173' named 2>&1
"$me" -c 'h() { nosuch_ch_fx173; }; h' 2>&1
"$me" -c 'nosuch_c2_fx173' 2>&1 | sed "s#^$me: #SHELL: #"
history -q 2>&1
wait 99999 2>&1
( echo "${never_set_fx173?not set here}" ) 2>&1
x=$(local lc_fx173 2>&1); echo "[$x]"
rm -f /tmp/fx173src.sh
echo done
