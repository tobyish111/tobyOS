case $SH in zsh|ash) exit ;; esac

PATH=".:$PATH"

for name in 'a[' 'a[5'; do
  echo "echo hi from $name: \$# args: \$@" > "$name"
  chmod +x "$name"
done

# this does not executed a[5
a[5 + 1]=
a[5 / 1]=y
echo len=${#a[@]}

# Not detected as assignment because there's a non-arith character
# bash and mksh both give a syntax error
a[5 # 1]=
