/*
 *
 *  BIOS ROM Startup Assembler
 *  (C)2002 Andy, Michael, Paul, Steve
 * Original top and bottom ROM code by Steve from an idea by Michael
 * -- NOTE: Comment removed, the top / bottom Code changed to turnaround code.
 */

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/
/* 
	Rewritten from Original .bin linking to compiler system by Lehner Franz (franz@caos.at) 
	Rewritten to Dual Boot concept for 2BL loading
	New written CPU Inits by Lehner Franz (franz@caos.at) 
	Written New Working Xcodes + Xcode compiler by Lehner Franz (franz@caos.at) 
	Focus support by Lehner Franz (franz@caos.at) 
	Xcalibur support by Lehner Franz (franz@caos.at) 
*/
 
 
#define xcode_peek(val1) .byte 0x2; .long val1 ; .long 0x0 ;
#define xcode_poke(val1,val2) .byte 0x3; .long val1 ; .long val2 ;
#define xcode_pciout(val1,val2) .byte 0x4; .long val1 ; .long val2 ;
#define xcode_pciin_a(val1) .byte 0x5; .long val1 ; .long 0x0 ;

#define xcode_bittoggle(val1,val2) .byte 0x6; .long val1 ; .long val2 ;

#define xcode_ifgoto(val1,val2) .byte 0x8; .long val1 ; .long (9*(val2-1)) ;
#define xcode_ifgoto_label(val1,val2) .byte 0x8; .long val1 ; .long (val2-.-4) ;

#define xcode_outb(val1,val2) .byte 0x11; .long val1 ; .long val2 ;			    
#define xcode_inb(val1) .byte 0x12; .long val1 ; .long 0x0 ;			    

#define xcode_poke_a(val1) .byte 0x7; .long 0x3; .long val1 ;  
#define xcode_pciout_a(val1) .byte 0x7; .long 0x4; .long val1 ;  
#define xcode_outb_a(val1) .byte 0x7; .long 0x11; .long val1 ;  

#define xcode_goto(val1) .byte 0x9; .long 0x0; .long (9*(val1-1));
#define xcode_goto_label(val1) .byte 0x9; .long 0x0; .long (val1-.-4);

#define xcode_END(val1) .byte 0xEE; .long val1 ; .long 0x0;

#define SMBUS 		0x0000c000

#define SMBUS_READ_RETRY -4

#define SMB_xcode_Write(val1,val2); \
			xcode_outb(SMBUS+8, val1); \
			xcode_outb(SMBUS+6, val2); \
			xcode_outb(SMBUS+2, 0x0000000a); \
			xcode_inb(SMBUS); \
			xcode_ifgoto(0x00000010,-1); \
			xcode_outb(SMBUS, 0x00000010); 

.code32

.section .low_rom, "ax"

.org 0x0
	/* MCPX Magic Values - clock timings*/
	
	.long 0xff000009 //SmartXX sets this one to 0xff000008 but was originally 0xff000009. SpiderGX has this set to 0xff000003
	.long 0xff000008
	.long 0x2b16d065
	.long 0x3346322d
	.long 0x01010101
	.long 0x08080808
	.long 0x00000801

	.long 0xc8fc7c8a
	.long 0x44290213
	.long 0x90004998		
	.long 0x00000000

	.long 0xffffffff
	.long 0xffffffff
	
	.org 0x40
	.long _start_checksum	// This number will be overwritten  
									// With imagebld, but we need a temp value
	
	.org 0x6c
	.long 0x00000107
	
	.org 0x70		// MCPX Config Area
	.long 0x0000000f
	.long 0x40004400
	
	.long 0x12d10070
	.long 0x00000c90

	
	//The bytecode interpreter begins here
xcode_pciout(0x80000884, 0x00008001);
xcode_pciout(0x80000810, 0x00008001);
xcode_pciout(0x80000804, 0x00000003);
xcode_outb(0x00008049, 0x00000008);
xcode_outb(0x000080d9, 0x00000000);
xcode_outb(0x00008026, 0x00000001);
xcode_pciout(0x8000f04c, 0x00000001);
xcode_pciout(0x8000f018, 0x00010100);
xcode_pciout(0x80000084, 0x07ffffff);

xcode_pciout(0x8000f020, 0x0ff00f00);
xcode_pciout(0x8000f024, 0xf7f0f000);
xcode_pciout(0x80010010, 0x0f000000);
xcode_pciout(0x80010014, 0xf0000000);
xcode_pciout(0x80010004, 0x00000007);
xcode_pciout(0x8000f004, 0x00000007);
// Set up the SM - bus controller
xcode_pciout(0x80000904, 0x00000001);
xcode_pciout(0x80000914, 0x0000c001);
xcode_pciout(0x80000918, 0x0000c201);
xcode_goto_label(common_init);
//xcode_goto_label(xcalibur_init);	//Not very useful! Probably leftover from debugging...

common_init:
xcode_poke(0x0f0010b0, 0x07633461);
xcode_poke(0x0f0010cc, 0x66660000);
xcode_peek(0x0f101000);
xcode_bittoggle(0x000c0000, 0x00000000);
xcode_ifgoto_label(0x00000000, common_init_alt1);
xcode_peek(0x0f101000);
xcode_bittoggle(0xe1f3ffff, 0x80000000);
xcode_poke_a(0x0f101000);
xcode_poke(0x0f0010b8, 0xeeee0000);
xcode_goto_label(common_init_ending);

common_init_alt1:
xcode_ifgoto_label(0x000c0000, common_init_alt2);
xcode_peek(0x0f101000);
xcode_bittoggle(0xe1f3ffff, 0x860c0000);
xcode_poke_a(0x0f101000);
xcode_poke(0x0f0010b8, 0xffff0000);
xcode_goto_label(common_init_ending);

common_init_alt2:
xcode_peek(0x0f101000);
xcode_bittoggle(0xe1f3ffff, 0x820c0000);
xcode_poke_a(0x0f101000);
xcode_poke(0x0f0010b8, 0x11110000);

common_init_ending:								//Ultimately, all of the above code merge back to here.
xcode_poke(0x0f0010d4, 0x00000009);
xcode_poke(0x0f0010b4, 0x00000000);
xcode_poke(0x0f0010bc, 0x00005866);
xcode_poke(0x0f0010c4, 0x0351c858);
xcode_poke(0x0f0010c8, 0x30007d67);
xcode_poke(0x0f0010d8, 0x00000000);
xcode_poke(0x0f0010dc, 0xa0423635);
xcode_poke(0x0f0010e8, 0x0c6558c6);
xcode_poke(0x0f100200, 0x03070103);
xcode_poke(0x0f100410, 0x11000016);
xcode_poke(0x0f100330, 0x84848888);
xcode_poke(0x0f10032c, 0xffffcfff);
xcode_poke(0x0f100328, 0x00000001);
xcode_poke(0x0f100338, 0x000000df);
xcode_outb(0x0000c200, 0x00000070);

//VIDEO INIT CODE
/* It is necessary to write to the video encoder, as the PIC
 snoops the I2C traffic and will reset us if it doesn't see what
 it judges as 'appropriate' traffic.  Conexant is the most urgent,
 as on 1.0 Xboxes, the PIC was very strict and reset us earlier
 than later models
 */
 //This is focus...
xcode_outb(SMBUS+4, 0x000000d4);
xcode_outb(SMBUS+8, 0x0000000c);
xcode_outb(SMBUS+6, 0x00000000);
xcode_outb(SMBUS+2, 0x0000000a);

xcode_inb(SMBUS);
xcode_ifgoto(0x00000010, 0x00000002);
xcode_goto_label(focus_init_part2);						//Success!
xcode_bittoggle(0x00000008, 0x00000000); //Check fail status
xcode_ifgoto(0x00000000, SMBUS_READ_RETRY);//No result yet, read again.
xcode_outb(SMBUS, 0x000000ff);
xcode_goto_label(connexant_init);			//Abandon focus, go to connexant

focus_init_part2:						//focus phase 2
xcode_outb(SMBUS, 0x00000010);
SMB_xcode_Write(0x0000000d, 0x00000020);
xcode_goto_label(focus_init_part3);		//Oops, missed that one.

connexant_init:			//Connexant init attempt.
xcode_outb(SMBUS, 0x00000010);
xcode_outb(SMBUS+4, 0x0000008a);	//This is the address for connexant.
xcode_outb(SMBUS+8, 0x000000ba);
xcode_outb(SMBUS+6, 0x0000003f);
xcode_outb(SMBUS+2, 0x0000000a);
xcode_inb(SMBUS);
xcode_ifgoto(0x00000010, 0x00000002); //Write failed, forward 2
xcode_goto_label(connexant_init_part2);						//Success!
xcode_bittoggle(0x00000008, 0x00000000);//Check fail status
xcode_ifgoto(0x00000000, SMBUS_READ_RETRY);//No result yet, read again.
xcode_outb(SMBUS, 0x000000ff);			//Abandon connexant.
xcode_outb(SMBUS, 0x00000010);

//xcode_outb(SMBUS+4, 0x000000e0);
//SMB_xcode_Write(0x00000000, 0x00000000);
//SMB_xcode_Write(0x000000b8, 0x00000000);
xcode_goto_label(xcalibur_init);				//to 1.6 init?

connexant_init_part2:				//Connexant phase 2
xcode_outb(SMBUS, 0x00000010);
SMB_xcode_Write(0x0000006c, 0x00000046);
SMB_xcode_Write(0x000000b8, 0x00000000);
SMB_xcode_Write(0x000000ce, 0x00000019);
SMB_xcode_Write(0x000000c6, 0x0000009c);
SMB_xcode_Write(0x00000032, 0x00000008);
SMB_xcode_Write(0x000000c4, 0x00000001);

//VIDEND - Encoder 'init' complete.

// PIC SLAVE Address (Write)
focus_init_part3:	//Connexant, focus common phase 3.
xcode_outb(SMBUS+4, 0x00000020);
// I2Ctransmit(0x20,0x1,0x0);
SMB_xcode_Write(0x00000001, 0x00000000);

// PIC SLAVE Address (Read)
xcode_outb(SMBUS+4, 0x00000021);

// I2Cgetbyte(0x8a,0x1);
xcode_outb(SMBUS+8, 0x00000001);
xcode_outb(SMBUS+2, 0x0000000a);
xcode_inb(SMBUS);
xcode_ifgoto(0x00000010, -1);	//retry
xcode_outb(SMBUS, 0x00000010);

// If SMC version does not match ... ?????
xcode_inb(SMBUS+6);

//Not going to add markers in this section. Pointless since self-contained.
xcode_poke(0x0f680500, 0x00011c01);
xcode_poke(0x0f68050c, 0x000a0400);
xcode_poke(0x0f001220, 0x00000000);
xcode_poke(0x0f001228, 0x00000000);
xcode_poke(0x0f001264, 0x00000000);
xcode_poke(0x0f001210, 0x00000010);
xcode_peek(0x0f101000);
xcode_bittoggle(0x06000000, 0x00000000);
xcode_ifgoto(0x00000000, 0x00000004);
xcode_poke(0x0f001214, 0x48480848);
xcode_poke(0x0f00122c, 0x88888888);
xcode_goto(0x00000007);
xcode_ifgoto(0x06000000, 0x00000004);
xcode_poke(0x0f001214, 0x09090909);
xcode_poke(0x0f00122c, 0xaaaaaaaa);
xcode_goto(0x00000003);
xcode_poke(0x0f001214, 0x09090909);
xcode_poke(0x0f00122c, 0xaaaaaaaa);
xcode_poke(0x0f001230, 0xffffffff);
xcode_poke(0x0f001234, 0xaaaaaaaa);
xcode_poke(0x0f001238, 0xaaaaaaaa);
xcode_poke(0x0f00123c, 0x8b8b8b8b);
xcode_poke(0x0f001240, 0xffffffff);
xcode_poke(0x0f001244, 0x8b8b8b8b);
xcode_poke(0x0f001248, 0x8b8b8b8b);
xcode_poke(0x0f1002d4, 0x00000001);
xcode_poke(0x0f1002c4, 0x00100042);
xcode_poke(0x0f1002cc, 0x00100042);
xcode_poke(0x0f1002c0, 0x00000011);
xcode_poke(0x0f1002c8, 0x00000011);
xcode_poke(0x0f1002c0, 0x00000032);
xcode_poke(0x0f1002c8, 0x00000032);
xcode_poke(0x0f1002c0, 0x00000132);
xcode_poke(0x0f1002c8, 0x00000132);
xcode_poke(0x0f1002d0, 0x00000001);
xcode_poke(0x0f1002d0, 0x00000001);
xcode_poke(0x0f100210, 0x80000000);
xcode_poke(0x0f00124c, 0xaa8baa8b);
xcode_poke(0x0f001250, 0x0000aa8b);
xcode_poke(0x0f100228, 0x081205ff);
xcode_poke(0x0f001218, 0x00010000);
xcode_pciin_a(0x80000860);
xcode_bittoggle(0xffffffff, 0x00000400);
xcode_pciout_a(0x80000860);
xcode_pciout(0x8000084c, 0x0000fdde);
xcode_pciout(0x8000089c, 0x871cc707);
xcode_pciin_a(0x800008b4);
xcode_bittoggle(0xfffff0ff, 0x00000f00);
xcode_pciout_a(0x800008b4);
xcode_pciout(0x80000340, 0xf0f0c0c0);
xcode_pciout(0x80000344, 0x00c00000);
xcode_pciout(0x8000035c, 0x04070000);
xcode_goto_label(endExec);

xcalibur_init:
xcode_outb(0x0000c200, 0x00000070);
xcode_poke(0x0f0010b0, 0x01000010);
xcode_poke(0x0f0010cc, 0x66660000);
xcode_goto_label(xcalibur_init_part2);

label13:
xcode_poke(0x0f0010d4, 0x0000000f);
xcode_poke(0x0f0010b4, 0x00000000);
xcode_poke(0x0f0010bc, 0x00005860);
xcode_poke(0x0f0010c4, 0xaaaa0000);
xcode_poke(0x0f0010c8, 0x00007d67);
xcode_poke(0x0f0010d8, 0x00000000);
xcode_poke(0x0f0010dc, 0x00000000);
xcode_poke(0x0f0010e8, 0x04000000);
xcode_poke(0x0f100200, 0x03070103);
xcode_poke(0x0f100410, 0x11000016);
xcode_poke(0x0f100330, 0x84848888);
xcode_poke(0x0f10032c, 0xffffcfff);
xcode_poke(0x0f100328, 0x00000001);
xcode_poke(0x0f100338, 0x000000df);
xcode_outb(SMBUS+4, 0x00000020);
SMB_xcode_Write(0x00000001, 0x00000000);
xcode_outb(SMBUS+4, 0x00000021);
xcode_outb(SMBUS+8, 0x00000001);
xcode_outb(SMBUS+2, 0x0000000a);
xcode_inb(SMBUS);
xcode_ifgoto(0x00000010, -1);
xcode_outb(SMBUS, 0x00000010);
xcode_inb(SMBUS+6);
xcode_ifgoto(0x00000050, 0x00000002);
xcode_goto(0x00000002);
xcode_pciout(0x8000036c, 0x01000000);
xcode_poke(0x0f680500, 0x00011c01);
xcode_poke(0x0f68050c, 0x000a0400);
xcode_poke(0x0f001220, 0x00000000);
xcode_poke(0x0f001228, 0x00000000);
xcode_poke(0x0f001264, 0x00000000);
xcode_poke(0x0f001210, 0x00000000);
xcode_goto_label(xcalibur_init_part3);

xcalibur_init_part2:
xcode_pciout(0x8000183c, 0x000000c3);

xcalibur_init_part3:
xcode_outb(SMBUS+4, 0x000000a9);
xcode_pciin_a(0x8000183c);
xcode_outb_a(0x0000c008);
xcode_outb(SMBUS, 0x00000000);
xcode_outb(SMBUS+2, 0x0000000a);
xcode_inb(SMBUS);
xcode_pciout_a(0x8000103c);
xcode_bittoggle(0x00000008, 0x00000000);
xcode_ifgoto(0x00000008, 0x00000002);
xcode_goto(SMBUS_READ_RETRY);
xcode_pciin_a(0x8000103c);
xcode_bittoggle(0x000000ff, 0x00000000);
xcode_ifgoto_label(0x00000010, xcalibur_init_part3);
xcode_inb(SMBUS+6);
xcode_bittoggle(0x000000ff, 0x00000000);
xcode_poke_a(0x0f001248);
xcode_pciin_a(0x8000183c);
xcode_bittoggle(0x000000ff, 0x00000000);
xcode_ifgoto_label(0x000000c3, label11);
xcode_peek(0x0f001248);
xcode_ifgoto(0x00000042, 0x00000004);
xcode_peek(0x0f101000);
xcode_bittoggle(0xe1f3ffff, 0x800c0000);
xcode_goto_label(label12);
xcode_ifgoto(0x00000040, 0x00000004);
xcode_peek(0x0f101000);
xcode_bittoggle(0xe1f3ffff, 0x80080000);
xcode_goto_label(label12);
xcode_ifgoto(0x00000002, 0x00000004);
xcode_peek(0x0f101000);
xcode_bittoggle(0xe1f3ffff, 0x80040000);
xcode_goto_label(label12);
xcode_peek(0x0f101000);
xcode_bittoggle(0xe1f3ffff, 0x80000000);

label12:
xcode_poke_a(0x0f101000);
xcode_pciout(0x8000183c, 0x000000c2);
xcode_goto_label(xcalibur_init_part3);

label11:
xcode_peek(0x0f001248);
xcode_bittoggle(0x00000001, 0x00000000);
xcode_ifgoto_label(0x00000000, label14);
xcode_peek(0x0f001248);

label14_2:
	xcode_bittoggle(0x00000002,0x00000000);
	xcode_ifgoto_label(0x00000000,label15)
	xcode_peek(0x0f001248);

label15_2:
	xcode_bittoggle(0x00000004,0x00000000);
	xcode_ifgoto_label(0x00000000,label16)
	xcode_peek(0x0f001248);

label16_2:
	xcode_bittoggle(0x00000008,0x00000000);
	xcode_ifgoto_label(0x00000000,label17)
	xcode_peek(0x0f001248);

label17_2:
	xcode_bittoggle(0x00000010,0x00000000);
	xcode_ifgoto_label(0x00000000,label18)
	xcode_peek(0x0f001248);

label18_2:
	xcode_bittoggle(0x00000020,0x00000000);
	xcode_ifgoto_label(0x00000000,label19)
	xcode_peek(0x0f001248);

label19_2:
	xcode_bittoggle(0x00000040,0x00000000);
	xcode_ifgoto_label(0x00000000,label20)
	xcode_peek(0x0f001248);

label20_2:
	xcode_bittoggle(0x00000080,0x00000000);
	xcode_ifgoto_label(0x00000000,label21)

label21_2:

	xcode_pciin_a(0x8000183C);
	xcode_bittoggle(0x000000ff,0x00000000);

	xcode_ifgoto_label(0x000000c2,label22)

	xcode_peek(0x0f001248);
	xcode_bittoggle(0xffff0000,0x00000000);
	xcode_poke_a(0x0f0010b8);
	xcode_pciout(0x8000183C, 0x000000c1);

	xcode_goto_label(label13)

label22:

	xcode_pciin_a(0x8000183C);
	xcode_bittoggle(0x000000ff,0x00000000);
	xcode_ifgoto_label(0x000000c1,label23)
	xcode_peek(0x0f001248);
	xcode_poke_a(0x0f00122c);
	xcode_pciout(0x8000183C, 0x000000c0);
	xcode_goto_label(xcalibur_init_part3)

label23:

	xcode_peek(0x0f001248);
	xcode_poke_a(0x0f001214);

	xcode_pciout(0x8000183C, 0x000000c3);
	xcode_goto_label(extendedEndExec)

label14:
	xcode_peek(0x0f001248);
	xcode_bittoggle(0xffffffff,0x01010100);
	xcode_poke_a(0x0f001248);
	xcode_goto_label(label14_2);

label15:
	xcode_peek(0x0f001248);
	xcode_bittoggle(0xffffffff,0x02020200);
	xcode_poke_a(0x0f001248);
	xcode_goto_label(label15_2);

label16:
	xcode_peek(0x0f001248);
	xcode_bittoggle(0xffffffff,0x04040400);
	xcode_poke_a(0x0f001248);
	xcode_goto_label(label16_2);

label17:
	xcode_peek(0x0f001248);
	xcode_bittoggle(0xffffffff,0x08080800);
	xcode_poke_a(0x0f001248);
	xcode_goto_label(label17_2);

label18:
	xcode_peek(0x0f001248);
	xcode_bittoggle(0xffffffff,0x10101000);
	xcode_poke_a(0x0f001248);
	xcode_goto_label(label18_2);

label19:
	xcode_peek(0x0f001248);
	xcode_bittoggle(0xffffffff,0x20202000);
	xcode_poke_a(0x0f001248);
	xcode_goto_label(label19_2);

label20:
	xcode_peek(0x0f001248);
	xcode_bittoggle(0xffffffff,0x40404000);
	xcode_poke_a(0x0f001248);
	xcode_goto_label(label20_2);

label21:
	xcode_peek(0x0f001248);
	xcode_bittoggle(0xffffffff,0x80808000);
	xcode_poke_a(0x0f001248);
	xcode_goto_label(label21_2);


extendedEndExec:

	xcode_poke(0x0f001248, 0x00000000);
	xcode_poke(0x0f001230, 0xffffffff);
	xcode_poke(0x0f001234, 0xaaaaaaaa);
	xcode_poke(0x0f001238, 0xaaaaaaaa);
	xcode_poke(0x0f00123c, 0x8b8b8b8b);
	xcode_poke(0x0f001240, 0xffffffff);
	xcode_poke(0x0f001244, 0x8b8b8b8b);
	xcode_poke(0x0f001248, 0x8b8b8b8b);
	xcode_poke(0x0f1002d4, 0x00000001);
	xcode_poke(0x0f1002c4, 0x00100000);
	xcode_poke(0x0f1002cc, 0x00100000);
	xcode_poke(0x0f1002c0, 0x00000011);
	xcode_poke(0x0f1002c8, 0x00000011);
	xcode_poke(0x0f1002c0, 0x00000032);
	xcode_poke(0x0f1002c8, 0x00000032);
	xcode_poke(0x0f1002c0, 0x00000132);
	xcode_poke(0x0f1002c8, 0x00000132);
	xcode_poke(0x0f1002d0, 0x00000001);
	xcode_poke(0x0f1002d0, 0x00000001);
	xcode_poke(0x0f100210, 0x80000000);
	xcode_poke(0x0f00124c, 0xaa8baa8b);
	xcode_poke(0x0f001250, 0x00005474);
	xcode_poke(0x0f100228, 0x081205ff);
	xcode_poke(0x0f001218, 0x00000000);

	xcode_pciin_a(0x80000860);
	xcode_bittoggle(0xffffffff,0x00000400);
	xcode_pciout_a(0x80000860);


	xcode_pciout(0x8000084c, 0x0000fdde);
	xcode_pciout(0x8000089c, 0x871cc707);
	xcode_pciin_a(0x800008b4);
	xcode_bittoggle(0xfffff0ff,0x00000f00);
	xcode_pciout_a(0x800008b4);
	xcode_pciout(0x80000340, 0xf0f0c0c0);
	xcode_pciout(0x80000344, 0x00000000);
	xcode_pciout(0x8000035c, 0x00000000);

endExec:		//There is no going back now.
xcode_pciout(0x8000036c, 0x00230801);
xcode_pciout(0x8000036c, 0x01230801);
xcode_goto(1);
xcode_goto(1);
xcode_poke(0x0f100200, 0x03070103);
xcode_poke(0x0f100204, 0x11448000);
xcode_pciout(0x8000103c, 0x00000000);
xcode_pciout(0x8000183c, 0x00000000);
xcode_outb(SMBUS, 0x00000010);

/* ----  Report Memory Size to PIC scratch register ---- */

// We emulate Good memory result to PIC
//xcode_pciin_a(0x8000183c);
//xcode_bittoggle(0x000000ff,0x00000000);
//xcode_outb_a(SMBUS+6);

xcode_outb(SMBUS+4, 0x00000020);

SMB_xcode_Write(0x00000013, 0x0000000f);
SMB_xcode_Write(0x00000012, 0x000000f0);

/* ---- Reload Nvidia Registers  ------------------------*/

xcode_pciout(0x8000f020, 0xfdf0fd00);
xcode_pciout(0x80010010, 0xfd000000);

// overflow trick
xcode_poke(0x00000000, 0x000ED0ea);	//SmartXX uses bottom bank from flash. Make sure you change this in linker script too.
xcode_poke(0x00000004, 0x000008ff);
xcode_END(0x00000806);


	// Note: never change this from offset 0x1000 ....
	// This is the Main Entry point ....        
	.org 0xED0	//Could be 0xECF
        
	// Good Morning CPU

	// NOTE:
	// We come here form the high rom section Jump
	
	// Clear Intel Interrupts in Processor Register
	// Everytime very good, specially when comming out of a running envoronment
		
	movl	$0x1b, %ecx
	xor	%eax, %eax
	xor	%edx, %edx
	wrmsr
        
	// Interrupts now Dead

	xor	%eax, %eax
	xor	%edx, %edx
	xor	%ecx, %ecx
	
	// kill the cache  = Disable bit 30 + 29 = CD + NW 
	// CD = Cache Disable (disable = 1)
	// NW Not write through (disable = 1)
	mov 	%cr0, %eax
	orl	$0x60000000, %eax
	mov	%eax, %cr0
	wbinvd

	// We clear the cr3 register
	mov	%eax, %eax
	mov	%eax, %cr3

	// Clear Memory Type register
	movl	$0x2ff, %ecx
	xor	%eax, %eax
	xor	%edx, %edx
	wrmsr

	//We are setting the Ram Init's now to set up the Regions in the Ram


	// MTRR for RAM
	// from address 0, Writeback Caching, 128MB range
	movl	$0x200, %ecx
	movl	$0x00000000, %edx
	movl	$0x00000006, %eax 	// == WB_CACHE == 6
	//movl	$0x00000004, %eax 	// Temporary, as USB development
	wrmsr

	// MASK0 set to 0xf8000[000] == 128M
        inc %ecx //movl	$0x201, %ecx
	movl	$0x0000000f, %edx
	movl	$0xf8000800, %eax
	wrmsr

	// MTRR for shadow RAM
	// from address 0xf0000000, Write-combining Caching, 128MB range
        inc %ecx //movl	$0x202, %ecx
	movl	$0x00000000, %edx
	movl	$0xf0000001, %eax 	// Write-Combining == 1
	wrmsr

	// MASK0 set to 0xf8000[000] == 128M
        inc %ecx //movl	$0x203, %ecx
	movl	$0x0000000f, %edx
	movl	$0xf8000800, %eax
	wrmsr

	// MTRR for FLASH
	inc %ecx //movl	$0x204, %ecx
	movl	$0x00000000, %edx
	movl	$0xFf000000, %eax 	    // We set to Uncacheable
	wrmsr

    inc %ecx //movl	$0x205, %ecx
	movl	$0x0000000f, %edx
	movl	$0xff000800, %eax
	wrmsr
	

	xor	%eax, %eax
	xor	%edx, %edx

	inc %ecx //movl	$0x206, %ecx		// IA32_MTRR_PHYS Base 3
	wrmsr
	inc %ecx //movl	$0x207, %ecx		// IA32_MTRR_PHYS_MASK 3
	wrmsr

	inc %ecx //movl	$0x208, %ecx		// IA32_MTRR_PHYS Base 4
	wrmsr                                                   
	inc %ecx //movl	$0x209, %ecx		// IA32_MTRR_PHYS_MASK 4
	wrmsr                                                   

	inc %ecx //movl	$0x20a, %ecx		// IA32_MTRR_PHYS Base 5
	wrmsr                                                   
	inc %ecx //movl	$0x20b, %ecx		// IA32_MTRR_PHYS_MASK 5
	wrmsr                                                   

	inc %ecx //movl	$0x20c, %ecx		// IA32_MTRR_PHYS Base 6
	wrmsr                                                   
	inc %ecx //movl	$0x20d, %ecx		// IA32_MTRR_PHYS_MASK 6
	wrmsr

	inc %ecx //movl	$0x20e, %ecx		// IA32_MTRR_PHYS Base 7
	wrmsr
	inc %ecx //movl	$0x20f, %ecx		// IA32_MTRR_PHYS_MASK 7
	wrmsr                                                   
                                              

	// Define Memory in IA32_MTRR_DEF_TYPE
	movl	$0x2ff, %ecx
	xor	%edx, %edx
	movl	$0x800, %eax //Enable MTRRs
	wrmsr

	/* turn on normal cache */
	//  bit 30 + 29 = CD + NW 
	// CD = Cache Disable (disable = 1)
	// NW Not write through (disable = 1)
	movl	%cr0, %eax
	mov 	%eax, %ebx
	andl	$0x9FFFFFFF,%eax
	movl	%eax, %cr0

	cld

	// copy everything into RAM 

	mov 	$_ram_location, %edi
	mov 	$_start_ramcopy, %esi
	mov 	$(_size_ramcopy + 6), %ecx
	shr	$2,%ecx    // We dividy / 4, as we copy Dword oriented
	rep 	movsl
	
	jmp   initaftermemcpy


	// We leave Flash at this point, we never come back to it anymore.
	// the following ASM instructions below, already are linked to memory 
	// (look to the ldscript in this case)

	.section .text, "ax"
                         
	// Note: We are in Memory here, not in Flash anylonger, 
	// we have been copy'd here and linked for memory, as we reside in section .text
	// Look to LDscript
       
	.global	MemoryChecksum
MemoryChecksum:      	
	// The SHA-1 Hashsum is dumped here after with ImageBLD
	.byte 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	.byte 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	.long _size_2bl
	
	.org 0x19  // We have linkbase for this programm = 0x100000 + 0x40 = divisable /4 .. so CPU likes it
tableGdt:

	.byte 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 // 0x00 dummy
	.byte 0xff, 0xff, 0x00, 0x00, 0x00, 0x9b, 0xcf, 0x00 // 0x08 code32
	.byte 0xff, 0xff, 0x00, 0x00, 0x00, 0x9b, 0xcf, 0x00 // 0x10 code32
	.byte 0xff, 0xff, 0x00, 0x00, 0x00, 0x93, 0xcf, 0x00 // 0x18 data32
	.byte 0xff, 0xff, 0x00, 0x00, 0x00, 0x9b, 0x8f, 0x00 // 0x20 code16 (8f indicates 4K granularity, ie, huge limit)
	.byte 0xff, 0xff, 0x00, 0x00, 0x00, 0x93, 0x8f, 0x00 // 0x28 data16
	.byte 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 // Dummy
	
tableGdtDescriptor:

	.word 0x30
	.long tableGdt
	.word 0x0       // fill Word, so we get alliged again

tableIdtDescriptor:

	.word 2048
	.long 0x400000
	.word 0x0	// fill Word, so we get alliged again
		
initaftermemcpy:
	// These Init Codes Can be found in the 2'nd Bootloader

	// We set up a GDT now, this is not necessary needed, 
	// but it is preferred, as we have more ability with commands
        
	// We clear the IDT in RAM
	xor %eax,%eax
    // Below is equivalent to memset(0x400000, 0, 0x5000/4);
	mov $0x5000,%ecx
	mov $0x400000,%edi
	rep stosl
   	
   	//Clear decompressed kernel space.
   	// memset((void*)0x03800000,0x0,0x30000)
#if 0
   	#include "memory_layout.h"
	movl $0x30000, %ecx
	movl $CODE_LOC_START, %edi
	rep stosl
#endif
	// We'll do that one PIC challenge has been resolved.
	// Fix issue for 1.0 strict timing.

	lidt tableIdtDescriptor
	lgdt tableGdtDescriptor
	// set up selectors for everything

	xor	%eax, %eax
	lldt %ax

	// Reload CS as 0010 from the new GDT using a far jump
	.byte 0xEA          // jmp far 0010:reload_cs
	.long reload_cs
	.word 0x0010
	.align 16

reload_cs:

	// CS is now a valid entry in the GDT.  Set SS, DS, and ES to valid
	// descriptors, but clear FS and GS as they are not necessary.

	// Set SS, DS, and ES to a data32 segment with maximum limit.
	movw $0x0018, %ax
	mov %eax, %ss
	mov %eax, %ds
	mov %eax, %es

	// Clear FS and GS
	xor %eax, %eax
	mov %eax, %fs
	mov %eax, %gs
	
	movl	$ 0x1ffff0,%esp

	mov	$0x8, %al
	mov	$0x61, %dx
	out	%al, %dx

	// Enable IDE and NIC
	mov 	$0x8000088C, %eax 
	movw 	$0xcf8, %dx 
	outl	%eax, %dx 
	movw 	$0xcfc, %dx 
	movl 	$0x40000000, %eax 
	outl	%eax, %dx
      	
	// CPU Whoami   ? sesless ?
	mov 	$0x80000080, %eax 
	movw 	$0xcf8, %dx 
	outl	%eax, %dx 
	movw 	$0xcfc, %dx 
	movl 	$0x100, %eax 
	outl	%eax, %dx 
        
	// this can be found in 2bBootStartBios.c
	jmp 	BootStartBiosLoader
