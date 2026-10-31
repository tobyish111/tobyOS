# shopt -s -o and shopt -u -o with no names list the set -o options that
# are on, or off, alone -- tsh listed them all (finding 431).
exec 2>&1
set -o noglob
shopt -s -p -o | grep -c .; shopt -s -p -o | grep noglob
shopt -u -o | grep -c .; shopt -u -o | grep -E '^(allexport|noglob) '
shopt -o -p | grep -c .; set +o noglob
