compgen -W 'foo $(( 1 / 0 )) bar'
echo status=$?
