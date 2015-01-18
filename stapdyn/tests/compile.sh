#!/bin/sh

#g++ -o test test.cc

stap --dyninst -m hello.so -v -p4 hello.stp
stap --dyninst -m ping2.so -v -p4 ping2.stp
stap --dyninst -m ping5.so -v -p4 ping5.stp
#stap --dyninst -m count.so -v -p4 count.stp
