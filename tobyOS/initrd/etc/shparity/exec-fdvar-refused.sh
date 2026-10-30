# A {name} redirection whose name cannot take the descriptor says "name:
# cannot assign fd to variable" after the assignment's own message (finding
# 337).
{
( readonly fd=1; exec {fd}>/dev/null; echo "rc=$? fd=$fd" )
} 2>&1
