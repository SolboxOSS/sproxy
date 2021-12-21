file="/tmp/fuse.log"
export IFS=' '
offset=0
length=0
seq_offset=0
lineno=0
while read -r f1 f2 f3 f4 f5 f6 
do
	offset=${f3#*[}
	offset=${offset%]*}

	if [ $offset = "" ]
	then
		continue
	fi

	if expr "$offset" : '-\?[0-9]\+$' > /dev/null
	then

		if  [ $offset -ne $seq_offset ]
		then
			echo "sequence error:  expected=$seq_offset, got=$offset at line $lineno"
			length=${f4#*[}
			length=${length%]*}
	
			let seq_offset=$offset+$length
		else	
			length=${f4#*[}
			length=${length%]*}
	
			let seq_offset=$seq_offset+$length
		fi
	fi
	let lineno=$lineno+1

	if [ $(($lineno % 1000)) -eq 0 ]
	then
		echo "$lineno processed"
	fi
done < "$file"
