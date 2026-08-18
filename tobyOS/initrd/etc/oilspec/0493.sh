f1() {
  local v
  echo "[$1,(local)] v: ${v-(unset)}"
}
f2() {
  f1 "$1,(func)"
}
f3() {
  local v=local
  f1 "$1,local,(func)"
}
v=global

f1 'global'
v=tempenv f1 'global,tempenv'
(export v=global; f1 'xglobal')

f2 'global'
v=tempenv f2 'global,tempenv'
(export v=global; f2 'xglobal')

f3 'global'



# init.unset x tempenv-in-localctx
[global,(local)] v: (unset)
[global,tempenv,(local)] v: tempenv
[xglobal,(local)] v: (unset)
[global,(func),(local)] v: (unset)
[global,tempenv,(func),(local)] v: (unset)
[xglobal,(func),(local)] v: (unset)
[global,local,(func),(local)] v: (unset)

# init.empty
[global,(local)] v: 
[global,tempenv,(local)] v: 
[xglobal,(local)] v: 
[global,(func),(local)] v: 
[global,tempenv,(func),(local)] v: 
[xglobal,(func),(local)] v: 
[global,local,(func),(local)] v: 

# init.inherit
[global,(local)] v: global
[global,tempenv,(local)] v: tempenv
[xglobal,(local)] v: global
[global,(func),(local)] v: global
[global,tempenv,(func),(local)] v: tempenv
[xglobal,(func),(local)] v: global
[global,local,(func),(local)] v: local
