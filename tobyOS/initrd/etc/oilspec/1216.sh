trap 'echo line=$LINENO' ERR

false

{ false 
  true
} > /zz  # error
echo ok


# doesn't update line for redirect
