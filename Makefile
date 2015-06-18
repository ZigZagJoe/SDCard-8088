### AVR executable Makefile stub for Posix

#PROGRAMMER = -cusbtiny
PROGRAMMER = -cstk500v1 -P/dev/tty.usbmodem80981
#NO_CORELIB = true
#OPTIMIZE = 3

F_CPU 	= 20000000L
MCU	= atmega324pa

LIBS 	+= -lSDFatLib2
INC	+= -I ../lib/SDFatLib2

LIBS 	+= -lMD5_ASM
INC	+= -I ../lib/MD5_ASM

LFUSE	= 0xEE
HFUSE	= 0xD5
EFUSE 	= 0xFF

include ../base.mk

libs: lib
	cd ../lib/SDFatLib2 && make clean all
