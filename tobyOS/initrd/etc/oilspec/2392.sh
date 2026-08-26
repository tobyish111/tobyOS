IFS=x; X=x; eval abc=a${X}b

chicken() { for i in "${@:3:5}"; do echo =$i=; done; } ; chicken ab cd ef gh ij kl mn op qr

chicken() { for i in "${*:3:5}"; do echo =$i=; done; } ; chicken ab cd ef gh ij kl mn op qr
