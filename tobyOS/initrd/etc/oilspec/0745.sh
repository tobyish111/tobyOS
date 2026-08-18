chdir /tmp

if test $? -ne 0; then
  echo fail
  exit
fi

pwd

# It's the same with no args, but mksh fails because of $HOME
#chdir
#echo status=$?
