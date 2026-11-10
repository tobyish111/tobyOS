test_arr1=()
declare -a test_arr2=()
declare -A test_arr3=()
test_arr4=(1 2 3)
declare -a test_arr5=(1 2 3)
declare -A test_arr6=(['a']=1 ['b']=2 ['c']=3)
test_arr7=()
test_arr7[3]=foo
declare -p test_arr{1..7}
