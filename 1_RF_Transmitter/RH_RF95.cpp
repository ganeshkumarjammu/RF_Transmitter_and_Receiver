// RH_RF95.cpp
//
// Copyright (C) 2011 Mike McCauley
// $Id: RH_RF95.cpp,v 1.27 2020/07/05 08:52:21 mikem Exp $

#include "RH_RF95.h"

// Maybe a mutex for multithreading on Raspberry Pi?
#ifdef RH_USE_MUTEX
RH_DECLARE_MUTEX(lock);
#endif

// Interrupt vectors for the 3 Arduino interrupt pins
// Each interrupt can be handled by a different instance of RH_RF95, allowing you to have
// 2 or more LORAs per Arduino
RH_RF95* RH_RF95::_deviceForInterrupt[RH_RF95_NUM_INTERRUPTS] = {0, 0, 0};

// These are indexed by the values of ModemConfigChoice
// Stored in flash (program) memory to save SRAM
PROGMEM static const RH_RF95::ModemConfig MODEM_CONFIG_TABLE[] =
{
    //  1d,     1e,      26
    { 0x72,   0x74,    0x04}, // Bw125Cr45Sf128 (the chip default), AGC enabled
    { 0x92,   0x74,    0x04}, // Bw500Cr45Sf128, AGC enabled
    { 0x48,   0x94,    0x04}, // Bw31_25Cr48Sf512, AGC enabled
    { 0x78,   0xc4,    0x0c}, // Bw125Cr48Sf4096, AGC enabled
    { 0x72,   0xb4,    0x04}, // Bw125Cr45Sf2048, AGC enabled
    
};

RH_RF95::RH_RF95(uint8_t slaveSelectPin, uint8_t interruptPin, RHGenericSPI& spi)
    :
    RHSPIDriver(slaveSelectPin, spi),
    _rxBufValid(0)
{
    _interruptPin = interruptPin;
    _myInterruptIndex = 0xff; // Not allocated yet
    _enableCRC = true;
    _useRFO = false;
}

bool RH_RF95::init()
{
    if (!RHSPIDriver::init())
	return false;

#ifdef RH_USE_MUTEX
    if (RH_MUTEX_INIT(lock) != 0)
    { 
    	Serial.println("\n mutex init has failed\n");
    	return false;
    }
#endif

    if (!setupInterruptHandler())
	return false;

    // No way to check the device type :-(
    
    // Set sleep mode, so we can also set LORA mode:
    spiWrite(RH_RF95_REG_01_OP_MODE, RH_RF95_MODE_SLEEP | RH_RF95_LONG_RANGE_MODE);
    delay(10); // Wait for sleep mode to take over from say, CAD
    // Check we are in sleep mode, with LORA set
    if (spiRead(RH_RF95_REG_01_OP_MODE) != (RH_RF95_MODE_SLEEP | RH_RF95_LONG_RANGE_MODE))
    {
//	Serial.println(spiRead(RH_RF95_REG_01_OP_MODE), HEX);
	return false; // No device present?
    }

    
    // Set up FIFO
    // We configure so that we can use the entire 256 byte FIFO for either receive
    // or transmit, but not both at the same time
    spiWrite(RH_RF95_REG_0E_FIFO_TX_BASE_ADDR, 0);
    spiWrite(RH_RF95_REG_0F_FIFO_RX_BASE_ADDR, 0);

    // Packet format is preamble + explicit-header + payload + crc
    // Explicit Header Mode
    // payload is TO + FROM + ID + FLAGS + message data
    // RX mode is implmented with RXCONTINUOUS
    // max message data length is 255 - 4 = 251 octets

    setModeIdle();

    // Set up default configuration
    // No Sync Words in LORA mode. ACTUALLY thats not correct, and for tehRF95, the default LoRaSync Word is 0x12
    // (ie a private network) and it can be changed at RH_RF95_REG_39_SYNC_WORD
    setModemConfig(Bw125Cr45Sf128); // Radio default
//    setModemConfig(Bw125Cr48Sf4096); // slow and reliable?
    setPreambleLength(8); // Default is 8
    // An innocuous ISM frequency, same as RF22's
    setFrequency(434.0);
    // Lowish power
    setTxPower(13);

    return true;
}

bool RH_RF95::setupInterruptHandler()
{
    // For some subclasses (eg RH_ABZ)  we dont want to set up interrupt
    int interruptNumber = NOT_AN_INTERRUPT;
    if (_interruptPin != RH_INVALID_PIN)
    {
	// Determine the interrupt number that corresponds to the interruptPin
	interruptNumber = digitalPinToInterrupt(_interruptPin);
	if (interruptNumber == NOT_AN_INTERRUPT)
	    return false;
#ifdef RH_ATTACHINTERRUPT_TAKES_PIN_NUMBER
	interruptNumber = _interruptPin;
#endif

    // Tell the low level SPI interface we will use SPI within this interrupt
	spiUsingInterrupt(interruptNumber);
    }

    if (_interruptPin != RH_INVALID_PIN)
    {
	// Add by Adrien van den Bossche <vandenbo@univ-tlse2.fr> for Teensy
	// ARM M4 requires the below. else pin interrupt doesn't work properly.
	// On all other platforms, its innocuous, belt and braces
	pinMode(_interruptPin, INPUT); 
	
	// Set up interrupt handler
	// Since there are a limited number of interrupt glue functions isr*() available,
	// we can only support a limited number of devices simultaneously
	// ON some devices, notably most Arduinos, the interrupt pin passed in is actually the 
	// interrupt number. You have to figure out the interruptnumber-to-interruptpin mapping
	// yourself based on knwledge of what Arduino board you are running on.
	if (_myInterruptIndex == 0xff)
	{
	    static uint8_t interruptCount = 0; // Index into _deviceForInterrupt for next device
	    // First run, no interrupt allocated yet
	    if (interruptCount <= RH_RF95_NUM_INTERRUPTS)
		_myInterruptIndex = interruptCount++;
	    else
		return false; // Too many devices, not enough interrupt vectors
	}
	_deviceForInterrupt[_myInterruptIndex] = this;
	
	if (_myInterruptIndex == 0)
	    attachInterrupt(interruptNumber, isr0, RISING);
	else if (_myInterruptIndex == 1)
	    attachInterrupt(interruptNumber, isr1, RISING);
	else if (_myInterruptIndex == 2)
	    attachInterrupt(interruptNumber, isr2, RISING);
	else
	    return false; // Too many devices, not enough interrupt vectors
    }
    return true;
}

// C++ level interrupt handler for this instance
// LORA is unusual in that it has several interrupt lines, and not a single, combined one.
// On MiniWirelessLoRa, only one of the several interrupt lines (DI0) from the RFM95 is usefuly 
// connnected to the processor.
// We use this to get RxDone and TxDone interrupts
void RH_RF95::handleInterrupt()
{
    RH_MUTEX_LOCK(lock); // Multithreading support
    
    // we need the RF95 IRQ to be level triggered, or we ……have slim chance of missing events
    // https://github.com/geeksville/Meshtastic-esp32/commit/78470ed3f59f5c84fbd1325bcff1fd95b2b20183

    // Read the interrupt register
    uint8_t irq_flags = spiRead(RH_RF95_REG_12_IRQ_FLAGS);
    // Read the RegHopChannel register to check if CRC presence is signalled
    // in the header. If not it might be a stray (noise) packet.*
    uint8_t hop_channel = spiRead(RH_RF95_REG_1C_HOP_CHANNEL);
//    Serial.println(irq_flags, HEX);
//    Serial.println(_mode, HEX);
//    Serial.println(hop_channel, HEX);
//    Serial.println(_enableCRC, HEX);

    // ack all interrupts, 
    // Sigh: on some processors, for some unknown reason, doing this only once does not actually
    // clear the radio's interrupt flag. So we do it twice. Why? (kevinh - I think the root cause we want level
    // triggered interrupts here - not edge.  Because edge allows us to miss handling secondard interrupts that occurred
    // while this ISR was running.  Better to instead, configure the interrupts as level triggered and clear pending
    // at the _beginning_ of the ISR.  If any interrupts occur while handling the ISR, the signal will remain asserted and
    // our ISR will be reinvoked to handle that case)
    // kevinh: turn this off until root cause is known, because it can cause missed interrupts!
    // spiWrite(RH_RF95_REG_12_IRQ_FLAGS, 0xff); // Clear all IRQ flags
    spiWrite(RH_RF95_REG_12_IRQ_FLAGS, 0xff); // Clear all IRQ flags

    // error if:
    // timeout
    // bad CRC
    // CRC is required but it is not present
    if (_mode == RHModeRx
	&& (   (irq_flags & (RH_RF95_RX_TIMEOUT | RH_RF95_PAYLOAD_CRC_ERROR))
	    || (_enableCRC && !(hop_channel & RH_RF95_RX_PAYLOAD_CRC_IS_ON)) ))
//    if (_mode == RHModeRx && irq_flags & (RH_RF95_RX_TIMEOUT | RH_RF95_PAYLOAD_CRC_ERROR))
    {
//	Serial.println("E");
	_rxBad++;
        clearRxBuf();
    }
    // It is possible to get RX_DONE and CRC_ERROR and VALID_HEADER all at once
    // so this must be an else
    else if (_mode == RHModeRx && irq_flags & RH_RF95_RX_DONE)
    {
	// Packet received, no CRC error
//	Serial.println("R");
	// Have received a packet
	uint8_t len = spiRead(RH_RF95_REG_13_RX_NB_BYTES);

	// Reset the fifo read ptr to the beginning of the packet
	spiWrite(RH_RF95_REG_0D_FIFO_ADDR_PTR, spiRead(RH_RF95_REG_10_FIFO_RX_CURRENT_ADDR));
	spiBurstRead(RH_RF95_REG_00_FIFO, _buf, len);
	_bufLen = len;

	// Remember the last signal to noise ratio, LORA mode
	// Per page 111, SX1276/77/78/79 datasheet
	_lastSNR = (int8_t)spiRead(RH_RF95_REG_19_PKT_SNR_VALUE) / 4;

	// Remember the RSSI of this packet, LORA mode
	// this is according to the doc, but is it really correct?
	// weakest receiveable signals are reported RSSI at about -66
	_lastRssi = spiRead(RH_RF95_REG_1A_PKT_RSSI_VALUE);
	// Adjust the RSSI, datasheet page 87
	if (_lastSNR < 0)
	    _lastRssi = _lastRssi + _lastSNR;
	else
	    _lastRssi = (int)_lastRssi * 16 / 15;
	if (_usingHFport)
	    _lastRssi -= 157;
	else
	    _lastRssi -= 164;
	    
	// We have received a message.
	validateRxBuf(); 
	if (_rxBufValid)
	    setModeIdle(); // Got one 
    }
    else if (_mode == RHModeTx && irq_flags & RH_RF95_TX_DONE)
    {
//	Serial.println("T");
	_txGood++;
	setModeIdle();
    }
    else if (_mode == RHModeCad && irq_flags & RH_RF95_CAD_DONE)
    {
//	Serial.println("C");
        _cad = irq_flags & RH_RF95_CAD_DETECTED;
        setModeIdle();
    }
    else
    {
//	Serial.println("?");
    }
	
    // Sigh: on some processors, for some unknown reason, doing this only once does not actually
    // clear the radio's interrupt flag. So we do it twice. Why?
    spiWrite(RH_RF95_REG_12_IRQ_FLAGS, 0xff); // Clear all IRQ flags
    spiWrite(RH_RF95_REG_12_IRQ_FLAGS, 0xff); // Clear all IRQ flags
    RH_MUTEX_UNLOCK(lock); 
}

// These are low level functions that call the interrupt handler for the correct
// instance of RH_RF95.
// 3 interrupts allows us to have 3 different devices
void RH_INTERRUPT_ATTR RH_RF95::isr0()
{
    if (_deviceForInterrupt[0])
	_deviceForInterrupt[0]->handleInterrupt();
}
void RH_INTERRUPT_ATTR RH_RF95::isr1()
{
    if (_deviceForInterrupt[1])
	_deviceForInterrupt[1]->handleInterrupt();
}
void RH_INTERRUPT_ATTR RH_RF95::isr2()
{
    if (_deviceForInterrupt[2])
	_deviceForInterrupt[2]->handleInterrupt();
}

// Check whether the latest received message is complete and uncorrupted
void RH_RF95::validateRxBuf()
{
    if (_bufLen < 4)
	return; // Too short to be a real message
    // Extract the 4 headers
    _rxHeaderTo    = _buf[0];
    _rxHeaderFrom  = _buf[1];
    _rxHeaderId    = _buf[2];
    _rxHeaderFlags = _buf[3];
    if (_promiscuous ||
	_rxHeaderTo == _thisAddress ||
	_rxHeaderTo == RH_BROADCAST_ADDRESS)
    {
	_rxGood++;
	_rxBufValid = true;
    }
}

bool RH_RF95::available()
{
    RH_MUTEX_LOCK(lock); // Multithreading support
    if (_mode == RHModeTx)
    {
    	RH_MUTEX_UNLOCK(lock);
	return false;
    }
    setModeRx();
    RH_MUTEX_UNLOCK(lock);
    return _rxBufValid; // Will be set by the interrupt handler when a good message is received
}

void RH_RF95::clearRxBuf()
{
    ATOMIC_BLOCK_START;
    _rxBufValid = false;
    _bufLen = 0;
    ATOMIC_BLOCK_END;
}

bool RH_RF95::recv(uint8_t* buf, uint8_t* len)
{
    if (!available())
	return false;
    RH_MUTEX_LOCK(lock); // Multithread support
    if (buf && len)
    {
	ATOMIC_BLOCK_START;
	// Skip the 4 headers that are at the beginning of the rxBuf
	if (*len > _bufLen-RH_RF95_HEADER_LEN)
	    *len = _bufLen-RH_RF95_HEADER_LEN;
	memcpy(buf, _buf+RH_RF95_HEADER_LEN, *len);
	ATOMIC_BLOCK_END;
    }
    clearRxBuf(); // This message accepted and cleared
    RH_MUTEX_UNLOCK(lock);
    return true;
}

bool RH_RF95::send(const uint8_t* data, uint8_t len)
{
    if (len > RH_RF95_MAX_MESSAGE_LEN)
	return false;

    waitPacketSent(); // Make sure we dont interrupt an outgoing message
    setModeIdle();

    if (!waitCAD()) 
	return false;  // Check channel activity

    // Position at the beginning of the FIFO
    spiWrite(RH_RF95_REG_0D_FIFO_ADDR_PTR, 0);
    // The headers
    spiWrite(RH_RF95_REG_00_FIFO, _txHeaderTo);
    spiWrite(RH_RF95_REG_00_FIFO, _txHeaderFrom);
    spiWrite(RH_RF95_REG_00_FIFO, _txHeaderId);
    spiWrite(RH_RF95_REG_00_FIFO, _txHeaderFlags);
    // The message data
    spiBurstWrite(RH_RF95_REG_00_FIFO, data, len);
    spiWrite(RH_RF95_REG_22_PAYLOAD_LENGTH, len + RH_RF95_HEADER_LEN);
    
    RH_MUTEX_LOCK(lock); // Multithreading support
    setModeTx(); // Start the transmitter
    RH_MUTEX_UNLOCK(lock);
    
    // when Tx is done, interruptHandler will fire and radio mode will return to STANDBY
    return true;
}

bool RH_RF95::printRegisters()
{
#ifdef RH_HAVE_SERIAL
    uint8_t registers[] = { 0x01, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x014, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x4b};

    uint8_t i;
    for (i = 0; i < sizeof(registers); i++)
    {
	Serial.print(registers[i], HEX);
	Serial.print(": ");
	Serial.println(spiRead(registers[i]), HEX);
    }
#endif
    return true;
}

uint8_t RH_RF95::maxMessageLength()
{
    return RH_RF95_MAX_MESSAGE_LEN;
}

bool RH_RF95::setFrequency(float centre)
{
    // Frf = FRF / FSTEP
    uint32_t frf = (centre * 1000000.0) / RH_RF95_FSTEP;
    spiWrite(RH_RF95_REG_06_FRF_MSB, (frf >> 16) & 0xff);
    spiWrite(RH_RF95_REG_07_FRF_MID, (frf >> 8) & 0xff);
    spiWrite(RH_RF95_REG_08_FRF_LSB, frf & 0xff);
    _usingHFport = (centre >= 779.0);

    return true;
}

void RH_RF95::setModeIdle()
{
    if (_mode != RHModeIdle)
    {
	modeWillChange(RHModeIdle);
	spiWrite(RH_RF95_REG_01_OP_MODE, RH_RF95_MODE_STDBY);
	_mode = RHModeIdle;
    }
}

bool RH_RF95::sleep()
{
    if (_mode != RHModeSleep)
    {
	modeWillChange(RHModeSleep);
	spiWrite(RH_RF95_REG_01_OP_MODE, RH_RF95_MODE_SLEEP);
	_mode = RHModeSleep;
    }
    return true;
}

void RH_RF95::setModeRx()
{
    if (_mode != RHModeRx)
    {
	modeWillChange(RHModeRx);
	spiWrite(RH_RF95_REG_01_OP_MODE, RH_RF95_MODE_RXCONTINUOUS);
	spiWrite(RH_RF95_REG_40_DIO_MAPPING1, 0x00); // Interrupt on RxDone
	_mode = RHModeRx;
    }
}

void RH_RF95::setModeTx()
{
    if (_mode != RHModeTx)
    {
	modeWillChange(RHModeTx);
	spiWrite(RH_RF95_REG_01_OP_MODE, RH_RF95_MODE_TX);
	spiWrite(RH_RF95_REG_40_DIO_MAPPING1, 0x40); // Interrupt on TxDone
	_mode = RHModeTx;
    }
}

void RH_RF95::setTxPower(int8_t power, bool useRFO)
{
    _useRFO = useRFO;
    
    // Sigh, different behaviours depending on whether the module use PA_BOOST or the RFO pin
    // for the transmitter output
    if (useRFO)
    {
	if (power > 15)
	    power = 15;
	if (power < 0)
	    power = 0;
	// Set the MaxPower register to 0x7 => MaxPower = 10.8 + 0.6 * 7 = 15dBm
	// So Pout = Pmax - (15 - power) = 15 - 15 + power
	spiWrite(RH_RF95_REG_09_PA_CONFIG, RH_RF95_MAX_POWER | power);
	spiWrite(RH_RF95_REG_4D_PA_DAC, RH_RF95_PA_DAC_DISABLE);
    }
    else
    {
	if (power > 20)
	    power = 20;
	if (power < 2)
	    power = 2;

	// For RH_RF95_PA_DAC_ENABLE, manual says '+20dBm on PA_BOOST when OutputPower=0xf'
	// RH_RF95_PA_DAC_ENABLE actually adds about 3dBm to all power levels. We will use it
	// for 8, 19 and 20dBm
	if (power > 17)
	{
	    spiWrite(RH_RF95_REG_4D_PA_DAC, RH_RF95_PA_DAC_ENABLE);
	    power -= 3;
	}
	else
	{
	    spiWrite(RH_RF95_REG_4D_PA_DAC, RH_RF95_PA_DAC_DISABLE);
	}

	// RFM95/96/97/98 does not have RFO pins connected to anything. Only PA_BOOST
	// pin is connected, so must use PA_BOOST
	// Pout = 2 + OutputPower (+3dBm if DAC enabled)
	spiWrite(RH_RF95_REG_09_PA_CONFIG, RH_RF95_PA_SELECT | (power-2));
    }
}

// Sets registers from a canned modem configuration structure
void RH_RF95::setModemRegisters(const ModemConfig* config)
{
    spiWrite(RH_RF95_REG_1D_MODEM_CONFIG1,       config->reg_1d);
    spiWrite(RH_RF95_REG_1E_MODEM_CONFIG2,       config->reg_1e);
    spiWrite(RH_RF95_REG_26_MODEM_CONFIG3,       config->reg_26);
}

// Set one of the canned FSK Modem configs
// Returns true if its a valid choice
bool RH_RF95::setModemConfig(ModemConfigChoice index)
{
    if (index > (signed int)(sizeof(MODEM_CONFIG_TABLE) / sizeof(ModemConfig)))
        return false;

    ModemConfig cfg;
    memcpy_P(&cfg, &MODEM_CONFIG_TABLE[index], sizeof(RH_RF95::ModemConfig));
    setModemRegisters(&cfg);

    return true;
}

void RH_RF95::setPreambleLength(uint16_t bytes)
{
    spiWrite(RH_RF95_REG_20_PREAMBLE_MSB, bytes >> 8);
    spiWrite(RH_RF95_REG_21_PREAMBLE_LSB, bytes & 0xff);
}

bool RH_RF95::isChannelActive()
{
    // Set mode RHModeCad
    if (_mode != RHModeCad)
    {
	modeWillChange(RHModeCad);
        spiWrite(RH_RF95_REG_01_OP_MODE, RH_RF95_MODE_CAD);
        spiWrite(RH_RF95_REG_40_DIO_MAPPING1, 0x80); // Interrupt on CadDone
        _mode = RHModeCad;
    }

    while (_mode == RHModeCad)
        YIELD;

    return _cad;
}

void RH_RF95::enableTCXO(bool on)
{
    if (on)
    {
	while ((spiRead(RH_RF95_REG_4B_TCXO) & RH_RF95_TCXO_TCXO_INPUT_ON) != RH_RF95_TCXO_TCXO_INPUT_ON)
	{
	    sleep();
	    spiWrite(RH_RF95_REG_4B_TCXO, (spiRead(RH_RF95_REG_4B_TCXO) | RH_RF95_TCXO_TCXO_INPUT_ON));
	}
    }
    else
    {
	while ((spiRead(RH_RF95_REG_4B_TCXO) & RH_RF95_TCXO_TCXO_INPUT_ON))
	{
	    sleep();
	    spiWrite(RH_RF95_REG_4B_TCXO, (spiRead(RH_RF95_REG_4B_TCXO) & ~RH_RF95_TCXO_TCXO_INPUT_ON));
	}
    }
}

// From section 4.1.5 of SX1276/77/78/79
// Ferror = FreqError * 2**24 * BW / Fxtal / 500
int RH_RF95::frequencyError()
{
    int32_t freqerror = 0;

    // Convert 2.5 bytes (5 nibbles, 20 bits) to 32 bit signed int
    // Caution: some C compilers make errors with eg:
    // freqerror = spiRead(RH_RF95_REG_28_FEI_MSB) << 16
    // so we go more carefully.
    freqerror = spiRead(RH_RF95_REG_28_FEI_MSB);
    freqerror <<= 8;
    freqerror |= spiRead(RH_RF95_REG_29_FEI_MID);
    freqerror <<= 8;
    freqerror |= spiRead(RH_RF95_REG_2A_FEI_LSB);
    // Sign extension into top 3 nibbles
    if (freqerror & 0x80000)
	freqerror |= 0xfff00000;

    int error = 0; // In hertz
    float bw_tab[] = {7.8, 10.4, 15.6, 20.8, 31.25, 41.7, 62.5, 125, 250, 500};
    uint8_t bwindex = spiRead(RH_RF95_REG_1D_MODEM_CONFIG1) >> 4;
    if (bwindex < (sizeof(bw_tab) / sizeof(float)))
	error = (float)freqerror * bw_tab[bwindex] * ((float)(1L << 24) / (float)RH_RF95_FXOSC / 500.0);
    // else not defined

    return error;
}

int RH_RF95::lastSNR()
{
    return _lastSNR;
}

 ///////////////////////////////////////////////////
 //
 // additions below by Brian Norman 9th Nov 2018
 // brian.n.norman@gmail.com
 //
 // Routines intended to make changing BW, SF and CR
 // a bit more intuitive
 //
 ///////////////////////////////////////////////////
 
 void RH_RF95::setSpreadingFactor(uint8_t sf)
 {
   if (sf <= 6) 
     sf = RH_RF95_SPREADING_FACTOR_64CPS;
   else if (sf == 7) 
     sf = RH_RF95_SPREADING_FACTOR_128CPS;
   else if (sf == 8) 
     sf = RH_RF95_SPREADING_FACTOR_256CPS;
   else if (sf == 9)
     sf = RH_RF95_SPREADING_FACTOR_512CPS;
   else if (sf == 10)
     sf = RH_RF95_SPREADING_FACTOR_1024CPS;
   else if (sf == 11) 
     sf = RH_RF95_SPREADING_FACTOR_2048CPS;
   else if (sf >= 12)
     sf =  RH_RF95_SPREADING_FACTOR_4096CPS;
 
   // set the new spreading factor
   spiWrite(RH_RF95_REG_1E_MODEM_CONFIG2, (spiRead(RH_RF95_REG_1E_MODEM_CONFIG2) & ~RH_RF95_SPREADING_FACTOR) | sf);
   // check if Low data Rate bit should be set or cleared
   setLowDatarate();
 }
 
void RH_RF95::setSignalBandwidth(long sbw)
{
    uint8_t bw; //register bit pattern
 
    if (sbw <= 7800)
	bw = RH_RF95_BW_7_8KHZ;
    else if (sbw <= 10400)
	bw =  RH_RF95_BW_10_4KHZ;
    else if (sbw <= 15600)
	bw = RH_RF95_BW_15_6KHZ ;
    else if (sbw <= 20800)
	bw = RH_RF95_BW_20_8KHZ;
    else if (sbw <= 31250)
	bw = RH_RF95_BW_31_25KHZ;
    else if (sbw <= 41700)
	bw = RH_RF95_BW_41_7KHZ;
    else if (sbw <= 62500)
	bw = RH_RF95_BW_62_5KHZ;
    else if (sbw <= 125000)
	bw = RH_RF95_BW_125KHZ;
    else if (sbw <= 250000)
	bw = RH_RF95_BW_250KHZ;
    else 
	bw =  RH_RF95_BW_500KHZ;
     
    // top 4 bits of reg 1D control bandwidth
    spiWrite(RH_RF95_REG_1D_MODEM_CONFIG1, (spiRead(RH_RF95_REG_1D_MODEM_CONFIG1) & ~RH_RF95_BW) | bw);
    // check if low data rate bit should be set or cleared
    setLowDatarate();
}
 
void RH_RF95::setCodingRate4(uint8_t denominator)
{
    int cr = RH_RF95_CODING_RATE_4_5;
 
//    if (denominator <= 5)
//	cr = RH_RF95_CODING_RATE_4_5;
    if (denominator == 6)
	cr = RH_RF95_CODING_RATE_4_6;
    else if (denominator == 7)
	cr = RH_RF95_CODING_RATE_4_7;
    else if (denominator >= 8)
	cr = RH_RF95_CODING_RATE_4_8;
 
    // CR is bits 3..1 of RH_RF95_REG_1D_MODEM_CONFIG1
    spiWrite(RH_RF95_REG_1D_MODEM_CONFIG1, (spiRead(RH_RF95_REG_1D_MODEM_CONFIG1) & ~RH_RF95_CODING_RATE) | cr);
}
 
void RH_RF95::setLowDatarate()
{
    // called after changing bandwidth and/or spreading factor
    //  Semtech modem design guide AN1200.13 says 
    // "To avoid issues surrounding  drift  of  the  crystal  reference  oscillator  due  to  either  temperature  change  
    // or  motion,the  low  data  rate optimization  bit  is  used. Specifically for 125  kHz  bandwidth  and  SF  =  11  and  12,  
    // this  adds  a  small  overhead  to increase robustness to reference frequency variations over the timescale of the LoRa packet."
 
    // read current value for BW and SF
    uint8_t BW = spiRead(RH_RF95_REG_1D_MODEM_CONFIG1) >> 4;	// bw is in bits 7..4
    uint8_t SF = spiRead(RH_RF95_REG_1E_MODEM_CONFIG2) >> 4;	// sf is in bits 7..4
   
    // calculate symbol time (see Semtech AN1200.22 section 4)
    float bw_tab[] = {7800, 10400, 15600, 20800, 31250, 41700, 62500, 125000, 250000, 500000};
   
    float bandwidth = bw_tab[BW];
   
    float symbolTime = 1000.0 * pow(2, SF) / bandwidth;	// ms
   
    // the symbolTime for SF 11 BW 125 is 16.384ms. 
    // and, according to this :- 
    // https://www.thethingsnetwork.org/forum/t/a-point-to-note-lora-low-data-rate-optimisation-flag/12007
    // the LDR bit should be set if the Symbol Time is > 16ms
    // So the threshold used here is 16.0ms
 
    // the LDR is bit 3 of RH_RF95_REG_26_MODEM_CONFIG3
    uint8_t current = spiRead(RH_RF95_REG_26_MODEM_CONFIG3) & ~RH_RF95_LOW_DATA_RATE_OPTIMIZE; // mask off the LDR bit
    if (symbolTime > 16.0)
	spiWrite(RH_RF95_REG_26_MODEM_CONFIG3, current | RH_RF95_LOW_DATA_RATE_OPTIMIZE);
    else
	spiWrite(RH_RF95_REG_26_MODEM_CONFIG3, current);
}
 
void RH_RF95::setPayloadCRC(bool on)
{
    // Payload CRC is bit 2 of register 1E
    uint8_t current = spiRead(RH_RF95_REG_1E_MODEM_CONFIG2) & ~RH_RF95_PAYLOAD_CRC_ON; // mask off the CRC
   
    if (on)
	spiWrite(RH_RF95_REG_1E_MODEM_CONFIG2, current | RH_RF95_PAYLOAD_CRC_ON);
    else
	spiWrite(RH_RF95_REG_1E_MODEM_CONFIG2, current);
    _enableCRC = on;
}
 
uint8_t RH_RF95::getDeviceVersion()
{
    _deviceVersion = spiRead(RH_RF95_REG_42_VERSION);
    return _deviceVersion;
}

