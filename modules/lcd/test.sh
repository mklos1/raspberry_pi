#!/bin/bash
while :
   do
      echo -n `date +"%T"` > /sys/devices/soc/20804000.i2c/i2c-1/1-0027/content
      sleep 1
   done
