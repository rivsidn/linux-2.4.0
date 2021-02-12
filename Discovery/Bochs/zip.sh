#! /bin/bash

file="hdc.img"

if [ -f "${file}" ]; then
	zip hdc.img.zip hdc.img
	rm hdc.img
fi



