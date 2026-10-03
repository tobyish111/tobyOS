trap 'echo "E:$BASH_SUBSHELL"' ERR
echo 1; (false)
echo 2; (false) | (false)
echo 3; false | (false)
echo 4; (false) | false
echo 5; { false; } | { false; }
echo 6; (false; true) | (false)
echo 7; (exit 3)
echo 8; ( (false) )
echo 9; x=$(false)
echo 10; x=$( (false) )
echo end
