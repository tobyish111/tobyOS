# from Crestwave's bf.bash

program='^++--hello.,world<>[]'
program=${program//[^'><+-.,[]']} 
echo $program
