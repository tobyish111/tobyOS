A=1 B=$(echo x) D="[$A][$B]" env | grep '^D='
A=1 B=$(true; echo x) D="[$A][$B]" env | grep '^D='
A=1 B=$(C=2 echo x) D="[$A][$B]" env | grep '^D='
A=1 B=$(C=2 true; echo x) D="[$A][$B]" env | grep '^D='
B=$(C=2 true; echo x) D="[$B]" env | grep '^D='
B=$(C=2 true; echo x); echo "plain [$B]"
