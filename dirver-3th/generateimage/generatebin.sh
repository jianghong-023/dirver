#!/bin/bash

file_name="ydnsystem.bin"
str="--help"
num=2
argc=$#
let "argc=$argc+1"
preloader="./preloader-mkpimage.bin"
devtree="./socfpga_cyclone5.dtb"
uboot="./u-boot.img"
zimage="./zImage"
rootfs="rootfs.jffs2"
fpga="./output_file.rbf"


if [ $argc -lt $num ] ; then
	
	echo "**************************************************"
	echo "* Usage ..."
	echo "* ./generatebin.sh [PATH]"
	echo "* PATH : You want to store the file path"
	echo "* --help"
	echo "* View partition"
	echo "**************************************************"
	exit 1
fi

function part_regin()
{
	echo "************************************************"
	echo "* system partition regin :"
	echo "* preloader-mkpimage [0x00000 ~ 0x4FFFF  ]"
	echo "* device tree        [0x50000 ~ 0x6FFFF  ]"
	echo "* u-boot		   [0x70000 ~ 0xDFFFF  ]"
	echo "* zImage		   [0xE0000 ~ 0x79FFF  ]"
	echo "* rootfs		   [0x80000 ~ 0x17FFFFF]"
	echo "* fpga		   [0x1800000 ~ 0x2000000]"
	echo "*************************************************"
}

function copy_file()
{
	if [ -f "$file_name" ] ; then
		cp  -rf ./$file_name $1
	fi
}

if [ "$1" == "$str" ] ; then
	part_regin
	exit 1
fi

echo "start build file,please wait ..."

if [ -d "$file_name" ] ; then
	rm -rf $file_name
fi

if [ ! -f "$preloader" ] ;then
	echo "preloader-mkpimage not existent,exit shell ... "
	exit 1
else
	echo "build preloader ..."
	dd if=$preloader of=$file_name  bs=1 count=327679
fi

if [ ! -f "$devtree" ] ;then
	echo "device tree image[socfpga_sclone5.dtb] not existent,exit shell ..."
	exit 1
else
	echo "build device tree ..."
	dd if=$devtree of=$file_name seek=327680 bs=1 count=131071
fi

if [ ! -f "$uboot" ] ; then
	echo "u-boot[u-boot.img] image not existent,exit shell ..."
	exit 1
else
	echo "build uboot ..."
	dd if=$uboot of=$file_name seek=458752 bs=1 count=458751
fi

if [ ! -f "$zimage" ] ; then
	echo "zImage not existent,exit shell ..."
	exit 1
else
	echo "build zImage ..."
	dd if=$zimage of=$file_name seek=917504 bs=1 count=7077887
fi

if [ ! -f "$rootfs" ] ; then
	echo "rootfs[rootfs.jffs2] image not existent,exit shell ..."
	exit 1
else
	echo "build roofs ..."
	dd if=$rootfs of=$file_name seek=8388608 bs=1 count=16777215
fi

if [ ! -f "$fpga" ] ; then

	echo "fpga[output_file.rfb] image not existent,exit shell ..."
	exit 1
else
	echo "build fpga ..."
	dd if=$fpga of=$file_name seek=25165824 bs=1 count=8388608
fi

if [ ! -x "$file_name" ] ; then
	chmod a=wrx $file_name
	copy_file
else
	
	copy_file
fi

echo " build finish ..."

