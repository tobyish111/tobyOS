# A script sourced with arguments sees them as its parameters, and the
# caller's come back afterwards -- unless the script gave them new values with
# set, which then stay; shift, and a set inside a function, do not count
# (finding 426).
exec 2>&1
d=$(mktemp -d)
echo 'set -- m n o p' > "$d/s4.sub"; set -- a b c; . "$d/s4.sub" x y z; echo "[$*]"
set -- a b c; . "$d/s4.sub"; echo "[$*]"
echo 'echo "in: $*"' > "$d/s3.sub"; set -- a b c; . "$d/s3.sub" x y z; echo "[$*]"
echo 'shift' > "$d/s5.sub"; set -- a b c; . "$d/s5.sub" x y z; echo "[$*]"
printf 'f() { set -- q; }\nf\n' > "$d/s6.sub"; set -- a b c; . "$d/s6.sub" x y z; echo "[$*]"
echo 'set -- inner' > "$d/b.sub"; printf '. "%s/b.sub" k\necho "mid [$*]"\n' "$d" > "$d/a.sub"
set -- a b c; . "$d/a.sub" x y z; echo "[$*]"
rm -rf "$d"
