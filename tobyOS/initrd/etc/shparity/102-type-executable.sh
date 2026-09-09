# `type` and `command -v` against files that are and are not executable.
#
# Oils 1260 ("type -t doesn't find non-executable") and 1287 ("type of a
# relative path") went the WRONG WAY when path_is_runnable landed -- the
# predicate they all share started requiring the execute bit, which fixed
# `command -v` (0963) and `$PATH` lookup (1419) but moved these two. The
# oilspec gate prints per-case diffs only for POSIX-class failures and
# these are BASH-ONLY, so there is nothing to read; this probes each shape
# separately so the divergence names itself.
#
# Everything is built here rather than assumed, and the outputs are shapes
# (found / not-found / status) rather than paths, so the transcript does
# not depend on where the case runs.

d=t102; rm -rf $d; mkdir -p $d
: > $d/plain          # exists, NOT executable
: > $d/runme; chmod +x $d/runme

# 0. WHAT MODE DID THESE ACTUALLY GET? bash and tsh disagreed about
# whether $d/plain is a command, and they read the same stat -- so the
# first thing to establish is what the file's bits really are.
ls -l $d/plain | cut -c1-10
ls -l $d/runme | cut -c1-10

# 1. a RELATIVE PATH with a slash: type does not search PATH for these.
type $d/plain  >/dev/null 2>&1; echo "rel-plain  status=$?"
type $d/runme  >/dev/null 2>&1; echo "rel-runme  status=$?"
type -t $d/plain >/dev/null 2>&1; echo "rel-t-plain status=$?"
type -t $d/runme; echo "rel-t-runme status=$?"

# 2. the same two found through PATH.
PATH="$PWD/$d:$PATH"
type -t plain >/dev/null 2>&1; echo "path-t-plain status=$?"
type -t runme; echo "path-t-runme status=$?"
command -v plain >/dev/null 2>&1; echo "path-v-plain status=$?"
command -v runme >/dev/null 2>&1; echo "path-v-runme status=$?"

# 3. -p and -P report a path only when the name would really run.
type -p plain >/dev/null 2>&1; echo "p-plain status=$?"
type -p runme >/dev/null 2>&1; echo "p-runme status=$?"
type -P runme >/dev/null 2>&1; echo "P-runme status=$?"

# 4. a builtin and a keyword are unaffected by any of this.
type -t echo
type -t if
type -t nosuchcommandanywhere >/dev/null 2>&1; echo "missing status=$?"

rm -rf $d
echo end
