fun() {
  case $0 in
    *sh)
      echo 'sh'
      ;;
    *sh-*)  # bash-4.4 is OK
      echo 'sh'
      ;;
  esac

  echo $1 $2
}
fun a b
