#
# setup network error situation
#
# delay <base>ms <diff>ms
#
#
#DEV="dev eth1"
DEV="dev eth1"

P_DELAY="delay 300ms 100"
P_LOSS="loss 6%"
P_DUP="duplicate 50%"
P_CORRUPT="corrupt 20%"

#tc qdisc add $DEV root netem $P_DELAY $P_LOSS $P_DUP $P_CORRUPT 
tc qdisc change $DEV root netem $P_DELAY $P_LOSS $P_DUP $P_CORRUPT 
