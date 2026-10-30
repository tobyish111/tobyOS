# A run of `**` is one `**` -- `**/**` lists each path once, and a run that
# ends the pattern shows the directory itself without its `/` -- while a
# path two routes reach, as `**/a/**` reaches a/a/a under both a and a/a,
# is listed twice, as bash does (finding 350). tsh listed every path once.
shopt -s globstar
cd "${TMPDIR:-/tmp}" && rm -rf gsr.$$ && mkdir gsr.$$ && cd gsr.$$ || exit 1
mkdir -p {a,b}/{a,b}/a
p() { printf '<%s>' "$@"; echo; }
p **/**; p a/**; p a/**/**; p **/a/**; p **/**/a/**; p **/**/**/a
cd .. && rm -rf gsr.$$
