x=${ for i in a b; do echo -$i-; done; }
echo "$x"

y=${|for i in a b; do REPLY+="-$i-"; done; }
echo "$y"

echo

x2=${ case foo in foo) echo sh-case ;; esac; }
echo "$x2"

y2=${|case foo in foo) REPLY=sh-case ;; esac; }
echo "$y2"
