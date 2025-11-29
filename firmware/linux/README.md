# About This Directory

This directory contains files used to build alternative firmware for badge,
running Linux. It is based off of:

* Linux
* buildroot
* [buildroot-submodule](https://github.com/Openwide-Ingenierie/buildroot-submodule) 
* [esp32-linux-build](https://github.com/jcmvbkbc/esp32-linux-build)

Special thanks to [marble](https://chaos.social/@marble)
for being [crazy enough](https://chaos.social/@marble/115543049028792450) to try compiling Linux for this badge!

# TEMPORARY build instructions

First, make sure you've recursively checked out this repo. If not, do:

`$ git submodule update --init --recursive`

Next, build the toolchain:

```
pushd toolchain

make -C xtensa-dynconfig ORIG=1 CONF_DIR=`pwd` esp32s3.so

export XTENSA_GNU_CONFIG=`pwd`/xtensa-dynconfig/esp32s3.so

pushd crosstool-NG
./bootstrap && ./configure --enable-local && make
./ct-ng xtensa-esp32s3-linux-uclibcfdpic
CT_PREFIX=`pwd`/builds nice ./ct-ng build
popd
popd
```

Now we can build the kernel and file system image:

```
make
```