set -o errexit

if command time -f '%e %M' true; then
  echo 'supports -f'
  # BUG: this was wrong
  #time -f '%e %M' true

  # Need 'command time'
  command time -f '%e %M' true
fi

if env time -f '%e %M' true; then
  echo 'env'
  env time -f '%e %M' true
fi
