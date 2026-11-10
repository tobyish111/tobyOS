case $SH in bash*|dash|mksh) exit ;; esac

shopt --set ysh:all

for x in a b c { 
  echo $x
  if (x === 'a') {
    break
  }
}

echo ---

for keyword in break continue return exit {
  try {
    $[ENV.SH] -o ysh:all -c '
    var k = $1
    for x in a b c { 
      echo $x
      if (x === "a") {
        $k
      }
    }
    ' unused $keyword
  }
  echo code=$[_error.code]
  echo '==='
}
