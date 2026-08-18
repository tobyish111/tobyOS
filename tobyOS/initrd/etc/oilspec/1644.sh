# bash is lenient; zsh disagrees

for ((i = '3';  i < '5';  ++i)); do echo $i; done
for ((i = "3";  i < "5";  ++i)); do echo $i; done
for ((i = $'3'; i < $'5'; ++i)); do echo $i; done
for ((i = $"3"; i < $"5"; ++i)); do echo $i; done
