/*
 * mb_main.c
 *
 *  Created on: 03/09/2013
 *      Author: Bruno Luiz
 */

#include "mb_main.h"

Uint16 rx_frame[MB_FRAME_CHAR_TOTALS] = {0};
Uint16 rx_frame_pointer = 0;
Uint16 tx_frame[MB_FRAME_CHAR_TOTALS] = {0};
Uint16 tx_frame_pointer = 0;
Uint32 modbus_usTimerT35 = 0;
Uint16 modbus_state = MB_STATE_T35;

/*
 * Initialize the MODBUS
 * - ulBaudRate: select the preferred baudRaute
 * - ucDataBits: number of bytes which MODBUS will transmit (common: 8 bits)
 * - eParity: the type of parity used (check the mb_main.h - it will have constants for it - the common is NONE)
 */
void modbus_init(Uint32 ulBaudRate, Uint16 ucDataBits, Uint16 eParity )
{

	// Allows some modifications on protected registers
	EALLOW;
	DINT;

	// Initialize serial - check the mb_serial for more info
	serial_init(ulBaudRate, ucDataBits, eParity);
	// Defined timing for the 3.5char time
	modbus_usTimerT35 = ( 7UL * 220000UL ) / ( 2UL * ulBaudRate );
	// Turns off read and transmission of serial (will be reenabled on timer_init)
	serial_interrupt_switch(0, 0);
	// Initialize the timer - it starts the receiver
	timer_init(modbus_usTimerT35);

	EDIS;
	EINT;
	ERTM;
}

/*
 * MODBUS STATES
 * - It will start at MB_STATE_T35, enabling the receiver and going to WAITING state
 * - When it is waiting for data it will be at STATE_WAITING (which does nothing)
 * - When it changes to STATE_READING it will prepare the next state (SEND) and will enable the transmitter
 * - When it is transmitting, it will be at STATE_T35_SEND. At the end, it will turn of the receiver
 * and transmitter and will go to STATE_T35
 */
void modbus_chk_states()
{
    if(modbus_state == MB_STATE_T35)
    {
    	serial_interrupt_switch(1,0);
    	modbus_state = MB_STATE_WAITING;
    }
    else if(modbus_state == MB_STATE_WAITING)
	{
		;
	}
    else if(modbus_state == MB_STATE_READING)
    {
    	modbus_state = MB_STATE_T35_SEND;
    	reset_data_pointers();
    	serial_interrupt_switch(0,1);
    }
    else if(modbus_state == MB_STATE_T35_SEND)
    {
    	serial_send_data();
    	serial_interrupt_switch(0,0);
    	modbus_state = MB_STATE_T35;
    }
}

/*
 * Prepares the data to send
 * - Check if the contents from rx_frame are valid (function code and slave ID)
 * - Based on function code it sends it to read_func or write_func
 * - These *_func will process the data and will return an 0 or any number (which will be the size of the tx_frame)
 * - If the value is different from 0 it will transmit the data (check the serial_send_data from mb_serial.c)
 * - Otherwise it would not transmit anything
 */
Uint16 modbus_prep_response()
{
	// Disable receiver, enable transmitter
	serial_interrupt_switch(0,1);

	// Clear the tx_frame
	clear_tx_frame();

	// Check if is the sent ID is the same of configured for the device
	if(rx_frame[0] == MB_SLAVE_ID)
	{
		//Get Slave ID
		tx_frame[0] = rx_frame[0];

		//Get Function Code
		tx_frame[1] = rx_frame[1];

		//Check if function code is for read data or write data
		if(rx_frame[1] == MB_FUNC_READ_HOLDINGREGISTERS || rx_frame[1] == MB_FUNC_READ_INPUTREGISTERS)
			return modbus_read_func();
		else if(rx_frame[1] == MB_FUNC_WRITE_REGISTER)
			return modbus_write_func();
		else
			return modbus_error(MB_ERROR_ILLEGALFUNC);

	} else return 0;
}

/* Read the demanded data from the registers */
Uint16 modbus_read_func(){
	Uint16 data_address = 0;		// Stores the start address where the data will be put
	Uint16 crc = 0;					// Stores CRC code

	Uint16 number_of_data = 0;		// Stores the number of data which have to be got
	Uint16 frame_lenght = 0;		// Stores the frame length (it is variable)
	Uint16 i = 0, j = 0, register_pos = 0; 		// Iterators

	Uint32 mem_map = memory_map(rx_frame[1]);	// Gets the address of memory where are the registers we want

	//Get the start address
	data_address = rx_frame[2] << 8;
	data_address = data_address|rx_frame[3];
	data_address = data_address|mem_map;

	//Get the number of data to get
	number_of_data = rx_frame[4] << 8;
	number_of_data = number_of_data|rx_frame[5];

	// As it uses FIFO, it needs to specify how many data will be placed at FIFO.
	// The default will consider one register, but this function recalculate for more registers (up to 6)
	serial_tx_frame_calc(number_of_data);

	//Number of bytes to follows
	tx_frame[2] = (Uint16)(number_of_data*(Uint16)2);

	//Getting the data, based on the number of bytes required (specified at tx_frame[2])
	while(i < tx_frame[2])
	{
		// Points to the start data address + iterator
		Uint32 *addr = (Uint32 *)(data_address+i/2);
		// Get the value from this data address
		Uint32 addr_val = *addr;

		// Iterates through all 8 bit data and put it the tx_frame
		for(j = 0; j < 4; j++){
			tx_frame[3+(4*register_pos)+j] = (addr_val >> j*8) & 0x00FF;
		}
		// Swap the values (it will be inverted, so it will revert)
		swap_values(&tx_frame[3+(4*register_pos)],&tx_frame[3+(4*register_pos)+1]);
		swap_values(&tx_frame[3+(4*register_pos)+2],&tx_frame[3+(4*register_pos)+3]);

		// Increment the iterators
		i+=4;
		register_pos++;
	}

	// Actual frame length (counting the data)
	frame_lenght = 3 + tx_frame[2];

	// Generate CRC code
	crc = generate_crc(tx_frame, frame_lenght);

	// Put the CRC code at tx_frame
	tx_frame[frame_lenght+1] 		= (crc & 0xFF00) >> 8;
	tx_frame[frame_lenght] 	= crc & 0x00FF;

	frame_lenght += 2; // Adding to the size of frame length the 2 bytes of CRC
	return frame_lenght;
}

/* Write the received data to the registers */
Uint16 modbus_write_func(){
	Uint16 data = 0;				// Stores the sent data
	Uint16 data_address = 0;		// Stores the start address where the data will be put
	Uint16 crc = 0;					// Stores CRC code
	Uint32 *addr = 0;				// Points the address where it will be written
	Uint32 mem_map = memory_map(rx_frame[1]);	// Gets the address of memory where are the registers we want

	//Get the start address
	data_address = rx_frame[2] << 8;
	data_address = data_address|rx_frame[3];
	data_address = data_address|mem_map;
	addr = (Uint32 *)data_address;

	// Transfer the start address to TX frame
	tx_frame[2] = rx_frame[2];
	tx_frame[3] = rx_frame[3];

	if(rx_frame[1] == MB_FUNC_WRITE_REGISTER)
	{
		// Get the data to be written
		data = rx_frame[4] << 8;
		data = data|rx_frame[5];

		// Set the data
		*(addr) = data;

		// Transfer the data to TX frame
		tx_frame[4] = (*(addr) & 0xFF00) >> 8;
		tx_frame[5] = *(addr) & 0x00FF;
	}
	else if(rx_frame[1] == MB_FUNC_WRITE_NREGISTERS){
		Uint16 i = 0;
		Uint16 number_of_data = 0;

		//Get the number of registers/coils to write
		number_of_data = rx_frame[4] << 8;
		number_of_data = number_of_data|rx_frame[5];

		// Transfer the number of registers to TX frame
		tx_frame[4] = rx_frame[4];
		tx_frame[5] = rx_frame[5];

		// Writing the data
		// At f28335, a memory address points to a 32 bits registers. So address 0x8000
		// and 0x8001 will point to the same data. To write at 0x8001 you'll need to set
		// the high data of this 32 bits register (the last 16 bits)
		while(i < number_of_data)
		{
			Uint32 Hdata, Ldata;
			Uint32 address = data_address + i;
			addr = (Uint32 *)address;

			// Take the low data (first register)
			Ldata = rx_frame[7 + i*2];
			Ldata = (Ldata << 8*1) | rx_frame[7 + i*2 + 1];

			// Take the high data (second register)
			Hdata = rx_frame[7 + i*2 + 2];
			Hdata = (Hdata << 8) | rx_frame[7 + i*2 + 3];

			// Add both and put at the register:
			// ==> If the quantity of registers is odd (1, for example), then maintain the high data of the last
			// addressed register.
			// ==> These address registers holds the data of two 16 bits registers (0x8000 and 0x8001, for example),
			// so if the quantity is odd the last register high data don�t have to be written (0x8001 value will be
			// maintained)
			if(rx_frame[5] == (i+1))
				*(addr) = (*(addr) & 0xFFFF0000) | Ldata;
			// Else overwrite everything
			else
				*(addr) = (Hdata << 16) | Ldata;

			i+=2;
		}
	}

	// Generate CRC code
	crc = generate_crc(tx_frame, 6);

	tx_frame[7] = (crc & 0xFF00) >> 8;
	tx_frame[6] = crc & 0x00FF;

	return 8;
}

/* Generate an MODBUS Error message, depending on specified type (see the mb_main.h for the
*  constants which make the types of errors */
Uint16 modbus_error(Uint16 type){
	Uint32 crc;

	tx_frame[2] = type;
	crc = generate_crc(tx_frame, 3);

	tx_frame[4] = (crc & 0xFF00) >> 8;
	tx_frame[3] = crc & 0x00FF;

	return 5;
}

/* Generate the CRC code for the inserted buffer. The data (variable "buf") can have any
 * length, but it has to be specified at variable "len" */
Uint16 generate_crc(Uint16 buf[], int len)
{
  Uint16 crc = 0xFFFF;
  int pos = 0;
  int i = 0;

  for (pos = 0; pos < len; pos++) {
	  // XOR byte into least sig. byte of crc
	  crc ^= (Uint16) buf[pos];

	  // Loop over each bit
	  for (i = 8; i != 0; i--) {

		  // If the LSB is set
		  if ((crc & 0x0001) != 0) {
			  crc >>= 1; 		// Shift right and XOR 0xA001
			  crc ^= 0xA001;
		  }
		  else
			  crc >>= 1;		// Just shift right
    }
  }
  // Note, this number has low and high bytes swapped, so use it accordingly (or swap bytes)
  return crc;
}


Uint32 memory_map(Uint16 type){
	// The addresses 0x8000 to 0x10000 are RAM
	// The addresses 0x0000 to 0x00800 are RAM too, but they are too limited
	if(type == MB_FUNC_READ_COIL || type == MB_FUNC_FORCE_COIL || type == MB_FUNC_FORCE_NCOILS){
		return 0x00000;
	}
	else if(type == MB_FUNC_READ_INPUT){
		return 0x00000;
	}
	else if(type == MB_FUNC_READ_HOLDINGREGISTERS || type == MB_FUNC_WRITE_REGISTER || type == MB_FUNC_WRITE_NREGISTERS){
		return 0x08000;
	}
	else if(type == MB_FUNC_READ_INPUTREGISTERS){
		return 0x08000;
	}
	else
		return 0x0000;
}

/*
 * Resets the data pointers for rx_frame and tx_frame
 */
void reset_data_pointers(){
	rx_frame_pointer = 0;
	tx_frame_pointer = 0;
}

/*
 * Clears the rx_frame, by zeroing all data
 */
void clear_rx_frame(){
	int i;
	for(i = 0; i < MB_FRAME_CHAR_TOTALS; i++)
		rx_frame[i] = 0;
}

/*
 * Clears the tx_frame, by zeroing all data
 */
void clear_tx_frame(){
	// Clears TX frame
	int i;
	for(i = 0; i < MB_FRAME_CHAR_TOTALS; i++)
		tx_frame[i] = 0;
}

/*
 * Swap values from va1 to val2 and val2 to val1
 */
void swap_values(Uint16 *val1, Uint16 *val2){
	Uint16 temp = *val1;
	*val1 = *val2;
	*val2 = temp;
}
