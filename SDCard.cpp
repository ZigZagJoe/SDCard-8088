#include <avr/sleep.h>
#include <avr/interrupt.h>
#include <avr/wdt.h>
#include <util/crc16.h>

#include <md5.h>
#include <zzjduino.h>
#include <SdFat.h>
#include <SdFatUtil.h>

#define SDCARD_CPP
#include "SDCard.h"

#define SERIAL_DEBUG 1

// global variables for instruction handling
uint16_t dlen;
uint8_t inst;
uint8_t buffer[BUFFER_SIZE];

// state variables
bool canUseSD = false;
uint8_t currDir = 0;

SdFat sdFat;

SdFile openFile;	// working file

// used for wake-up on status write - EMPTY_INTERRUPT generates reti and nothing else
// SIGNAL(INT0_vect) would generate a prologue and epilogue
EMPTY_INTERRUPT(INT0_vect);

// soft reset
SIGNAL(INT1_vect) {
	do_reset();
}

int main(void) {
	MCUSR = 0; // required for wdt_disable to actually work
	wdt_disable();
	
	// set up output port
	PORTC = bits(REG_CS, LED, IOW, IOR, FF_RESET, FIFO_RESET);
	DDRC = bits(REG_CS, ERR_BIT, LED, IOW, IOR, FF_RESET, FIFO_RESET);
	
	// turn on SPI
	SPCR = bits(SPE0, MSTR0);
	
	// set up interrupt 0 & 1
	// int1 on rising edge (ISCn1 = 1, ISCn0 = 1)
	// int0 on falling edge (ISCn1 = 1, ISCn0 = 0);
	EICRA |= bits(ISC11, ISC01, ISC00); 
	EIMSK |= bits(INT1); // enable interrupt 1 (soft reset)
	
	// start millis timer
	millis_start();
	
	sei(); // interrupts on
	set_sleep_mode(SLEEP_MODE_STANDBY);
	
#ifdef SERIAL_DEBUG
	Serial.begin(38400);
	Serial.print(F("Free RAM: "));
	Serial.println(FreeRam());
#endif
	
	canUseSD = sdFat.begin(-1 /*chip select - not used*/, SPI_FULL_SPEED);
	
    //root.openRoot(&volume); // open root directory
	
	if (!canUseSD)
		bset(PORTC, ERR_BIT);
	
	volatile uint32_t t; // weird behavior without volatile...
	
	delay(10);
	
	bclr(PORTC, LED);
	disable_ctrl();
	fifo_reset();
	
	while(true) {
		ff_reset();
		t = 0;
		while (!bisset(PIND, Q))  // wait until the flip-flop is set
			if (++t >= 1700000) { // around 2.5 seconds
				
				// millis will not wake us as only timer2 can wake from sleep
				bset(EIMSK, INT0);// enable wake interrupt
				do_sleep();		  // sleep until interrupt
				bclr(EIMSK, INT0);// disable wake interrupt
			
				t = 0;			  // one more loop, to make sure ff is set
			}
		
		Serial.println("Go time");
		// OK, flip-flop is set, time to do stuff
		bclr(PORTC, ERR_BIT);
		bset(PORTC, LED);
		
		enable_ctrl();
		data_in();
		
		// read the instruction byte from the register
		// could move the clr to be the first thing and remove nops
		// but this is better for the sake of readability
		bclr(PORTC, REG_CS);
		_NOP;
		_NOP;
		inst = PINA;
		bset(PORTC, REG_CS);
		
#ifdef SERIAL_DEBUG
		Serial.print("Command: ");
		Serial.println(inst);
#endif
		
		// if fifo is not empty, read its contents out
		if (bisset(PIND, EMPTY)) {
			dlen = -1;
			
			// read in the entire fifo contents
			while (bisset(PIND, EMPTY)) { // ~empty INACTIVE
				bclr(PORTC, IOR);		
				dlen++;				// do something productive while waiting for AVR sync circuit
				_NOP;
				buffer[dlen] = PINA;// read byte in
				bset(PORTC, IOR);
			}
			
			dlen ++; // dlen is now the count of data bytes
		} else 
			dlen = 0;
			
#ifdef SERIAL_DEBUG
		Serial.print("Data bytes: ");
		Serial.println(dlen);
#endif
		
		data_tri();
		fifo_reset(); // superfluous, really...
		data_out();
		
		// handle the instruction
		handle();

		// clean up, tristate everything shared
		data_tri();
		disable_ctrl();	
		bclr(PORTC, LED);
	}
	
	return 0;
}

inline bool containsFilename(void* b, uint16_t dlen) {
	byte * inb  = (byte*)b;
	for (uint8_t i = 0; i < 13 && i < dlen; i++)
		if (inb[i] == 0 && i > 0) 
			return true;
	
	return false;
}


FUNC_HANDLER(OPEN) {
	if (fileOpen)
		SET_ERROR(FILE_ALREADY_OPEN);
	
	char* filename = (char*)buffer + 1;
	
	if (!containsFilename(filename,dlen)) 
		SET_ERROR(BAD_ARGUMENT);
	
	if (!openFile.open(filename, buffer[0]))
		SET_ERROR(FAILED_TO_OPEN);
}

FUNC_HANDLER(CLOSE) {
	if (fileOpen) {
		openFile.sync();
		openFile.close();
	}
}
 
FUNC_HANDLER(READ) {
	if (!fileOpen) 
		SET_ERROR(FILE_NOT_OPEN);
	
	uint16_t req = READ_MAX_SZ;
	
	if (dlen >= 2) {
		req = readuint16(buffer, 0);
		if (req > READ_MAX_SZ)
			req = READ_MAX_SZ;
	}
	
	int16_t rd = openFile.read(buffer, req);
	
	if (rd < 0) {
#ifdef SERIAL_DEBUG
		Serial.print(F("READ ERROR: "));
		Serial.println(sdFat.card()->errorCode());
#endif
		SET_ERROR(READ_ERROR);
	} else {
		fifo_write16(rd);
		fifo_writeptr(buffer, rd);
	}
}

FUNC_HANDLER(WRITE) {
	if(!fileOpen) 
		SET_ERROR(FILE_NOT_OPEN);
	
	if (!dlen)
		return;
	
	uint16_t wr = openFile.write(buffer, dlen);
	
	if (!wr)
		SET_ERROR(WRITE_ERROR);
	
	fifo_write16(wr);
 
    /*uint16_t crc = 0;
    
    for (int i = 0; i < dlen; i++)
        crc = _crc16_update(crc,buffer[i]);
    
    fifo_write16(crc);*/
}


FUNC_HANDLER(SEEK) {
	if (!fileOpen)
		SET_ERROR(FILE_NOT_OPEN);
	
	if (dlen < 4) 
		SET_ERROR(BAD_ARGUMENT);
	
	uint32_t s = readuint32(buffer, 0);
	
	if(!openFile.seekSet(s))
		SET_ERROR(OPERATION_FAILED);
}

FUNC_HANDLER(SEEKREL) {
	if (!fileOpen)
		SET_ERROR(FILE_NOT_OPEN);
	
	if (dlen < 4) 
		SET_ERROR(BAD_ARGUMENT);
	
	int32_t s = (int32_t)readuint32(buffer, 0);
	
	if(!openFile.seekSet(openFile.curPosition() + s))
		SET_ERROR(OPERATION_FAILED);
}

FUNC_HANDLER(LENGTH) {
	if (!fileOpen)
		SET_ERROR(FILE_NOT_OPEN);
	
	fifo_write32(openFile.fileSize());
}

FUNC_HANDLER(POSITION) { 
	if (!fileOpen)
		SET_ERROR(FILE_NOT_OPEN);
	
	fifo_write32(openFile.curPosition());
}

///////////////////////////////////////////////////////////////////////////////

FUNC_HANDLER(FILE_MD5) {

	SdFile md5File;
	
	if (!containsFilename(buffer, dlen))
		SET_ERROR(BAD_ARGUMENT);
	
	if(!md5File.open((char*)buffer, O_RDONLY)) 
		SET_ERROR(FAILED_TO_OPEN);

	int16_t rd = 0;
    
	md5_state_t state;
	uint8_t digest[16];
	
	md5_init(&state);
	
	while ((rd = md5File.read(buffer, BUFFER_SIZE)) > 0)
		md5_append(&state, buffer, rd);
	
	if (rd < 0) {
#ifdef SERIAL_DEBUG
		Serial.print(F("READ ERROR: "));
		Serial.println(sdFat.card()->errorCode());
#endif
		md5File.close();
		SET_ERROR(READ_ERROR);
	}

	md5_finish(&state, digest);
	
	fifo_write32(md5File.fileSize());
	fifo_writeptr(digest, 16);
	
	md5File.close();
}

FUNC_HANDLER(CHDIR) {
	if (!containsFilename(buffer, dlen)) 
		SET_ERROR(BAD_ARGUMENT);
	
	if (((buffer[0] == '/') || (buffer[0] == '\\')) && (buffer[1] == 0)) {
		sdFat.chdir(true); // return to root
	} else {
		if (!sdFat.chdir((char*)buffer, true))
			SET_ERROR(INVALID_DIR);
	}
}

FUNC_HANDLER(DIR) {
	dir_t p;
	
	int skip = 0;
	int c = 0;
	int8_t code;
	
	byte page = 0;
	
	if (dlen) {
		page = buffer[0] + 1;
		skip = page * 30;
	}
	
	fifo_write(page);	
	
	sdFat.vwd()->rewind();	
	
	while (((code = sdFat.vwd()->readDir(&p)) > 0) && c < FILES_PER_DIR_PAGE) {
		if (skip && skip--)
			continue;
		
		// done if past last used entry
		if (p.name[0] == DIR_NAME_FREE) 
			break;
		
		// skip deleted entry and entries for . and  ..
		if (p.name[0] == DIR_NAME_DELETED || p.name[0] == '.') 
			continue;
		
		// only list subdirectories and files
		if (!DIR_IS_FILE_OR_SUBDIR(&p)) 
			continue;
		
		fifo_writeptr(&p.name, 11);
		
		if (!DIR_IS_SUBDIR(&p)) {
			fifo_write32(p.fileSize);
		} else 
			fifo_write32(0xFFFFFFFF);
		
		c++;
	}
	
	if (c < FILES_PER_DIR_PAGE)
		fifo_write(DIR_NO_MORE_FILES);
}

FUNC_HANDLER(BENCH_READ) {
	SdFile benchFile;
	
	if (!benchFile.open("bench.txt", OPEN_READ))
		SET_ERROR(FAILED_TO_OPEN);
	
	int16_t rd = 0;
	uint32_t bytesTotal = 0;
	
	while ((rd = benchFile.read(buffer, BUFFER_SIZE)) > 0)
		bytesTotal += rd;
	
	if (rd < 0) {
#ifdef SERIAL_DEBUG
		Serial.print(F("READ ERROR: "));
		Serial.println(sdFat.card()->errorCode());
#endif
		benchFile.close();
		SET_ERROR(READ_ERROR);
	}
	
	fifo_write32(benchFile.fileSize());
	
	benchFile.close();
}

FUNC_HANDLER(BENCH_WRITE) {
	SdFile benchFile;
	
	if (!benchFile.open("bench.txt",OPEN_WRITE))
		SET_ERROR(FAILED_TO_OPEN);
		
	uint32_t bytesTotal = 0;
	size_t wr;
	
	for (int n = 0; n < 2000; n++) {
		for (int i = 0; i < BUFFER_SIZE; i++) 
			buffer[i] = i;
		
		wr = benchFile.write(buffer, BUFFER_SIZE);
		
		if (!wr)
			break;
		
		bytesTotal += wr;
	}
	
	if (!wr) {
#ifdef SERIAL_DEBUG
		Serial.println(F("WRITE ERROR"));
#endif
		benchFile.close();
		SET_ERROR(WRITE_ERROR);
	}
	
	benchFile.close();
	
	fifo_write32(bytesTotal);
}

FUNC_HANDLER(EXISTS) {
	if (!containsFilename(buffer, dlen)) 
		SET_ERROR(BAD_ARGUMENT);
	
	SdFile child;
	
	bool exists = child.open((char*) buffer, O_RDONLY);
	
	if (exists) {
		child.close(); 
	} else 
		SET_ERROR(NONEXISTANT_FILE);
}

FUNC_HANDLER(DELETE) {
	for (uint8_t i = 0; i < 13 && i < dlen; i++)
		if (buffer[i] == 0 && i > 0) {
			if (buffer[i+1] == 0xDE) {
				if (!sdFat.remove((char*)buffer))
					SET_ERROR(OPERATION_FAILED);
				return;
			}
			break;
		}
	
	SET_ERROR(BAD_ARGUMENT);
}

//////////////////////////////////////////////////////////////

inline void handle() {
	if (bisset(PORTD, SW)) // card not present, disable all SD operations
		canUseSD = false;	
	
	switch (inst) {
		case CRCTEST: {// crc16 of data
				uint16_t chksm = 0;
				
				for (uint16_t i = 0; i < dlen; i++)
					chksm = _crc16_update(chksm, buffer[i]);
				
				fifo_write16(dlen);
				fifo_write16(chksm);
				return;
			}
		case HELLO: // simple hello response - returns 0xDEADBEEF 
			fifo_write(0xDE);
			fifo_write(0xAD);
			fifo_write(0xBE);
			fifo_write(0xEF);
			return;
		case ECHO: // simply returns all the data that was in fifo - basic integrity testing
			for (uint16_t i = 0; i < dlen; i++)
				fifo_write(buffer[i]);

			return;
	}
	
	// if initialization failed, card is missing, or was missing in the past
	if (!canUseSD) // reset is required
		SET_ERROR(SD_NOT_PRESENT);
	
	switch (inst) {
			CASE_HANDLER(EXISTS);
			CASE_HANDLER(DIR);
			CASE_HANDLER(CHDIR);
			CASE_HANDLER(DELETE);
			
			CASE_HANDLER(FILE_MD5);
			CASE_HANDLER(BENCH_READ);
			CASE_HANDLER(BENCH_WRITE);
			
			CASE_HANDLER(OPEN);
			CASE_HANDLER(CLOSE);
			
			CASE_HANDLER(LENGTH);
			CASE_HANDLER(POSITION);
			CASE_HANDLER(SEEK);
			CASE_HANDLER(SEEKREL);
			CASE_HANDLER(READ);
			CASE_HANDLER(WRITE);
	}
	
	SET_ERROR(UNKNOWN_INSTRUCTION);
}

inline uint16_t readuint16(byte * buffer, int pos) {
	return *((uint16_t*)(buffer + pos));
}

inline uint32_t readuint32(byte * buffer, int pos) {
	return *((uint32_t*)(buffer + pos));
}

inline void fifo_write32(uint32_t p) {
	fifo_write16(p);
	fifo_write16(p >> 16);
}

inline void fifo_write16(uint16_t p) {
	fifo_write8(p);
	fifo_write8(p >> 8);
}

inline void fifo_write8(uint8_t b) {
	PORTA = b;
	bclr(PORTC,IOW);
	bset(PORTC,IOW);
}

inline void fifo_writeptr(void* p, uint16_t count) {
	byte * ptr = (byte*)p;
	for (uint16_t i = 0; i < count; i++)
		fifo_write(ptr[i]);
}

inline void do_sleep() {
	sleep_enable();
	sei();
	sleep_cpu();
	sleep_disable();
}

inline void do_reset() {
	data_tri();
	disable_ctrl();
	wdt_enable(WDTO_15MS);
	while(true) ;
}

inline void disable_ctrl() {
	DDRC &= ~(bv(IOW) | bv(IOR));
	bclr(PORTC, IOW); // set these after changing input direction so lines are not
	bclr(PORTC, IOR); // set low at any point in time
}

inline void enable_ctrl() {
	bset(PORTC, IOW); // set port first so that the lines are not pulled low 
	bset(PORTC, IOR); // when output direction is changed
	DDRC |= bv(IOW) | bv(IOR);
}

inline void ff_reset() {
	bclr(PORTC, FF_RESET);
	bset(PORTC, FF_RESET);	
}

inline void fifo_reset() {
	bclr(PORTC, FIFO_RESET);
	bset(PORTC, FIFO_RESET);
}

inline byte fifo_read() {
	byte tmp;
	
	bclr(PORTC, IOR);
	_NOP; _NOP; // wait for valid data - AVR sync circuit (1.5 cycles)
	tmp = PINA;
	bset(PORTC, IOR);
	
	return tmp;
}

inline void data_out() {
	DDRA = 0xFF; // all input 
}

inline void data_in() {
	data_tri();
	_NOP;  // syncronization
	_NOP;
}

inline void data_tri() {
	DDRA = 0;  // input mode
	PORTA = 0; // all pull ups off
}
