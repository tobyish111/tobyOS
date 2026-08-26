if [[ ! (" ${params[*]} " =~ " -shared " || " ${params[*]} " =~ " -static ") ]]; then
  echo one
fi

# Reduced idiom
if [[ (foo =~ foo) ]]; then
  echo two
fi
