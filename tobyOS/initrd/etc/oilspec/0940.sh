kill -l 128
if [ $? -ne 0 ]; then
    echo "invalid"
fi
