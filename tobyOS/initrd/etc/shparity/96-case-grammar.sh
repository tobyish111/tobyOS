# XCU 2.10: case-grammar corners. A pattern may carry an OPENING paren;
# `esac` is usable as a (parenthesised) pattern; first match wins; `;;`
# after the last item is optional.
case x in (x) echo "1=paren";; esac
case esac in (esac) echo "2=esac-pat";; *) echo "2=no";; esac
case x in x) echo "3=first";; x) echo "3=second";; esac
case y in
  a) echo "4=a";;
  y) echo "4=y"
esac
case "" in "") echo "5=empty";; *) echo "5=no";; esac
echo end
