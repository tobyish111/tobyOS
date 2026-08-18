shopt -s extglob
mkdir eg6
argv.py eg6/@(no|matches)  # no matches
shopt -s nullglob  # test this too
argv.py eg6/@(no|matches)  # no matches
