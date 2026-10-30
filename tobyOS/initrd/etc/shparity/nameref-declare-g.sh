# declare -g in a function resolves a name as the global scope has it: a
# global reference's target is written, not a local reference's (finding
# 343).
{
( f() { declare -n ref=var; declare -g ref=(X); declare -p ref var; }; declare -n ref=foo; f; echo outside; declare -p foo; declare -p var )
( f() { declare -n ref=v2; declare -gi ref=3+4; declare -p v2; }; declare -n ref=w2; f; declare -p w2 )
} 2>&1
