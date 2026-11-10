# $? and the && / || / ! operators. The status of a FAILED && chain is the
# status of the part that failed -- a detail that decides whether `set -e`
# scripts abort in the right place.
true;  echo "true=$?"
false; echo "false=$?"
true && echo and-ok
false || echo or-ok
if ! false; then echo negated; fi
false && echo never
echo "chain=$?"
true && false
echo "andfalse=$?"
(exit 3); echo "sub=$?"
(true); echo "subtrue=$?"
{ false; }; echo "brace=$?"
exit 5
