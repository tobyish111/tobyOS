{ sleep 0.1 | seq 3; } | cat
{ sleep 0.1 | seq 10; } | { cat | cat; } | wc -l
