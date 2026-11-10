# This case was found in Kubernetes and others
array=(aa bb '')
argv.py ${array[@]/#/prefix-}
