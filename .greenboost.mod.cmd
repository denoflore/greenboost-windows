savedcmd_greenboost.mod := printf '%s\n'   greenboost.o | awk '!x[$$0]++ { print("./"$$0) }' > greenboost.mod
