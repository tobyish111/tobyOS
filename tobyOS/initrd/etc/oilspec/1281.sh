touch _tmp/pwd
chmod +x _tmp/pwd
PATH=/bin:_tmp  # control output

type -a pwd
echo ---

pwd () 
{ 
    echo function-too
}

osh-normalize() {
  sed 's/shell function/function/'
}

type -a pwd | osh-normalize
echo ---

type -a -f pwd | osh-normalize
