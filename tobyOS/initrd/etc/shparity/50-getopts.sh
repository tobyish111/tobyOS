# POSIX XCU getopts -- option parsing, OPTIND and OPTARG.
# This is the sanctioned way for a shell script to parse its own flags, so a
# shell without a working getopts cannot host portable scripts.
parse() {
    OPTIND=1
    while getopts "ab:c" opt; do
        case "$opt" in
            a) echo "  -a" ;;
            b) echo "  -b arg=[$OPTARG]" ;;
            c) echo "  -c" ;;
            ?) echo "  unknown" ;;
        esac
    done
    shift $((OPTIND - 1))
    echo "  rest n=$# [$1]"
}

echo "1:"
parse -a
echo "2:"
parse -a -c
echo "3:"
parse -b value
echo "4:"
parse -b value -a
echo "5:"
parse -ac
echo "6:"
parse -a operand
echo "7:"
parse -b val operand1 operand2
echo "8:"
parse operand-only
echo "9:"
parse
echo "10:"
parse -a -- -notanopt
echo end
