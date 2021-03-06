//--------------------------------------------------------------------
// File: lcd.c
// Devices: MSP430F247
// Author: David Finn, Tritium Pty Ltd.
// History:
//  28/06/11 - original
// Required hardware:
// Notes:
//--------------------------------------------------------------------
//#include <msp430x24x.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include "variant.h"
#include "lcd.h"
#include "meas.h"
#include "usci.h"
#include "cfg.h"
#include "flag.h"
#include "flash.h"
#include "util.h"
#include "comms.h"
#include "usci.h"
#include "protocol.h"
#include "crc16.h"
#include "status.h"
#include "can.h"
#include "stats.h"
#include "ctrl.h"
#include "adc.h"

#define SEGMENT_A_ADDRESS 0x10C0
#define SEGMENT_A_LENGTH 64
#define FLASH_TYPE_PERSISTENT 0x7FFE
#define FLASH_TYPE_CALIB 0x7FFF
#define LCD_FLASH_READ_ATTEMPTS 2
#define LCD_FLASH_WRITE_ATTEMPTS 2

int lcd_writeNext = -1;
int lcd_writeRemainingAttempts = 0;
lcd_CfgState lcd_cfgState = LCDCFG_STATE_CFG_IDLE;
int lcd_writeType = TYPE_TELEMETRY;
unsigned char lcd_writeBuffer[FLASH_BUFFER_SIZE];
unsigned long lcd_writeAddress;
unsigned long lcd_writeLen;
int update_persistent = 0;

void lcd_update_local_config(void);
void lcd_update_remote_config(void);

void lcd_copyProductInfo(void);
void lcd_loadTelemetry(void);
void lcd_loadSysInfo(void);
void lcd_loadUserDefaults(void);
void lcd_loadFactoryDefaults(void);
void lcd_loadEventsDefaults(void);
void lcd_loadMiscStateDefaults(void);
void lcd_loadPersistentDefaults(void);

int lcd_check_calibration(unsigned char* calib);
int lcd_calibrationChanged(unsigned char* calib, unsigned char* calibBackup);
void lcd_restoreCalibration(unsigned char* calib);
void lcd_startWriteCalibrationBackup(void);

extern floatValue_t calculatedValue;

int lcd_read_flash(unsigned long addr, unsigned long len, unsigned char* buffer, int attempts)
{
	int res;
	int remainingAttempts = attempts;
	if (remainingAttempts <= 0)
	{
		remainingAttempts = 1;
	}

	while (remainingAttempts > 0)
	{
		FLASH_readStr(lcd_writeBuffer, addr, len + 2, 0);
		res = CalculateCRC16(lcd_writeBuffer, len + 2, 1, 0xFF);
		if (res == 2)
		{
			memcpy(buffer, lcd_writeBuffer, len);
			return 1;
		}
		remainingAttempts--;
	}

	return 0;
}

void lcd_init(void)
{
	int calibOK;
	int calibBackupOK;
	unsigned char* segmentA = (unsigned char*) SEGMENT_A_ADDRESS;

	lcd_writeNext = -1;
	lcd_writeRemainingAttempts = 0;

	if (!lcd_read_flash(FLASH_USER_CFG_ADDR, sizeof(userConfig_t), userConfig_R.bytes, LCD_FLASH_READ_ATTEMPTS))
	{
		lcd_loadUserDefaults();
		telemetry_R.status.config_userCRC = 0;
	}
	else
	{
		telemetry_R.status.config_userCRC = 1;
	}
	
	if (!lcd_read_flash(FLASH_FACT_CFG_ADDR, sizeof(factoryConfig_t), factoryConfig_R.bytes, LCD_FLASH_READ_ATTEMPTS))
	{
		lcd_loadFactoryDefaults();
		telemetry_R.status.config_factoryCRC = 0;
	}
	else 
	{
		telemetry_R.status.config_factoryCRC = 1;
	}

	lcd_loadSysInfo();
	lcd_copyProductInfo();

	if (!lcd_read_flash(FLASH_EVNT_CFG_ADDR, sizeof(eventConfig_t), eventConfig_R.bytes, LCD_FLASH_READ_ATTEMPTS))
	{
		lcd_loadEventsDefaults();
		telemetry_R.status.config_eventCRC = 0;
	}
	else
	{
		telemetry_R.status.config_eventCRC = 1;
	}

	if (!lcd_read_flash(FLASH_MISC_STATE_ADDR, sizeof(miscState_t), miscState_R.bytes, LCD_FLASH_READ_ATTEMPTS))
	{
		lcd_loadMiscStateDefaults();
		telemetry_R.status.config_miscCRC = 0;
	}
	else
	{
		telemetry_R.status.config_miscCRC = 1;
	}

	if (!lcd_read_flash(FLASH_PERSISTENT_ADDR, sizeof(persistentStorage_t), (unsigned char*)&persistentStorage, LCD_FLASH_READ_ATTEMPTS))
	{
		lcd_loadPersistentDefaults();
	}

	lcd_update_local_config();
	lcd_update_remote_config();

	CFG_toggleSwitchMode = miscState_R.toggleSwitchMode;
	CFG_outVoltCutoffOffset = miscState_R.outVoltCutoffOffset;
	CFG_outVoltCutoffScale = miscState_R.outVoltCutoffScale;

	cfg.status = telemetry_R.bytes[0];

	// Check calibration data
	calibOK = lcd_check_calibration(segmentA);
	FLASH_readStr(lcd_writeBuffer, FLASH_CALIB_ADDR, SEGMENT_A_LENGTH, 0);
	calibBackupOK = lcd_check_calibration(lcd_writeBuffer);

	if (calibOK && ((!calibBackupOK) || lcd_calibrationChanged(segmentA, lcd_writeBuffer)))
	{
		lcd_startWriteCalibrationBackup();
	}
	else if ((!calibOK) && calibBackupOK)
	{
		lcd_restoreCalibration(lcd_writeBuffer);
	}
}

int lcd_check_calibration(unsigned char* calib)
{
	int i;
	int blank = 1;
	unsigned int crc;

	for (i=0; i<SEGMENT_A_LENGTH; i++)
	{
		if (calib[i] != 0xFF)
		{
			blank = 0;
			break;
		}
	}
	if (blank)
	{
		return 0;
	}

	crc = 0;
	for (i=2; i<SEGMENT_A_LENGTH-1; i += 2)
	{
		crc ^= *((unsigned int*)&calib[i]);
	}
	return !(crc + *((unsigned int*)&calib[0]));
}

int lcd_calibrationChanged(unsigned char* calib, unsigned char* calibBackup)
{
	int i;
	for (i=0; i<SEGMENT_A_LENGTH; i++)
	{
		if (calibBackup[i] != calib[i])
		{
			return 1;
		}
	}
	return 0;
}

void lcd_restoreCalibration(unsigned char* calib)
{
//	unsigned char* segmentA = (unsigned char*) SEGMENT_A_ADDRESS;
//	unsigned int i;

//	BCSCTL1 = calib[63];	// Set DCO to 1MHz
//	DCOCTL = calib[62];

//	FCTL2 = FWKEY + FSSEL0 + FN1;
//	if (FCTL3 & LOCKA)		// Unlock segmentA if it is locked
//	{
//		FCTL3 = FWKEY + LOCKA;
//	}
//	FCTL3 = FWKEY;			// clear lock bit
//	FCTL1 = FWKEY + ERASE;	// set erase bit
//	
//	*segmentA = 0;			// erase segment A

//	FCTL1 = FWKEY + WRT;	// set write bit

//	for (i=0; i<SEGMENT_A_LENGTH; i++)
//	{
//		segmentA[i] = calib[i];	// write
//	}

//	FCTL1 = FWKEY;			// Clear write bit
//	FCTL3 = FWKEY + LOCKA;	// Lock segmentA

//	BCSCTL1 = CALBC1_16MHZ;	// Set DCO to 16MHz
//	DCOCTL = CALDCO_16MHZ;
}

void lcd_update_local_config(void)
{
	strncpy(CFG_localCfg.productCode.chars, factoryConfig_R.productID,4);
	CFG_localCfg.serialNumber = 0;
	if (factoryConfig_R.serialNumber[0] != 0x00)
	{
		CFG_localCfg.serialNumber = (Uint32) atoi( factoryConfig_R.serialNumber);
	}

	CFG_localCfg.canBaseId = (Uint32) userConfig_R.commsConfig.canBusID;
	CFG_localCfg.canBaud = (Uint32) userConfig_R.commsConfig.canBaudRate;
	CFG_localCfg.configVersion = 101;
	CFG_localCfg.outVoltAdcCal.scale = IQ_cnst(factoryConfig_R.outVoltAdcCal.scale);
	CFG_localCfg.outVoltAdcCal.offset = IQ_cnst(factoryConfig_R.outVoltAdcCal.offset);
	CFG_localCfg.outCurrAdcCal.scale = IQ_cnst(factoryConfig_R.outCurrentAdcCal.scale);
	CFG_localCfg.outCurrAdcCal.offset = IQ_cnst(factoryConfig_R.outCurrentAdcCal.offset);
	CFG_localCfg.pvVoltAdcCal.scale = IQ_cnst(factoryConfig_R.pvVoltAdcCal.scale);
	CFG_localCfg.pvVoltAdcCal.offset = IQ_cnst(factoryConfig_R.pvVoltAdcCal.offset);
	CFG_localCfg.pvCurrAdcCal.scale = IQ_cnst(factoryConfig_R.pvCurrentAdcCal.scale);
	CFG_localCfg.pvCurrAdcCal.offset = IQ_cnst(factoryConfig_R.pvCurrentAdcCal.offset);

	CFG_localCfg.rail12VoltAdcCal.scale = IQ_cnst(factoryConfig_R.rail12VoltAdcCal.scale);
	CFG_localCfg.rail12VoltAdcCal.offset = IQ_cnst(factoryConfig_R.rail12VoltAdcCal.offset);

	CFG_localCfg.flSetSenseAdcCal.scale = IQ_cnst(factoryConfig_R.floatSetSenseAdcCal.scale);
	CFG_localCfg.flSetSenseAdcCal.offset = IQ_cnst(factoryConfig_R.floatSetSenseAdcCal.offset);

	CFG_localCfg.caseTmpAdcCal.scale = IQ_cnst(factoryConfig_R.pwmErrorMinusAdcCal.scale);
	CFG_localCfg.caseTmpAdcCal.offset = IQ_cnst(factoryConfig_R.pwmErrorMinusAdcCal.offset);
	
	CFG_localCfg.tmpCmpSenseAdcCal.scale = IQ_cnst(factoryConfig_R.tmpCmpSenseAdcCal.scale);
#ifndef EXTERNAL_TEMP
	CFG_localCfg.tmpCmpSenseAdcCal.offset = IQ_cnst(factoryConfig_R.tmpCmpSenseAdcCal.offset);
#else
	// Divide offset by 100 so calibration can be done using a default range of 0 to 100 deg
	CFG_localCfg.tmpCmpSenseAdcCal.offset = IQ_cnst(factoryConfig_R.tmpCmpSenseAdcCal.offset/MEAS_TEMPR_BASE);
#endif
	
	CFG_localCfg.mpptSamplePtDacCal.scale = IQ_cnst(factoryConfig_R.mpptSamplePointDacCal.scale);
	CFG_localCfg.mpptSamplePtDacCal.offset = IQ_cnst(factoryConfig_R.mpptSamplePointDacCal.offset);
	CFG_localCfg.vinLimDacCal.scale = IQ_cnst(factoryConfig_R.vinLimDacCal.scale);
	CFG_localCfg.vinLimDacCal.offset = IQ_cnst(factoryConfig_R.vinLimDacCal.offset);
	CFG_localCfg.flTrimDacCal.scale = IQ_cnst(factoryConfig_R.floatTrimDacCal.scale);
	CFG_localCfg.flTrimDacCal.offset = IQ_cnst(factoryConfig_R.floatTrimDacCal.offset);
	
	CFG_localCfg.thermRinf =factoryConfig_R.thermRinf;
	CFG_localCfg.thermBeta = factoryConfig_R.thermBeta;
	CFG_localCfg.isSlave = (float) userConfig_R.commsConfig.isSlave;
	CFG_localCfg.overCurrSpSw = factoryConfig_R.overCurrentSetPoint;
}


void lcd_update_remote_config(void)
{
	CFG_remoteCfg.systemInitFlag.mode = FLAG_LOG;
	CFG_remoteCfg.lowOutVoltWarnFlag.mode = (FLAG_Mode) eventConfig_R.lowOutVoltWarn.mode;
	CFG_remoteCfg.lowOutVoltWarnFlag.hystTime = (TimeShort) eventConfig_R.lowOutVoltWarn.hystTime;
	CFG_remoteCfg.lowOutVoltWarnFlag.resetVal = eventConfig_R.lowOutVoltWarn.resetVal;
	CFG_remoteCfg.lowOutVoltWarnFlag.triggerVal = eventConfig_R.lowOutVoltWarn.triggerVal;

	CFG_remoteCfg.lowOutVoltFaultFlag.mode = (FLAG_Mode) eventConfig_R.lowOutVoltFault.mode;
	CFG_remoteCfg.lowOutVoltFaultFlag.hystTime = (TimeShort) eventConfig_R.lowOutVoltFault.hystTime;
	CFG_remoteCfg.lowOutVoltFaultFlag.resetVal = eventConfig_R.lowOutVoltFault.resetVal;
	CFG_remoteCfg.lowOutVoltFaultFlag.triggerVal = eventConfig_R.lowOutVoltFault.triggerVal;

	CFG_remoteCfg.lowOutVoltGensetFlag.mode = (FLAG_Mode) userConfig_R.lowOutVoltGenset.mode;
	CFG_remoteCfg.lowOutVoltGensetFlag.hystTime = (TimeShort) userConfig_R.lowOutVoltGenset.hystTime;
	CFG_remoteCfg.lowOutVoltGensetFlag.resetVal = userConfig_R.lowOutVoltGenset.resetVal;
	CFG_remoteCfg.lowOutVoltGensetFlag.triggerVal = userConfig_R.lowOutVoltGenset.triggerVal;
	CFG_remoteCfg.lowOutVoltGensetFlag.holdTime = (TimeShort) userConfig_R.lowOutVoltGenset.holdTime;

	CFG_remoteCfg.highOutVoltFaultFlag.mode = (FLAG_Mode) eventConfig_R.highOutVoltFault.mode;
	CFG_remoteCfg.highOutVoltFaultFlag.hystTime = (TimeShort) eventConfig_R.highOutVoltFault.hystTime;
	CFG_remoteCfg.highOutVoltFaultFlag.resetVal = eventConfig_R.highOutVoltFault.resetVal;
	CFG_remoteCfg.highOutVoltFaultFlag.triggerVal = eventConfig_R.highOutVoltFault.triggerVal;

	CFG_remoteCfg.highOutCurrFaultFlag.mode = (FLAG_Mode) eventConfig_R.highOutCurrentFault.mode;
	CFG_remoteCfg.highOutCurrFaultFlag.hystTime = (TimeShort) eventConfig_R.highOutCurrentFault.hystTime;
	CFG_remoteCfg.highOutCurrFaultFlag.resetVal = eventConfig_R.highOutCurrentFault.resetVal;
	CFG_remoteCfg.highOutCurrFaultFlag.triggerVal = eventConfig_R.highOutCurrentFault.triggerVal;

	CFG_remoteCfg.highDisCurrFaultFlag.mode = (FLAG_Mode) eventConfig_R.highDischargeCurrentFault.mode;
	CFG_remoteCfg.highDisCurrFaultFlag.hystTime = (TimeShort) eventConfig_R.highDischargeCurrentFault.hystTime;
	CFG_remoteCfg.highDisCurrFaultFlag.resetVal = eventConfig_R.highDischargeCurrentFault.resetVal;
	CFG_remoteCfg.highDisCurrFaultFlag.triggerVal = eventConfig_R.highDischargeCurrentFault.triggerVal;

	CFG_remoteCfg.highTempFaultFlag.mode = (FLAG_Mode) eventConfig_R.highBatteryTempFault.mode;
	CFG_remoteCfg.highTempFaultFlag.hystTime = (TimeShort) eventConfig_R.highBatteryTempFault.hystTime;
	CFG_remoteCfg.highTempFaultFlag.resetVal = eventConfig_R.highBatteryTempFault.resetVal;
	CFG_remoteCfg.highTempFaultFlag.triggerVal = eventConfig_R.highBatteryTempFault.triggerVal;

	CFG_remoteCfg.highTempFaultFlag.mode = (FLAG_Mode) eventConfig_R.highBatteryTempFault.mode;
	CFG_remoteCfg.highTempFaultFlag.hystTime = (TimeShort) eventConfig_R.highBatteryTempFault.hystTime;
	CFG_remoteCfg.highTempFaultFlag.resetVal = eventConfig_R.highBatteryTempFault.resetVal;
	CFG_remoteCfg.highTempFaultFlag.triggerVal = eventConfig_R.highBatteryTempFault.triggerVal;

	CFG_remoteCfg.inBreakerOpenFlag.mode = (FLAG_Mode) eventConfig_R.inputBreakerOpen.mode;
	CFG_remoteCfg.outBreakerOpenFlag.mode = (FLAG_Mode) eventConfig_R.outputBreakerOpen.mode;
	CFG_remoteCfg.tempSenseFaultFlag.mode = (FLAG_Mode) eventConfig_R.tempSensorFault.mode;
	CFG_remoteCfg.sdFlags[SAFETY_SD_BIT_PVCURR_NEG].mode = (FLAG_Mode) eventConfig_R.pvNegativeCurrentSutdown.mode;
	CFG_remoteCfg.sdFlags[SAFETY_SD_BIT_PVCURR_POS].mode = (FLAG_Mode) eventConfig_R.pvHighCurrentShutdown.mode;
	CFG_remoteCfg.sdFlags[SAFETY_SD_BIT_PVVOLT].mode = (FLAG_Mode) eventConfig_R.pvHighVoltShutdown.mode;
	CFG_remoteCfg.sdFlags[SAFETY_SD_BIT_OUTCURR_POS].mode = (FLAG_Mode) eventConfig_R.highOutCurrentShutdown.mode;
	CFG_remoteCfg.sdFlags[SAFETY_SD_BIT_OUTVOLT].mode = (FLAG_Mode) eventConfig_R.highOutVoltShutdown.mode;

	CFG_remoteCfg.logFullFlag.mode = (FLAG_Mode) eventConfig_R.logFull.mode;

	CFG_remoteCfg.panelMissingFlag.mode = (FLAG_Mode) eventConfig_R.solarPanelMissing.mode;
	CFG_remoteCfg.panelMissingFlag.checkTime = (TimeShort) eventConfig_R.solarPanelMissing.checkTime;
	CFG_remoteCfg.panelMissingFlag.triggerVal = eventConfig_R.solarPanelMissing.triggerVal;

	CFG_remoteCfg.telemSamplePeriod = TELEM_PERIOD_200MS;
	CFG_remoteCfg.telemEnableBitfield = TELEM_OUTVOLT_MASK | TELEM_OUTCURR_MASK | TELEM_OUTCHARGE_MASK | TELEM_PVVOLT_MASK
		| TELEM_PVCURR_MASK | TELEM_PVPOWER_MASK | TELEM_PVOCVOLT_MASK;

	CFG_remoteCfg.pvOcVolt = userConfig_R.setPointsConfig.pvOcVolt;
	CFG_remoteCfg.pvMpVolt = userConfig_R.setPointsConfig.pvMpVolt;
	CFG_remoteCfg.floatVolt = userConfig_R.setPointsConfig.floatVolt;
	CFG_remoteCfg.bulkVolt = userConfig_R.setPointsConfig.bulkVolt;
	CFG_remoteCfg.bulkTime = userConfig_R.setPointsConfig.bulkTime;
	CFG_remoteCfg.bulkResetVolt = userConfig_R.setPointsConfig.bulkResetVolt;
	CFG_remoteCfg.tmpCmp = userConfig_R.setPointsConfig.tempCompensation;
}

void lcd_update(void)
{	
	if(lcd_cfgState == LCDCFG_STATE_CFG_IDLE)
	{
		lcd_startWrite();
	}

	lcd_runCfgStateMachine();
}

// Copy productID and serialNumber to telemetry_R and sysInfo_R
void lcd_copyProductInfo(void)
{
	// Copy to telemetry
	strncpy(telemetry_R.productID, factoryConfig_R.productID,8);
	strncpy(telemetry_R.serialNumber, factoryConfig_R.serialNumber,8);

	// Copy to sysInfo
	strncpy(sysInfo_R.productID, factoryConfig_R.productID,8);
	strncpy(sysInfo_R.serialNumber, factoryConfig_R.serialNumber,8);
}

void lcd_loadSysInfo(void)
{
	sysInfo_R.hwVersion[0] = 'V';
	sysInfo_R.hwVersion[1] = (char) hware.hardware_version + '0';
	sysInfo_R.hwVersion[2] = '.';
	sysInfo_R.hwVersion[3] = '0';
	sysInfo_R.hwVersion[4] = 0x00;
	strcpy(sysInfo_R.fwVersion, VAR_VERSION_NUMBER_STR);
	if( hware.model_id == MODEL_ID_MV )
	{
		strcpy(sysInfo_R.modelType, "MV");
	}
	else
	{
		if( hware.model_id == MODEL_ID_HV )
		{
			strcpy(sysInfo_R.modelType, "HV");
		}
		else
		{
			strcpy(sysInfo_R.modelType, "?V");
		}
	}
}

int time = 0;
void lcd_loadTelemetry(void)
{
	/*
	telemetry_R.pvVoltage = 100.0f;
	telemetry_R.pvCurrent = 15.2f;
	telemetry_R.outputVoltage = 48.0f;
	telemetry_R.outputCurrent = 30.3f;
	telemetry_R.ocVoltage = 130.5f;
	telemetry_R.outputCharge = 16.7f;
	telemetry_R.pvPower = 99.9f;
	telemetry_R.batteryTemp = 34.6f;
	telemetry_R.eventFlags.flags = (uint32_t) 0x11223344;
	*/
	
//	telemetry_R.pvVoltage = meas.pvVolt.valReal;
	telemetry_R.pvVoltage = calculatedValue.vInSensor;
//	telemetry_R.pvCurrent = meas.pvCurr.valReal;
//	telemetry_R.outputVoltage = meas.outVolt.valReal;
//	telemetry_R.outputCurrent = meas.outCurr.valReal;
//	telemetry_R.ocVoltage = meas.pvOcVolt.valReal;
//	telemetry_R.outputCharge = meas.outCharge.valReal;
//	telemetry_R.pvPower = meas.pvPower.valReal;
	if ((userConfig_R.setPointsConfig.tempCompensation > 0.01f) || (userConfig_R.setPointsConfig.tempCompensation < -0.01f))
	{
		telemetry_R.batteryTemp = meas.batTempr.valReal;
	}
	else
	{
		telemetry_R.batteryTemp = meas.caseTempr.valReal;
	}
	telemetry_R.eventFlags.flags = FLAG_getFlagBitfield();
	
	/*
	telemetry_R.eventFlags.flags = (uint32_t) 0xFFFFFFFF;

	telemetry_R.status.config_factoryCheck = 1;
	telemetry_R.status.config_userCheck = 1;
	telemetry_R.status.config_eventCheck = 1;

	telemetry_R.status.statistics = 0;
	telemetry_R.status.flags = 0;
	telemetry_R.status.telemetry = 0;
	telemetry_R.status.communications = 0;
	telemetry_R.status.flash = 0;

	telemetry_R.status.control_isSlave = 0;
	telemetry_R.status.control_usingBulk = 0;
	telemetry_R.status.control_mode = 0;

	//telemetry_R.status.status =  (uint64_t) 0x1234567887654321;
	*/
	//telemetry_R.status.status =  (uint64_t) STATUS_getStatus();
	if (CFG_configRangesOk())
	{
		telemetry_R.status.bytes[0]  |= CONFIG_CRC_OK;
	}
	else
	{
		telemetry_R.status.bytes[0]  &= ~(CONFIG_CRC_OK);
	}
	cfg.status = telemetry_R.status.bytes[0];

	telemetry_R.status.bytes[1] = STATS_getStatus();
	telemetry_R.status.bytes[2] = FLAG_getStatus();
	telemetry_R.status.bytes[3] = TELEM_getStatus();
	telemetry_R.status.bytes[4] = COMMS_getStatus();
	telemetry_R.status.bytes[5] = FLASH_getStatus();
	telemetry_R.status.bytes[6] = CTRL_getStatus();
	telemetry_R.status.bytes[7] = SAFETY_getStatus();

	//telemetry_R.time = (uint64_t) 0x1234567887654321; 
	//telemetry_R.time = time++; 

	telemetry_R.time = (uint64_t) TIME_get();
}

void lcd_loadFactoryDefaults()
{
	strcpy(factoryConfig_R.productID, "A001");
	strcpy(factoryConfig_R.serialNumber, "0000");

	factoryConfig_R.outVoltAdcCal.scale = 1.0f;
	factoryConfig_R.outVoltAdcCal.offset = 0.0f;
	factoryConfig_R.pvVoltAdcCal.scale = 1.0f;
	factoryConfig_R.pvVoltAdcCal.offset = 0.0f;

	factoryConfig_R.outCurrentAdcCal.scale = 1.0f;
	factoryConfig_R.outCurrentAdcCal.offset = 0.0f;
	factoryConfig_R.pvCurrentAdcCal.scale = 1.0f;
	factoryConfig_R.pvCurrentAdcCal.offset = 0.0f;

	factoryConfig_R.mpptSamplePointDacCal.scale = 1.0f;
	factoryConfig_R.mpptSamplePointDacCal.offset = 1.0f;
	factoryConfig_R.pwmErrorMinusAdcCal.scale = 1.0f;
	factoryConfig_R.pwmErrorMinusAdcCal.offset = 0.0f;
	factoryConfig_R.rail12VoltAdcCal.scale = 1.0f;
	factoryConfig_R.rail12VoltAdcCal.offset = 0.0f;

	factoryConfig_R.floatSetSenseAdcCal.scale = 1.0f;
	factoryConfig_R.floatSetSenseAdcCal.offset = 0.0f;
	factoryConfig_R.floatTrimDacCal.scale = 1.0f;
	factoryConfig_R.floatTrimDacCal.offset = 0.0f;
		
	factoryConfig_R.tmpCmpSenseAdcCal.scale = 1.0f;
	factoryConfig_R.tmpCmpSenseAdcCal.offset = 0.0f;

	factoryConfig_R.vinLimDacCal.scale = 1.0f;
	factoryConfig_R.vinLimDacCal.offset = 0.0f;
		
	factoryConfig_R.thermRinf = 0.0f;
	factoryConfig_R.thermBeta = 0.0f;
	factoryConfig_R.overCurrentSetPoint = 50.0f;
}

void lcd_loadUserDefaults()
{
	userConfig_R.lowOutVoltGenset.mode = FLAG_LOG_AND_RELAY;
	userConfig_R.lowOutVoltGenset.triggerVal = 38.0f;
	userConfig_R.lowOutVoltGenset.resetVal = 55.0f;
	userConfig_R.lowOutVoltGenset.hystTime = 10000l;
	userConfig_R.lowOutVoltGenset.holdTime = 3600000l;
	userConfig_R.commsConfig.modBusAddress = 0x01;
	userConfig_R.commsConfig.canBaudRate = BAUD_500;
	userConfig_R.commsConfig.canBusID = 0x600;
	userConfig_R.commsConfig.isSlave = 0;
	userConfig_R.setPointsConfig.pvOcVolt = 120.0f;
	userConfig_R.setPointsConfig.pvMpVolt =96.0f;
	userConfig_R.setPointsConfig.floatVolt = 56.4f;
	userConfig_R.setPointsConfig.bulkVolt = 60.9f;
	userConfig_R.setPointsConfig.bulkTime = 1.0l;
	userConfig_R.setPointsConfig.bulkResetVolt = 50.4f;
	userConfig_R.setPointsConfig.tempCompensation = 0.0f;
	userConfig_R.setPointsConfig.nominalVolt = 48.0f;
}

void lcd_loadEventsDefaults()
{		
	eventConfig_R.lowOutVoltWarn.mode = FLAG_LOG;
	eventConfig_R.lowOutVoltWarn.hystTime = 10000l;
	eventConfig_R.lowOutVoltWarn.resetVal = 44.0f;
	eventConfig_R.lowOutVoltWarn.triggerVal = 42.0f;

	eventConfig_R.lowOutVoltFault.mode = FLAG_LOG;
	eventConfig_R.lowOutVoltFault.hystTime = 10000l;
	eventConfig_R.lowOutVoltFault.resetVal = 42.0f;
	eventConfig_R.lowOutVoltFault.triggerVal = 40.0f;

	eventConfig_R.highOutVoltFault.mode = FLAG_LOG;
	eventConfig_R.highOutVoltFault.hystTime = 10000l;
	eventConfig_R.highOutVoltFault.resetVal = 63.0f;
	eventConfig_R.highOutVoltFault.triggerVal = 65.0f;

	eventConfig_R.highOutCurrentFault.mode = FLAG_LOG;
	eventConfig_R.highOutCurrentFault.hystTime = 10000l;
	eventConfig_R.highOutCurrentFault.resetVal = 60.0f;
	eventConfig_R.highOutCurrentFault.triggerVal = 61.0f;

	eventConfig_R.highDischargeCurrentFault.mode = FLAG_LOG;
	eventConfig_R.highDischargeCurrentFault.hystTime = 10000l;
	eventConfig_R.highDischargeCurrentFault.resetVal = 495.0f;
	eventConfig_R.highDischargeCurrentFault.triggerVal = 500.0f;

	eventConfig_R.highBatteryTempFault.mode = FLAG_LOG;
	eventConfig_R.highBatteryTempFault.hystTime = 60000l;
	eventConfig_R.highBatteryTempFault.resetVal = 95.0f;
	eventConfig_R.highBatteryTempFault.triggerVal = 100.0f;

	eventConfig_R.inputBreakerOpen.mode = FLAG_LOG;
	eventConfig_R.outputBreakerOpen.mode = FLAG_LOG;
	eventConfig_R.tempSensorFault.mode = FLAG_LOG;

	eventConfig_R.pvNegativeCurrentSutdown.mode = FLAG_LOG;
	eventConfig_R.pvHighCurrentShutdown.mode = FLAG_LOG;
	eventConfig_R.pvHighVoltShutdown.mode = FLAG_LOG;

	eventConfig_R.highOutCurrentShutdown.mode = FLAG_LOG;
	eventConfig_R.highOutVoltShutdown.mode = FLAG_LOG;

	eventConfig_R.logFull.mode = FLAG_LOG;

	eventConfig_R.solarPanelMissing.mode = FLAG_LOG;
	eventConfig_R.solarPanelMissing.checkTime = 4320000l;
	eventConfig_R.solarPanelMissing.triggerVal = 20.0f;
}

void lcd_loadMiscStateDefaults()
{
	miscState_R.toggleSwitchMode = 1;
	miscState_R._enableSmartShutdown = 0;
#if (MODEL_ID_MV == 1)
	miscState_R.outVoltCutoffOffset = 5.0f;
	miscState_R.outVoltCutoffScale = 0.185f;
#else
	miscState_R.outVoltCutoffOffset = 6.35f;
	miscState_R.outVoltCutoffScale = 0.11f;
#endif
}

void lcd_loadPersistentDefaults()
{
	persistentStorage.autoOn = 1; //RDD 
}

void lcd_startWritePacket(int writeType, unsigned long addr, unsigned long len, unsigned char* buffer, int attempts)
{
	lcd_writeRemainingAttempts = attempts;
	lcd_writeType = writeType;
	lcd_writeAddress = addr;
	memcpy(lcd_writeBuffer, buffer, len);
	lcd_writeLen = len + 2;
	CalculateCRC16(lcd_writeBuffer, (int) lcd_writeLen, 0, 0xFF);
	lcd_cfgState = LCDCFG_STATE_ERASE;
}

// Start writing factoryConfig to flash
void lcd_startWriteFactory(void)
{
	lcd_startWritePacket(TYPE_FACTORY, FLASH_FACT_CFG_ADDR, sizeof(factoryConfig_t), factoryConfig_W.bytes, LCD_FLASH_WRITE_ATTEMPTS);
}

// Start writing userConfig to flash
void lcd_startWriteUser(void)
{
	lcd_startWritePacket(TYPE_USER, FLASH_USER_CFG_ADDR, sizeof(userConfig_t), userConfig_W.bytes, LCD_FLASH_WRITE_ATTEMPTS);
}

// Start writing eventConfig to flash
void lcd_startWriteEvents(void)
{
	lcd_startWritePacket(TYPE_EVENTS, FLASH_EVNT_CFG_ADDR, sizeof(eventConfig_t), eventConfig_W.bytes, LCD_FLASH_WRITE_ATTEMPTS);
}

// Start writing miscState to flash
void lcd_startWriteMiscState(void)
{
	lcd_startWritePacket(TYPE_MISC_STATE, FLASH_MISC_STATE_ADDR, sizeof(miscState_t), miscState_W.bytes, LCD_FLASH_WRITE_ATTEMPTS);
}

void lcd_startWriteCalibrationBackup(void)
{
	lcd_writeType = FLASH_TYPE_CALIB;
	lcd_writeAddress = (unsigned long) FLASH_CALIB_ADDR;
	lcd_writeLen = SEGMENT_A_LENGTH;
	memcpy(lcd_writeBuffer, (unsigned char*) SEGMENT_A_ADDRESS, SEGMENT_A_LENGTH);
	lcd_cfgState = LCDCFG_STATE_ERASE;
}

void lcd_startWritePersistent(void)
{
	lcd_startWritePacket(FLASH_TYPE_PERSISTENT, FLASH_PERSISTENT_ADDR, sizeof(persistentStorage_t), (unsigned char*) &persistentStorage, 1);
}

void lcd_checkPersistentUpdate(void)
{
	if (update_persistent && lcd_cfgState == LCDCFG_STATE_CFG_IDLE)
	{
		update_persistent = 0;
		lcd_startWritePersistent();
	}
}

int lcd_startWrite()
{
	int ret;
	if(lcd_cfgState != LCDCFG_STATE_CFG_IDLE || lcd_writeNext < 0)
	{
		return 0;
	}
	
	switch(lcd_writeNext)
	{
	case TYPE_FACTORY:
		lcd_startWriteFactory();
		ret = 1;
		break;
	case TYPE_USER:
		lcd_startWriteUser();
		ret = 1;
		break;
	case TYPE_EVENTS:
		lcd_startWriteEvents();
		ret = 1;
		break;
	case TYPE_MISC_STATE:
		lcd_startWriteMiscState();
		ret = 1;
		break;
	default:
		ret = 0;
		break;
	}

	lcd_writeNext = -1;

	return ret;
}

// Start writing the packet to flash. Return 1 if writing was started.
int lcd_queueWrite(int type)
{
	if (!(type == TYPE_FACTORY || type == TYPE_USER || type == TYPE_EVENTS || type == TYPE_MISC_STATE))
	{
		return 0;
	}

	lcd_writeNext = type;
	
	if(lcd_cfgState == LCDCFG_STATE_CFG_IDLE)
	{
		lcd_startWrite();
	}

	return 1;
}

void lcd_canRecved(void)
{
	//lcd.cancomms = COMMSTIMEOUT;
}

void lcd_eraseDoneCallback(int retval)
{
	lcd_cfgState = LCDCFG_STATE_WRITE;
}

void lcd_runCfgStateMachine()
{
	int retVal = 0;

	switch(lcd_cfgState)
	{
		case LCDCFG_STATE_ERASE:
			retVal = FLASH_erase(lcd_writeAddress, lcd_writeLen, lcd_eraseDoneCallback );
			if(retVal == 0)
			{
				lcd_cfgState = LCDCFG_STATE_WAIT_FOR_ERASE;
			}
			break;
		case LCDCFG_STATE_WAIT_FOR_ERASE:
			break;
		case LCDCFG_STATE_WRITE:
			retVal = FLASH_startWrite(lcd_writeAddress, lcd_writeLen );
			if ( retVal >= 0 )
			{
				FLASH_writeStr(lcd_writeBuffer, lcd_writeLen );	
				FLASH_endWriteData();
				if (lcd_writeType == FLASH_TYPE_CALIB)
				{
					lcd_cfgState = LCDCFG_STATE_CFG_IDLE;
				}
				else
				{
					lcd_cfgState = LCDCFG_STATE_CHECK_CFG;
				}
			}
			break;
		case LCDCFG_STATE_CHECK_CFG:
			if(FLASH_getMode() == FLASH_IDLE)
			{
				for(retVal=0;retVal<FLASH_BUFFER_SIZE;retVal++)
				{
					lcd_writeBuffer[retVal] = 0x00;
				}
				FLASH_readStr(lcd_writeBuffer, lcd_writeAddress, lcd_writeLen, 0 );
				lcd_cfgState = LCDCFG_STATE_SEND_CFG;
			}
			break;
		case LCDCFG_STATE_SEND_CFG:
			if(FLASH_getMode() == FLASH_IDLE)
			{
				retVal = CalculateCRC16(lcd_writeBuffer,(int) lcd_writeLen,1,0xFF);
				if (retVal != 2)
				{
					lcd_writeRemainingAttempts--;
					if (lcd_writeRemainingAttempts > 0)
					{
						// retry write
						lcd_cfgState = LCDCFG_STATE_ERASE;
						break;
					}
				}
				switch(lcd_writeType)
				{
					case TYPE_TELEMETRY:
						break;
					case TYPE_FACTORY:
						memcpy(factoryConfig_R.bytes, lcd_writeBuffer, sizeof(factoryConfig_t));
						if (retVal != 2)
						{
							lcd_loadFactoryDefaults();
							telemetry_R.status.config_factoryCRC = 0;
						}
						else
						{
							telemetry_R.status.config_factoryCRC = 1;
						}
						lcd_copyProductInfo();
						break;
					case TYPE_USER:
						memcpy(userConfig_R.bytes, lcd_writeBuffer, sizeof(userConfig_t));
						if (retVal != 2)
						{
							lcd_loadUserDefaults();
							telemetry_R.status.config_userCRC = 0;
						}
						else
						{
							telemetry_R.status.config_userCRC = 1;
						}
						break;
					case TYPE_EVENTS:
						memcpy(eventConfig_R.bytes, lcd_writeBuffer, sizeof(eventConfig_t));
						if (retVal != 2)
						{
							lcd_loadEventsDefaults();
							telemetry_R.status.config_eventCRC = 0;
						}
						else
						{
							telemetry_R.status.config_eventCRC = 1;
						}
						break;
					case TYPE_MISC_STATE:
						memcpy(miscState_R.bytes, lcd_writeBuffer, sizeof(miscState_t));
						if (retVal != 2)
						{
							lcd_loadMiscStateDefaults();
							telemetry_R.status.config_miscCRC = 0;
						}
						else
						{
							telemetry_R.status.config_miscCRC = 1;
						}
						break;
					case TYPE_SYS_INFO:
						break;
					case FLASH_TYPE_PERSISTENT:
						if (retVal != 2)
						{
							lcd_loadPersistentDefaults();
						}
						break;
					default:
						break;
				}

				lcd_cfgState = LCDCFG_STATE_CFG_IDLE;

				if (lcd_writeType != FLASH_TYPE_PERSISTENT)
				{
					uart_send(lcd_writeType);
				}
			}
			break;
		case LCDCFG_STATE_CFG_IDLE:
			break;
	}
}

