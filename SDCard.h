#ifndef SDCARD_H
#define SDCARD_H

// size of the fifo
#define BUFFER_SIZE 512

/*
               _____ _____
		 PB0 -|     U     |- PA0  ADC0
		 PB1 -|           |- PA1  ADC1
INT2     PB2 -|           |- PA2  ADC2
		 PB3 -|           |- PA3  ADC3
    ~SS  PB4 -|           |- PA4  ADC4
    MOSI PB5 -|           |- PA5  ADC5
    MISO PB6 -|           |- PA6  ADC6
    SCK  PB7 -|           |- PA7  ADC7
      ~RESET -|           |- AREF
         VCC -| ATMEGA324 |- GND
         GND -|  DIP-40   |- AVCC
       XTAL1 -|           |- PC7
       XTAL2 -|           |- PC6
     RX0 PD0 -|           |- PC5
     TX0 PD1 -|           |- PC4
INT0 RX1 PD2 -|           |- PC3
INT1 TX1 PD3 -|           |- PC2
         PD4 -|           |- PC1
         PD5 -|           |- PC0
         PD6 -|___________|- PD7
 
 
 ############ THIS CIRCUIT ###############
			   _____ _____
             -|     U     |- D0  \
             -|           |- D1   \
             -|           |- D2    |
             -|           |- D3    | DATA
         ~SS -|           |- D4    | BUS
        MOSI -|           |- D5    |
        MISO -|           |- D6   /
         SCK -|           |- D7  /
      ~RESET -|           |- AREF   [N/C]
         VCC -| ATMEGA324 |- GND
         GND -|  DIP-40   |- AVCC
       XTAL1 -|           |- 
       XTAL2 -|           |- ~373_OE
          RX -|           |- ERR_BIT
          TX -|           |- LED
	INT0 / Q -|           |- ~FIFO_RESET
	   ~INT1 -|           |- ~IOW
         ~SW -|           |- ~IOR
         ~EF -|           |- ~FFLOP_RESET
             -|___________|- 
 
Connections:
 
RESET: ISP & 10k pull up 
XTAL1/XTAL2: 20mhz crystal & 18pf caps
 
PA0-7: data bus
 
PB4 (~SS) : SD connector & 10k pullup
PB5 (MOSI): ISP & SD connector
PB6 (MISO): ISP & SD connector
PB7 (SCK) : ISP & SD connector
 
PD0 (RX0) : debug RX
PD1 (TX0) : debug TX
PD2 (INT0): Q input / wake interrupt
PD3 (INT1): ~soft-reset interrupt
PD4: ~sw input
PD5: ~empty input

 Outputs:
 
PC0: flipflop ~reset
PC1: ~IOR
PC2: ~IOW
PC3: fifo ~reset
PC4: LED
PC5: '244 error bit (input 2.3)
PC6: '373 ~OE
 
 
 Remember, all transfers are 8 bit.
 All multibyte numeric values are transfered in little endian (msb last)
 
Addresses:
 
 ### Address 0: FIFO transfer
 Read: reads byte from fifo
 Write: writes byte into fifo
 
 ### Address 1: Control
 Read: returns status nibble
 Write: execute instruction described by byte
 
 Special function:
   Write to control w/ bit 7 (0x80) set resets uC. 
   Wait for busy bit to go low before sending further commands.
 
 STATUS BYTE:
 
 7    6     5     4     3     2     1     0
                              ____  ____
 ???  ???   ???   ???   ERR   EMPT  CARD  BUSY
 
 Values of bits marked with ??? are undefined and should be discarded.
 
 ERR: 0 when no error in last operation, otherwise see error handling
 ~EMPT: 0 when fifo is empty
 ~CARD: 0 if a card is present
 BUSY: 1 if an operation is in progress. All accesses to FIFO are ignored while high.
 
 ## INSTRUCTIONS:
 
 API: 
 
 Write 0 - BUFFER_SIZE bytes of argument data to address 0 (the fifo)
 Then, write to address 1 with the value of the instruction you wish to execute.
 if MSB is set (>127), the controller will reset itself.
 
 The busy bit of the status register will be set. No operations aside from 
 read status byte and reset should be performed; access to fifo is prevented.
 
 When busy returns to 0, the fifo will have been emptied, and
 a variable amount of bytes (return data) will be added to the fifo
 
 First, check the error bit of the status nibble. If set, see ERROR HANDLING below
 
 If not set, a variable number of bytes of data may have been put in the 
 fifo. The fifo must be emptied before beginning another operation.
 
 ON RESET:
 
 if the error bit is set after the controller has been reset, SD initalization failed
 no operations on the SD card should be attempted, as they will fail.
 
 ERROR HANDLING:
 
 if error bit is set after an instruction, fifo will contain a single 
 return byte describing the error.  No other return from the instruction
 will be generated.
 
 ### Example:
 
 out base, OPEN_READ ; file mode
 out base, 'f'		 ; file name
 out base, 'i'
 out base, 'l'
 out base, 'e'
 out base, '.'
 out base, 't'
 out base, 'x'
 out base, 't'
 out base, 0
 out control, OPEN   ; OPEN("file.txt", OPEN_READ)
 
 ; busy flag is now high; wait for it to go low.
 ; can do other things during this time...
 
 busy_wait:
 in control, al
 test al, 1
 je busy_wait
 
 in al, control 
 test al, 8
 
 je error
 
 ; no error occured - do more stuff
 
 */


//###### THESE FUNCTIONS MAY BE USED AT ANY TIME WHEN CARD PRESENT

// returns a directory listing of current directory
// argument (optional): page to continue from, 0 or non present for first page
// returns:
// <page # 1b>
// <name 1-12b>\0<size 4b>\0 (repeat until fifo empty)
// only complete entries will be returned
// if first byte of name is 0xFF, end of listing
#define DIR			9
#define DIR_NO_MORE_FILES 0xFF
#define FILES_PER_DIR_PAGE 34
// FILES_PER_DIR_PAGE = (int)((BUFFER_SIZE - 1) / 15)

// opens file named by data in fifo
// arguments:
// 1b mode
// 1-12 bytes of filename, null terminated
// if termination is missing, no action will be taken
// error bit set if file opening failed

#define OPEN		11
#define OPEN_READ   O_READ
#define OPEN_WRITE  (O_READ | O_WRITE | O_CREAT)
#define OPEN_APPEND (O_READ | O_WRITE | O_APPEND)
#define OPEN_TRUNC  (O_WRITE | O_CREAT | O_TRUNC)

// deletes file named by fifo
// arguments: 1-12 bytes of filename, null terminated, FOLLOWED BY 0xDE
// if 0xDE is missing or wrong, no action will be taken
// error bit set if file not deleted
#define DELETE		12

// closes open file, if any, and flushes buffers. 
// argument: none
// returns: none
#define CLOSE		13

// tests if file named by data in fifo exists
// arguments: 1-12 bytes of filename, null terminated
// error bit set on file does not exist
#define EXISTS		10

// argument: null-terminated filename
// returns:
// 4b: file size
// 16b: digest
#define FILE_MD5	19

// reads bench.txt as fast as possible locally
// returns:
// 4b: bytes read (0xFFFFFFFF on fail)
#define BENCH_READ  20

// writes bench.txt as fast as possible locally - 2000 * WRITE_MAX_SZ
// returns: 
// 4b: bytes written (0xFFFFFFFF on fail)
#define BENCH_WRITE 21

// enters directory named by null-terminated string in fifo
// Special case:
// "/" or "\"	returns to root directory
// error bit 0 on success, else 1 
#define CHDIR		22

//###### THESE FUNCTIONS REQUIRE AN OPEN FILE

// returns file length
// argument: none
// returns:
// 4b: file length
#define LENGTH		14

// returns absolute position
// argument: none
// returns:
// 4b: absolute position
#define POSITION	15

// seeks to absolute position
// argument: 4 byte position
// error bit is 0 on success
#define SEEK		16

// seeks to position relative to current
// argument: SIGNED 4 byte position
// error bit is 0 on success
#define SEEKREL		23

// reads bytes into fifo
// argument (optional): number of bytes to read (default: READ_MAX_SZ)
// returns:
// 2b: number of bytes read or error bit set
#define READ		17
#define READ_MAX_SZ (BUFFER_SIZE - 2)

// writes entire contents of fifo to open file
// argument: data bytes to read (max WRITE_MAX_SZ)
// returns:
// 2b: number of bytes written or error bit set
#define WRITE		18
#define WRITE_MAX_SZ BUFFER_SIZE

//####### SPECIAL FUNCTIONS, NO SD REQUIRED

// CRC16 of data in fifo
// return: 2b CRC16
#define CRCTEST		0x68

// returns 0xDEADBEEF
#define HELLO		0x69

// returns all data that was in fifo
// 0-512 bytes of data
#define ECHO		0x6A

// soft-reset of uC
// no return
#define RESET		0x80

//###### ERRORS ####################

enum { 
	ERROR_UNKNOWN = 128,		// 128 (unused)
	ERROR_FILE_ALREADY_OPEN,	// 129 - open
	ERROR_FILE_NOT_OPEN,		// 130 - read, write, seek, position, length
	ERROR_FAILED_TO_OPEN,		// 131 - open, md5, bench
	ERROR_BAD_ARGUMENT,			// 132 
	ERROR_WRITE_ERROR,			// 133
	ERROR_READ_ERROR,			// 134
	ERROR_INVALID_DIR,			// 135 - chdir
	ERROR_DIR_MAX_DEPTH,		// 136 - chdir
	ERROR_NONEXISTANT_FILE,		// 137 
	ERROR_OPERATION_FAILED,		// 138 - seek, delete
	ERROR_SD_NOT_PRESENT,		// 139 - (any operation)
	ERROR_UNKNOWN_INSTRUCTION   // 140
};

#ifdef SDCARD_CPP
// private macros & function defintions

// ease of use wrappers for instructions
#define FUNC_HANDLER(X) inline void X##_handler()
#define CASE_HANDLER(X) case X: X##_handler(); return;

// pin bit in port definitions

// PORTC (output)
#define REG_CS 6
#define ERR_BIT 5
#define LED 4
#define FIFO_RESET 3
#define IOW 2
#define IOR 1
#define FF_RESET 0

// PORTD (input)
#define Q  2
#define SW 4
#define EMPTY 5

// function aliases

#define _NOP		__asm("nop\n")
#define fileOpen	openFile.isOpen()
#define fifo_write	fifo_write8

#define SET_ERROR(x)	{ \
							bset(PORTC, ERR_BIT); \
							fifo_write(ERROR_##x); \
							return; \
						}

// function prototypes

inline void data_out();
inline void data_tri();
inline void data_in();

// must set port mode first
inline byte fifo_read();
inline void fifo_write8(uint8_t b);
inline void fifo_write16(uint16_t p);
inline void fifo_write32(uint32_t p);
inline void fifo_writeptr(void* p, uint16_t count);

inline uint16_t readuint16(byte * buffer, int pos);
inline uint32_t readuint32(byte * buffer, int pos);

inline void fifo_reset();
inline void ff_reset();

inline void do_sleep();

inline void enable_ctrl();
inline void disable_ctrl();

inline void handle();

inline void do_reset();

#endif
#endif
