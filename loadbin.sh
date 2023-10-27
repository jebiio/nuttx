#!/bin/bash

pwd=$(pwd)
{ 
  echo "connect" 
  sleep 0.2
  echo 
  sleep 0.2
  echo "s" 
  sleep 0.2
  echo "1000" 
  sleep 0.2
  echo "r" 
  sleep 0.2
  echo "loadbin $(pwd)/nuttx.bin 0x08000000" 
  r
} | JLinkExe