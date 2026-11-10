# What `type` and `command -v` REPORT: every $PATH match, a function's
# source, and bash's own wording for aliases, keywords and builtins.
#
# The Oils corpus has twelve cases on this and they are all BASH-ONLY, so
# the oilspec gate prints no per-case diff for any of them. This runs the
# same shapes against the bash sitting in the same guest, where a diff
# names itself.
#
# Everything is built here rather than assumed, and $PWD is stripped from
# the output, so the transcript does not depend on where the case runs.

d=t103; rm -rf $d; mkdir -p $d/one $d/two
printf '#!/bin/sh\necho one\n' > $d/one/dup103; chmod +x $d/one/dup103
printf '#!/bin/sh\necho two\n' > $d/two/dup103; chmod +x $d/two/dup103
printf '#!/bin/sh\necho file\n' > $d/one/both103; chmod +x $d/one/both103

strip() { sed "s|$PWD/||g"; }

OLD103=$PATH
PATH="$PWD/$d/one:$PWD/$d/two:$PATH"

# 1. -a lists EVERY match in $PATH order; the bare form lists the first.
echo "== a"
type dup103 | strip
type -a dup103 | strip
type -ap dup103 | strip
type -aP dup103 | strip

# 2. A function is reported WITH ITS SOURCE, by type and by command -V.
echo "== fn"
f103() { echo hi; }
type f103
type -t f103
command -v f103
command -V f103

# 3. -f suppresses function lookup, so the file behind the name shows.
echo "== f"
both103() { echo func; }
type both103 | strip
type -f both103 | strip
type -t both103

# 4. A keyword and a builtin, including a POSIX special one -- bash calls
#    every builtin "a shell builtin" and never says "special".
echo "== kw"
type -t for
command -v for
type for
type eval
command -V eval
type -t eval

# 5. An alias, in bash's wording.
echo "== alias"
shopt -s expand_aliases
alias ll103='ls -lha'
type ll103
command -V ll103

# 6. The not-found message is a DIAGNOSTIC: it belongs on stderr, and a
#    2> redirection has to capture it. Both shells word it differently, so
#    what is compared is that the text arrived at all -- which is exactly
#    what Oils "type -> not found" asserts.
echo "== err"
type zz103 2>err103.txt; echo "status=$?"
grep -c 'zz103: not found' err103.txt || true
rm -f err103.txt

PATH=$OLD103
rm -rf $d
echo end
