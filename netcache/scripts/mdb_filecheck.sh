#!/bin/bash
CDIR=/stg/cache/solproxy
#
# libnc에 embedding된 sqlite3
#
SQLITE=/tmp/sqlite3
DBNAME=/stg/cache/solproxy/netcache.mdb

function mdb_check_exist {
        match=$($SQLITE $DBNAME "select count(*) from objects where hex(uuid)='$1'")
        if [ $match == 0 ]
        then
                echo "- file $1 not exist on DB"
                echo "hit any key"
                read in
        else
                echo "+ file $1 exist on DB"
        fi

}
for outer in {0..255}
do
        for inner in {0..255}
        do
                od=`printf "%02X" $outer`
                id=`printf "%02X" $inner`
                filelist=`ls $CDIR/$od/$id`

                if [ "X$filelist" != "X" ]
                then
                        #nel=${#filelist[@]}
                        #echo "$CDIR/$od/$id =" $nel

                        echo "Checking directory $CDIR/$od/$id ... hit any key"
                        #read m
                        for file in $filelist
                        do
                                mdb_check_exist ${file^^}
                        done
                fi

        done
done

