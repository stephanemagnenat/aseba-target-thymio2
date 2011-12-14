#include <p24fxxxx.h>
#include <string.h>
#include <stddef.h>
#include <skel-usb.h>
#include <flash/flash.h>
#include <vm/vm.h>
#include <common/consts.h>
#include "log.h"
#include "crc.h"
#include "mode.h"
#include "usb_uart.h"

// Run-time part

#define incs_u8(t) do{if(t != 255) t++;}while(0)

#define CRC_POLY 0x1021
#define CRC_SIZE 16


// Mesured time between two poweroff wakeup:
// 0.552s
// It should be: 512*32/32768 = 0.5s

struct _ud {
	unsigned int poweron_m;
	unsigned int studio_m;
	unsigned int usb_m;
	unsigned char poweron_c;
	unsigned char reprogram_c;
	unsigned char mode_m[MODE_MAX+2];
	unsigned char poweroff_d;

#define FLAG_INTERNAL		(0) // always not set
#define FLAG_BATTERY 		(1)
#define FLAG_ASEBABUG 		(2)
#define FLAG_VMCODESD		(3)
#define FLAG_PLAYBACKSD		(4)
#define FLAG_FLASHVMCODE	(5)
#define FLAG_RECORDSD		(6)
#define FLAG_MOTORUSED		(7)
#define FLAG_IRUSED			(8)
#define FLAG_NTCUSED		(9)
#define FLAG_SOUND			(10)
#define FLAG_LEDIR			(11)
#define FLAG_LEDRGB			(12)
#define FLAG_LEDCIRCLE		(13)
#define FLAG_ACCUSED		(14)
#define FLAG_BUTTONUSED		(15)
#define FLAG_SOUNDTRESH		(16)
#define FLAG_RC5USED		(17)
	unsigned char flags[3];
	
	
	unsigned int crc; // The crc of all the previous values ... must be at the end.
};

#define _set_bit(c,f) do {asm("ior.b %[yy],[%[xx]],[%[xx]]" :: [xx] "r" (&c),[yy] "r"(f) :"cc");}while(0)

// This address is not used by the bootloader.
static struct _ud __attribute((address(0x4800-sizeof(struct _ud)), noload)) ud;

static void set_flag(unsigned char f) {
	unsigned int p = f / 8;
	_set_bit(ud.flags[p], 1 << (f - 8*p));
}

void log_poweroff_tick(void) {
	static long pres;
	if(++pres == 24L*3600*2) {
		pres = 0;
		incs_u8(ud.poweroff_d);
	}
}

void log_prepare_reset(void) {
	crc_init(CRC_SIZE, CRC_POLY, 0);
	crc_process_8(&ud, offsetof(struct _ud, crc));
	ud.crc = crc_finish();
}

static void _tick(void) {
	int t;
	// poweron general counter
	ud.poweron_m++;
	
	// Mode counters
	t = mode_get();
	if(t < 0)
		incs_u8(ud.mode_m[MODE_MAX+1]);
	else
		incs_u8(ud.mode_m[t]);
		
	// Usb 5V present ? 
	if(U1OTGSTATbits.SESVD) {
		// Aseba Present ?
		if(usb_uart_serial_port_open())
			ud.studio_m++;
		else
			ud.usb_m++;
	}	
}

void log_poweron_tick(void) {
	static unsigned int poweron_tick;
	poweron_tick++;
	if(poweron_tick == 60*1000UL) {
		poweron_tick = 0;
		_tick();
	}
}

static int mem_valid(void) {
	crc_init(CRC_SIZE, CRC_POLY, 0);
	crc_process_8(&ud, offsetof(struct _ud, crc));
	if(crc_finish() != ud.crc)
		return 0;
	else
		return 1;
}
#define A(c_name) (offsetof(struct _vmVariables, c_name)/sizeof(sint16))

static void check_store(unsigned int i) {
	switch(i) {
	case A(target[0]):
	case A(target[1]):
		set_flag(FLAG_MOTORUSED);
		break;
		
	case A(sound_tresh):
		set_flag(FLAG_SOUNDTRESH);
		break;
	}
}

static void check_load(unsigned int i) {
	switch(i) {
	case A(buttons_state[0]) ... A(buttons_state[4]):
		set_flag(FLAG_BUTTONUSED);
		break;
		
	case A(prox[0]) ... A(prox[6]):
	case A(ground_ambiant[0]) ... A(ground_ambiant[1]):
	case A(ground_reflected[0]) ... A(ground_reflected[1]):
	case A(ground_delta[0]) ... A(ground_delta[1]):
		set_flag(FLAG_IRUSED);
		break;
		
	case A(acc[0]) ... A(acc[2]):
		set_flag(FLAG_ACCUSED);
		break;
		
	case A(ntc):
		set_flag(FLAG_NTCUSED);
		break;
		
	case A(rc5_address):
	case A(rc5_command):
		set_flag(FLAG_RC5USED);
		break;
		
	case A(sound_level):
		set_flag(FLAG_SOUNDTRESH);
		break;
	}
}
#undef A

void AsebaVMRunCB(AsebaVMState *vm) {
	unsigned int i;
	unsigned int max_start = 0;

	// Event scanning
	if(vm->bytecode[0] > vm->bytecodeSize)
		return; //Invalid bytecode.
		
	for(i = 0; i < vm->bytecode[0]; i+=2) {
		switch(vm->bytecode[i]) {
		case EVENT_B_BACKWARD:
		case EVENT_B_LEFT:
		case EVENT_B_CENTER:
		case EVENT_B_FORWARD:
		case EVENT_B_RIGHT:
		case EVENT_BUTTONS:
			set_flag(FLAG_BUTTONUSED);
			break;
		case EVENT_PROX:
			set_flag(FLAG_IRUSED);
			break;
		case EVENT_TAP:
		case EVENT_ACC:
			set_flag(FLAG_ACCUSED);
			break;
		case EVENT_MIC:
			set_flag(FLAG_SOUNDTRESH);
			break;
		case EVENT_RC5:
			set_flag(FLAG_RC5USED);
			break;
		case EVENT_MOTOR:
			set_flag(FLAG_MOTORUSED);
			break;
		}
		
		if(vm->bytecode[i+1] > max_start)
			max_start = vm->bytecode[i+1];
	}
	
	
	// Now, the fun part: parse the bytecode... 
	while(i < vm->bytecodeSize) {
		switch(vm->bytecode[i] >> 12) {
			case ASEBA_BYTECODE_STOP:
				if(i > max_start)
				// We are at the end of the bytecode.
					return;
				i++;
				break;
			case ASEBA_BYTECODE_CONDITIONAL_BRANCH:
			case ASEBA_BYTECODE_LARGE_IMMEDIATE:
				i+=2;
				break;
			case ASEBA_BYTECODE_LOAD:
				check_load(vm->bytecode[i] & 0xFFF);
				i++;
				break;
			case ASEBA_BYTECODE_STORE:
				check_store(vm->bytecode[i] & 0xFFF);
				i++;
				break;
			case ASEBA_BYTECODE_LOAD_INDIRECT:
				// We give only the base address of the array.
				check_load(vm->bytecode[i] & 0xFFF);
				i+=2;
				break;
			case ASEBA_BYTECODE_STORE_INDIRECT:
				// We give only the base address of the array.
				check_store(vm->bytecode[i] & 0xFFF);
				i+=2;
				break;
			case ASEBA_BYTECODE_JUMP:
			{
				unsigned int t = i + ((int)(vm->bytecode[i] << 4) >> 4);
				if(t > max_start)
					max_start = t;
				i++;
			}
				break;
			case ASEBA_BYTECODE_EMIT:
				check_load(vm->bytecode[i+1]);
				i+=3;
				break;
			case ASEBA_BYTECODE_SUB_CALL:
				if((vm->bytecode[i] & 0xFFF) > max_start)
					max_start = vm->bytecode[i] & 0xFFF;
				i++;
				break;
			default:
				i++;
				break;	
		}
	}
}


// Flash write/read part

#define LOG_FLASH_ADDRESS (FLASH_END - 0xc00 - 0x400 - 0x800)
unsigned char log_flash[INSTRUCTIONS_PER_PAGE * 2*2] __attribute__ ((space(prog), section(".log_flash"), noload, address(LOG_FLASH_ADDRESS)));

#define PAGE_0 LOG_FLASH_ADDRESS
#define PAGE_1 (PAGE_0 + INSTRUCTIONS_PER_PAGE*2)


struct _record {
	unsigned int __attribute((packed)) poweron;
	unsigned int __attribute((packed)) studio;
	unsigned int __attribute((packed)) usb;
	unsigned char __attribute((packed)) flags[3];
	unsigned char __attribute((packed)) switchon;
	unsigned char __attribute((packed)) reprogram;
	unsigned char __attribute((packed)) mmenu;
	unsigned char __attribute((packed)) mfollow;
	unsigned char __attribute((packed)) mexplorer;
	unsigned char __attribute((packed)) macc;
	unsigned char __attribute((packed)) mline;
	unsigned char __attribute((packed)) mrc5;
	unsigned char __attribute((packed)) msound;
	unsigned char __attribute((packed)) mvm;
	unsigned char __attribute((packed)) poweroff;
	unsigned char __attribute((packed)) _;
}; // Sizeof(_record) == 21 == 7 instructions

struct _header {
	unsigned int __attribute((packed)) version;	// binary format version.
	unsigned int __attribute((packed)) switchon; // number of time it has been switched on
	unsigned long __attribute((packed)) poweron;  // Poweron time in minutes
	unsigned int __attribute((packed)) studio; // Studio use time in minutes
	unsigned int __attribute((packed)) usb; // usb-non-studio time in minutes
	unsigned int __attribute((packed)) reprogram; // Number of time it has been reprogramed
	unsigned int __attribute((packed)) mmenu;	// mode time, in minutes
	unsigned int __attribute((packed)) mfollow; // ''
	unsigned int __attribute((packed)) mexplorer;// ''
	unsigned int __attribute((packed)) macc; 	// ''
	unsigned int __attribute((packed)) mline; 	// ''
	unsigned int __attribute((packed)) mrc5; 	// ''
	unsigned int __attribute((packed)) msound;	// ''
	unsigned int __attribute((packed)) mvm;		// ''
	unsigned int __attribute((packed)) poweroff;// poweroff time in days.
	unsigned char __attribute((packed)) flags[3]; // flags, or-ed
	unsigned char __attribute((packed)) _[10]; 	// padding, can be used for something else.
}; // sizeof(_header) == 45 == 15 instruction

#define HEADER_VERSION 1

#define HEADER_SIZE (sizeof(struct _header))
#define RECORD_SIZE (sizeof(struct _record))

#define HEADER_FLASH_SIZE ((HEADER_SIZE / 3)*2)
#define RECORD_FLASH_SIZE ((RECORD_SIZE / 3)*2)

// Number of record in one page: (512-15)/7 = 71

static void sum_stats(struct _header * h, unsigned long source) {
	unsigned long i;
	struct _record r;
	flash_read_chunk(source, HEADER_SIZE, (unsigned char *) &h); // Prefill with last sum
	
	for(i = source + HEADER_FLASH_SIZE; 
		i < source + INSTRUCTIONS_PER_PAGE*2; i += RECORD_FLASH_SIZE) {
			
		flash_read_chunk(i,RECORD_SIZE,(unsigned char *) &r);
		
		h->flags[0] |= r.flags[0];
		h->flags[1] |= r.flags[1];
		h->flags[2] |= r.flags[2];
		
		h->poweroff += r.poweroff;
		h->mvm += r.mvm;
		h->msound += r.msound;
		h->mrc5 += r.mrc5; 
		h->mline += r.mline;
		h->macc += r.macc;
		h->mexplorer += r.mexplorer;
		h->mfollow += r.mfollow;
		h->mmenu += r.mmenu;
		h->reprogram += r.reprogram;
		h->usb += r.usb;
		h->studio += r.studio;
		h->poweron += r.poweron;
		h->switchon += r.switchon;
	}
}


static void init_page(unsigned long target, unsigned long source) {
	struct _header h;
	unsigned long data;
	int i;
	
	if(!source)
		memset(&h, 0, HEADER_SIZE);
	else {
		sum_stats(&h,source);
	}
	h.version = HEADER_VERSION; // Force header version
	
	flash_erase_page(target);
	
	for(i = 0; i < HEADER_SIZE; i+=3) {
		memcpy(&data, ((char *) &h) + i, 3);
		flash_flash_instr(target, data);
		target += 2;
	}
}

static int is_initialized(unsigned long a) {
	// Is the flash un-initialized ? 
	struct _header h;	
	flash_read_chunk(a, HEADER_SIZE,(unsigned char *) &h);
	if(h.version == HEADER_VERSION)
		return 1;
	return 0;
}
	
static unsigned long _get_next_free(unsigned long addr) {
	// Return the next free entry on specific page.
	// Return 0 if none found.
	unsigned long i;
	
	for(i = addr + HEADER_FLASH_SIZE; 
			i < addr + INSTRUCTIONS_PER_PAGE * 2; i += RECORD_FLASH_SIZE) {
		struct _record r;
		flash_read_chunk(i, RECORD_SIZE, (unsigned char *) &r);
		if(r.flags[0] & FLAG_INTERNAL)
			return i;
	}
	
	return 0;
}

static unsigned long get_next_free(void) {
	unsigned long record_addr;

	if(!is_initialized(PAGE_0)) {
		if(!is_initialized(PAGE_1)) {
			init_page(PAGE_0,0);
			record_addr = PAGE_0 + HEADER_FLASH_SIZE;
		} else {
			record_addr = _get_next_free(PAGE_1);
			if(!record_addr) {
				// Page 1 full, start writing page 0
				init_page(PAGE_0,PAGE_1);
				record_addr = PAGE_0 + HEADER_FLASH_SIZE;
			}
		}
	} else {
		record_addr = _get_next_free(PAGE_0);
		if(!record_addr) {
			// Page 0 full
			if(!is_initialized(PAGE_1)) {
				init_page(PAGE_1,PAGE_0);
				record_addr = PAGE_1 + HEADER_FLASH_SIZE;
			} else {
				record_addr = _get_next_free(PAGE_1);
				if(!record_addr) {
					// Error, both page are full .. erase one ..
					init_page(PAGE_0, PAGE_1);
					record_addr = PAGE_0 + HEADER_FLASH_SIZE;
				}
			}
		}
	}
	
	return record_addr;
}

static int write_record(void) {
	unsigned long addr;
	unsigned long data;
	struct _record r;
	int i;

	// First wait until we have a valid vbat
	vmVariables.vbat[0] = 0;
	while(!vmVariables.vbat[0]) barrier();
	
	// Vbat < 3.3V, don't flash
	if(vmVariables.vbat[0] < 656)
		return 0;
	
	addr = get_next_free();
	
	// Fill the record data
	r.poweron = ud.poweron_m;
	r.studio = ud.studio_m;
	r.usb = ud.usb_m;
	r.flags[0] = ud.flags[0];
	r.flags[1] = ud.flags[1];
	r.flags[2] = ud.flags[2];
	r.switchon = ud.poweron_c;
	r.reprogram = ud.reprogram_c;
	r.mmenu = ud.mode_m[MODE_MENU];
	r.mfollow = ud.mode_m[MODE_FOLLOW];
	r.mexplorer = ud.mode_m[MODE_EXPLORER];
	r.macc = ud.mode_m[MODE_ACC];
	r.mline = ud.mode_m[MODE_LINE];
	r.mrc5 = ud.mode_m[MODE_RC5];
	r.msound = ud.mode_m[MODE_SOUND];
	r.mvm = ud.mode_m[MODE_MAX+1];
	r.poweroff = ud.poweroff_d;
	
	// TODO: Check that any code running for now support that the cpu clock can be halted for several ms.
	// Write the record into flash
	for(i = 0; i < RECORD_SIZE; i+=3) {
		memcpy(&data, ((char *) &r) + i, 3);
		flash_flash_instr(addr, data);
		addr += 2;
	}
	
	// If this is the last record from the page, prepare the next page
	if(addr == PAGE_1)
		init_page(PAGE_1, PAGE_0);
	if(addr == PAGE_1 + INSTRUCTIONS_PER_PAGE * 2)
		init_page(PAGE_0, PAGE_1);
		
	return 1;
}	

void log_init(void) {
	if(!mem_valid())
 		memset(&ud,0,sizeof(ud));
 		
 	if(_BOR) {
		set_flag(FLAG_BATTERY);
	 	_BOR = 0;
	 }
	 
	 if(ud.poweroff_d || ud.poweron_m > 60*24) {
		 // If more than 1 day since previous record
	 	if(write_record())
		 	// Reset the statistics
		 	memset(&ud,0,sizeof(ud));
	 }
	 
	 incs_u8(ud.poweron_c);
}

