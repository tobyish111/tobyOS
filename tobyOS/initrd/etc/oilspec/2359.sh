HOME=/home/bar

x=~:${undef-~:~}
echo $x

# Most shells agree on a different behavior, but with the OSH parsing model,
# it's easier to agree with yash.  bash disagrees in a different way
