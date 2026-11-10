# -c is special, with quit_parsing_flags

echo 'foo-bar' | { read -d -; echo reply=$REPLY; }
