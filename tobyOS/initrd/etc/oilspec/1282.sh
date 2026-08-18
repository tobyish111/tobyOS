touch _tmp/pwd
chmod +x _tmp/pwd
PATH=/bin:_tmp  # control output

# Function is also ignored
pwd() { echo function-too; }

type -ap pwd
echo ---

type -p pwd
