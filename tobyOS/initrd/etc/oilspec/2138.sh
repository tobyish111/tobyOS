[[ foo123 =~ ([a-z]+)([0-9]+) ]]
echo status=$?
argv.py "${BASH_REMATCH[@]}"

[[ failed =~ ([a-z]+)([0-9]+) ]]
echo status=$?
argv.py "${BASH_REMATCH[@]}"  # not cleared!
