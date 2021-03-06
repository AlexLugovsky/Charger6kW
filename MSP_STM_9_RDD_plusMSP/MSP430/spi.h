
//-------------------------------------------------------------------
// File: spi.h
// Project: CY CoolMax MPPT
// Device: MSP430F247
// Author: Monte MacDiarmid, Tritium Pty Ltd.
// Description: 
// History:
//   2010-07-27: original
//-------------------------------------------------------------------

#ifndef SPI_H
#define SPI_H

#include "debug.h"

void SPI_init(void);
unsigned char SPI_writeRead( unsigned char byte );


#endif // SPI_H

