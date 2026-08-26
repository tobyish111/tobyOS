HOME=/home/bob
[[ ~ == /home/bob ]]
echo status=$?

[[ ~ == */bob ]]
echo status=$?

[[ ~ == */z ]]
echo status=$?
