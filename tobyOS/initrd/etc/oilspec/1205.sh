# Oh this is because it's run for the PIPELINE, not for the last thing!  Hmmm

trap 'echo group2' ERR
{ false; } | { false; } | { false; false; }

echo ok
