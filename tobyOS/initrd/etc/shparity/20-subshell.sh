# GAP 6: ( ) subshells and { } groups, including what follows them.
# A subshell only parsed when it was the entire line -- `(exit 3); echo $?`
# was rejected as "expected ')'" because the parser refused any tail.
(exit 3); echo "sub=$?"
(true); echo "subtrue=$?"
(false); echo "subfalse=$?"

{ false; }; echo "brace=$?"
{ true; }; echo "bracetrue=$?"
{ echo g1; echo g2; }; echo "group=$?"

# A subshell gets its own copy of the environment.
x=outer
(x=inner; echo "in=$x")
echo "out=$x"

# ...a brace group does not.
y=before
{ y=after; }
echo "group-y=$y"

(echo s1; echo s2); echo "multi=$?"
(exit 0) && echo "and-after-sub"
(exit 1) || echo "or-after-sub"
echo subshell-done
