#!/bin/bash

trap byefunc EXIT

byefunc()
{
    [[ "$NO_CPUINFO" == "1" ]] && return
    lscpu
}

get_corecnt()
{
    corecnt=`grep "" /sys/devices/system/cpu/cpu[0-9]*/topology/core_id | sed 's/.*://g' | uniq | tail -1`
    ((corecnt++))
}

ctl_hyperthreads()
{
    get_corecnt
    for((c=0;c<$corecnt;c++)); do
        for core_id in `grep "" /sys/devices/system/cpu/cpu[0-9]*/topology/core_id`; do
            cpuid=`echo "$core_id" | sed 's/\// /g' | awk '{ print $5 }'`
            coreid=`echo "$core_id" | sed 's/.*://g'`
            [[ $c -ne $coreid ]] && continue
            onlinefile="/sys/devices/system/cpu/$cpuid/online"
            [[ ! -f "$onlinefile" ]] && continue
            echo $1 > $onlinefile
            break
        done
    done
}

# Disabling multicore will naturally disable hyperthreading as well
ctl_multicore()
{
    for((cpu=0;cpu<20;cpu++)); do
        onlinefile="/sys/devices/system/cpu/cpu$cpu/online"
        [[ ! -f "$onlinefile" ]] && continue
        echo $1 > $onlinefile
    done
}

usage()
{
    echo "Usage: $0 [disable_all/disable_ht]"
    echo "Usage: $0 [enable_all]"
    echo "ht=hyperthreading, _all enables/disables both multicore & hyperthreading"
}

[[ "$1" == "disable_all" ]] && ctl_multicore 0 && exit
[[ "$1" == "disable_ht" ]]  && ctl_hyperthreads 0 && exit
[[ "$1" == "enable_all" ]]  && ctl_multicore 1 && exit
[[ "$1" != "" ]] && NO_CPUINFO=1 && usage && exit
