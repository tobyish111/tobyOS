# A builtin or function run as a PIPELINE STAGE is a subshell in bash
# (lastpipe off): what it does to arrays stays there. /bin/tsh runs such a
# stage in-process (shell_run_pipeline_shell_stage), in a frame.
fa() { pa=(1 2); pb[3]=x; unset pc; }
pb=(0); pc=(9)
echo q | fa
declare -p pa pb pc 2>&1
fb() { read -a ra; }
echo "r s t" | fb
declare -p ra 2>&1
mapfile -t mf < <(printf '1\n2\n') | cat
declare -p mf 2>&1
printf '1\n2\n' | mapfile -t mg
declare -p mg 2>&1
