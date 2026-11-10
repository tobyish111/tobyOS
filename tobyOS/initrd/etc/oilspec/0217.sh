declare -A a=([apple]=red [orange]=orange)
a+=([lemon]=yellow [banana]=yellow)
echo "apple is ${a['apple']}"
echo "orange is ${a['orange']}"
echo "lemon is ${a['lemon']}"
echo "banana is ${a['banana']}"
