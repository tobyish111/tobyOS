set -- "-a" "--"
while getopts "a" name; do
        case "$name" in
                a)
                        echo "a"
                        ;;
                ?)
                        echo "?"
                        ;;
        esac
done
echo "name=$name"
echo "$OPTIND"
