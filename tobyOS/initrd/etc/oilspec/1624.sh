shopt -s extglob
for word in --help --verbose --unmatched -- -zxzx -; do
  case $word in
    --@(help|verbose) )
      echo A
      continue
      ;;
    ( --?(b|c) )
      echo B
      continue
      ;;
    ( -+(x|z) )
      echo C
      continue
      ;;
    ( -*(x|z) )
      echo D
      continue
      ;;
    *)
      echo U
      continue
      ;;
  esac
done
