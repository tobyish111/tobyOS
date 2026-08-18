echo hello > OSCFLAGS
case `cat OSCFLAGS` in
  hello)
    echo hello
    ;;
  *)
    echo other
    ;;
esac > OSCFLAGS
cat OSCFLAGS
