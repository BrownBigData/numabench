#!/bin/bash

if [ "$#" -ne 3 ]; then
    echo "Illegal number of parameters: (1) numa region, (2) numa distance, (3) function"
    exit
fi

#memory loop
for i in {10..30}
do
   echo "*******************************************"
   echo "Running numa_bench for 2^$i Byte"
    
     #threads loop
     for j in {1,2,4,8,16,32,64}
     do
       loops=`echo "2^((30-$i))*100" | bc` 
       echo "Running with $j threads: ./numa_bench $loops $j $i $1 $2 $3"
       ./numa_bench $loops $j $i $1 $2 $3 > stdout.txt
     done
   echo ""
done
