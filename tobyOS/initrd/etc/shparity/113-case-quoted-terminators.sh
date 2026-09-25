p() { printf '[%s]' "$1"; printf '\n'; }
case y in
  y) v="k
;;
esac"
  ;;
esac
p "Q27 $v"
case x in x) echo 'Q28 ;;' ;; esac
case x in x) echo "Q29 esac)";; esac
case ')' in ')') echo Q30 paren;; *) echo Q30 other;; esac
case x in x) echo $'Q31 a\'b;;' ;; esac
case x in x) echo Q32 a\;\; b ;; esac
case x in
  x) cat <<EOF
Q33 don't stop
EOF
  echo Q33 after ;;
  y) echo Q33 wrong ;;
esac
case x in
  x) cat <<EOF
Q34 a " quote ;; esac
EOF
  ;;
esac
echo Q34 end
echo done
