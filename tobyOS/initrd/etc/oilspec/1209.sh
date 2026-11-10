trap 'echo line=$LINENO' ERR

false & wait

{ false; echo async; } & wait
