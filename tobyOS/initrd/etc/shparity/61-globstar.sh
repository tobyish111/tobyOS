# `**` -- the globstar match.
#
# A component that is exactly ** matches ZERO OR MORE directory levels,
# which the flat single-directory globber cannot express. `shopt -s
# globstar` was accepted and did nothing, so the pattern came back
# unexpanded -- the shape a script uses to find its own sources.
mkdir -p g/sub/deep
touch g/top.md g/sub/mid.md g/sub/deep/low.md g/other.txt
echo A: g/*.md
shopt -s globstar
echo B: g/**/*.md
echo C: g/**/*.txt
echo D: **/low.md
shopt -u globstar
echo E: g/**/*.md
echo done
