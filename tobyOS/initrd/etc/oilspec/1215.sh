trap 'echo line=$LINENO' ERR

for y in 1 2; do
  false
done

case x in
  x) false ;;
  *) false ;;
esac

{ false; false; false; }
echo ok
