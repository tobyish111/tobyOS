set -- "-a" "--" "-c" "operand"
while getopts "a" name; do
    case "$name" in
        a)
            echo "a"
            ;;
        c)
            echo "c"
            ;;
        ?)
            echo "?"
            ;;
    esac
done
shift $((OPTIND - 1))
echo "$#"
echo "$@"
