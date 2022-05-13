#!/bin/bash

s=$1

echo Input: "$s"

while (( ${#s} < 6 ))
do
   s="0$s"
done

echo "$s"
echo "::set-output name=build-number::$s"
