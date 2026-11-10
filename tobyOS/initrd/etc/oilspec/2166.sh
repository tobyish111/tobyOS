if [[ 'a-b-c-d' =~ a-(b|  >>)-c-( ;|[de])|ff|gg ]]; then
  argv.py "${BASH_REMATCH[@]}"
fi

if [[ ff =~ a-(b|  >>)-c-( ;|[de])|ff|gg ]]; then
  argv.py "${BASH_REMATCH[@]}"
fi

# empty group ()

if [[ zz =~ ([a-z]+)() ]]; then
  argv.py "${BASH_REMATCH[@]}"
fi

# nested empty group
if [[ zz =~ ([a-z]+)(()z) ]]; then
  argv.py "${BASH_REMATCH[@]}"
fi
