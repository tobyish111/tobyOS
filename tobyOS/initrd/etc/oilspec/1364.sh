# These two code points form a single character.
two_code_points="__$(echo $'\u0061\u0300')__"

# U+0061 is A, and U+0300 is an accent.  
#
# (Example taken from # https://blog.golang.org/strings)
#
# However ? in bash/zsh only counts CODE POINTS.  They do NOT take into account
# this case.

for s in '__a__' '__μ__' "$two_code_points"; do
  case $s in
    __?__)
      echo yes
      ;;
    *)
      echo no
  esac
done
