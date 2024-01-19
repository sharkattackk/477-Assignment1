#include <iostream>
#include <iomanip>
#include <string>
#include <cstring>
#include <ctype.h>


#include "dumpHex.hpp"

using namespace std;

ostream &operator << (ostream&s,HexDump h){
   dumpHex(s,h.v, h.l, h.p);
   return s;
}


//+
// Function: Dump Hex
//
// Purpose:
//    This function dumps a pointer to memory and length
// to a stream in both hex and printable ascii. It prints
// 16 bytes per line in groups of 4 (the ascii is not grouped).
//
// Parameters:
//	outs - the output stream to which to send the data
//	b - pointer to the location in memory
//	len - the number of bytes to write.
//      p  - The position of the byte to write in red
//		default is -1 to not print any byte in red
//-

void dumpHex(ostream &outs, const uint8_t *b, uint32_t len, uint32_t p){
    
    int pos = 0;

    // dump whole lines (16 chars per line)
    while (pos + 15 < len){
	// print each entire line

	// firrst print 16 hex digits in groups of 4
	outs << hex << setfill('0') << setw(2) << right << pos;
	for (int i = 0;  i < 16; i++){
	    if ((i % 4) == 0)
		outs << ' ';
	    if (pos+i == p){
		outs << "\033[31m" << hex << setfill('0') << setw(2) << (uint16_t) b[pos + i] << "\033[0m";
	    } else {
		outs << hex << setfill('0') << setw(2) << (uint16_t) b[pos + i];
	    }
	}

	// also print as characters 
	outs << ' ';
	for (int i = 0;  i < 16; i++){
	    if (isprint(b[pos+i])){
		outs << b[pos+i];
	    } else {
		outs << '.';
	    }
        }
	// done the line, go to the next one.
	pos += 16;
	outs << endl;
    }

    if (pos < len){
    	// partial last line
	outs << hex << setfill('0') << setw(2) << right << pos;

	// print last line. partial number of groups, partial number of chars

	// save position in memory to back up for char output
	int savepos = pos;

	// preint partial hex
	while(pos < len){
	    if ((pos % 4) == 0)
		outs << ' ';
	    if (pos == p){
		outs << "\033[31m" << hex << setfill('0') << setw(2) << (uint16_t) b[pos] << "\033[0m";
	    } else {
		outs << hex << setfill('0') << setw(2) << (uint16_t) b[pos];
	    }
	    pos++;
	}

	// backup for char version of partial line
	pos = savepos;

	// calculate number of spaces to align with character output of previous lines
        int numextra = 32 - (len - pos)*2;
	numextra += (numextra+1) / 8 + 1; // number of groups (8 printing chars per group 2 hex per byte)
	outs << setfill(' ') << setw(numextra) << ' ' ;

	// print character version of partial line
	while(pos < len){
	    if (isprint(b[pos])){
		outs << b[pos];
	    } else {
		outs << '.';
	    }
	    pos++;
	}

	// done partial last line.
	outs << endl;
    }

    // print end pos
    outs << hex << setfill('0') << setw(2) << right << pos << endl;
}
