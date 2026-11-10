# bash 5.2 fixed bash 4.4 bug: this is now checked

# case from
# https://lists.gnu.org/archive/html/bug-bash/2020-05/msg00066.html

set -o errexit

while read line; do
 echo $line
done < not_exist.txt   

echo status=$?
echo 'should not get here'
