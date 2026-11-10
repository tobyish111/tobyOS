# bash and mksh give parse time error because of }
# dash gives 127 as runtime error
{ls; }
echo "status=$?"
