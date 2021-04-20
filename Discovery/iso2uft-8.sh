#! /bin/bash

files=`find ./ -type f -exec file {} \; | grep 'ISO-8859' | cut -d ':' -f 1`

for file in ${files}
do
	iconv -f GBK -t UTF-8 ${file} > ${file}-utf
	if (($?==0)); then
		mv ${file}-utf ${file}
	else
		echo ${file} && rm ${file}-utf
	fi
done

