#include "gps_device.h"
#include "byteswap.h"
#include <stdint.h>
#include <stddef.h>
#include "printk.h"
#include "mem_mang.h"
#include "taskUtil.h"
#include "printk.h"
#include "FreeRTOS.h"
#include "task.h"
#include <math.h>

/* UNIX time (epoch 1/1/1970) at the start of GNSS epoch (1/6/1980) */
#define GNSS_EPOCH_IN_UNIX_EPOCH 315964800

#define GNSS_NAVIGATION_MODE_AUTOMATIC  0
#define GNSS_NAVIGATION_MODE_PEDESTRIAN 1
#define GNSS_NAVIGATION_MODE_AUTOMOBILE 2
#define GNSS_NAVIGATION_MODE_MARINE     3
#define GNSS_NAVIGATION_MODE_BALLOON    4
#define GNSS_NAVIGATION_MODE_AIRBORNE   5

#define MAX_PROVISIONING_ATTEMPTS	10
#define MAX_PAYLOAD_LEN				256
#define GPS_MSG_RX_WAIT_MS			2000
#define GPS_MESSAGE_BUFFER_LEN		1024
#define TARGET_BAUD_RATE 			921600
#define MESSAGE_TYPE_NMEA			1
#define MESSAGE_TYPE_BINARY			2
#define GNSS_NAVIGATION_MODE        GNSS_NAVIGATION_MODE_AUTOMOBILE

#define BAUD_RATE_COUNT 			2
#define BAUD_RATES 					{ \
										{921600, 8},  \
										{9600, 1}   \
									}

typedef struct _BaudRateCodes{
	uint32_t baud;
	uint8_t code;
} BaudRateCodes;

#define UPDATE_RATE_COUNT 		10
#define UPDATE_RATES 			{1, 2, 4, 5, 8, 10, 20, 25, 40, 50}

#define MSG_ID_QUERY_GPS_SW_VER                             0x02
#define MSG_ID_ACK											0x83
#define MSG_ID_NACK 										0x84
#define MSG_ID_QUERY_SW_VERSION 							0x02
#define MSG_ID_SET_FACTORY_DEFAULTS							0x04
#define MSG_ID_SW_VERSION									0x80
#define MSG_ID_QUERY_POSITION_UPDATE_RATE					0x10
#define MSG_ID_CONFIGURE_POSITION_UPDATE_RATE				0x0E
#define MSG_ID_POSITION_UPDATE_RATE 						0x86
#define MSG_ID_CONFIGURE_SERIAL_PORT						0x05
#define MSG_ID_CONFIGURE_GNSS_NAVIGATION_MODE               0x64
#define MSG_SUBID_CONFIGURE_GNSS_NAVIGATION_MODE            0x17
#define MSG_ID_CONFIGURE_NMEA_MESSAGE						0x08
#define MSG_ID_CONFIGURE_MESSAGE_TYPE						0x09
#define MSG_ID_CONFIGURE_NAVIGATION_DATA_MESSAGE_INTERVAL	0x11
#define MSG_ID_NAVIGATION_DATA_MESSAGE						0xA8

#define GGA_INTERVAL							100
#define GSA_INTERVAL							0
#define GSV_INTERVAL							0
#define GLL_INTERVAL							0
#define RMC_INTERVAL							1
#define VTG_INTERVAL							0
#define ZDA_INTERVAL							0
#define NAVIGATION_DATA_MESSAGE_INTERVAL		1

typedef enum{
	ATTRIBUTE_UPDATE_TO_SRAM = 0,
	ATTRIBUTE_UPDATE_TO_SRAM_AND_FLASH,
	ATTRIBUTE_TEMPORARILY
} gps_config_attribute_t;


typedef struct _Ack{
	uint8_t messageId;
	uint8_t ackId;
} Ack;

typedef struct _Nack{
	uint8_t messageId;
	uint8_t ackId;
} Nack;

typedef struct _SetFactoryDefaults{
	uint8_t messageId;
	uint8_t type;
} SetFactoryDefaults;

typedef struct _QuerySwVersion{
	uint8_t messageId;
	uint8_t softwareType;
} QuerySwVersion;

typedef struct _QueryPositionUpdateRate{
	uint8_t messageId;
} QueryPositionUpdateRate;

typedef struct _ConfigurePositionUpdateRate{
	uint8_t messageId;
	uint8_t rate;
	uint8_t attributes;
} ConfigurePositionUpdateRate;

typedef struct _PositionUpdateRate{
	uint8_t messageId;
	uint8_t rate;
} PositionUpdateRate;

typedef struct _ConfigureSerialPort{
	uint8_t messageId;
	uint8_t comPort;
	uint8_t baudRateCode;
	uint8_t attributes;
} ConfigureSerialPort;

typedef struct _ConfigureNmeaMessage{
	uint8_t messageId;
	uint8_t GGA_interval;
	uint8_t GSA_interval;
	uint8_t GSV_interval;
	uint8_t GLL_interval;
	uint8_t RMC_interval;
	uint8_t VTG_interval;
	uint8_t ZDA_interval;
	uint8_t attributes;
} ConfigureNmeaMessage;

typedef struct _ConfigureGNSSNavigationMode{
    uint8_t messageId;
    uint8_t messageSubId;
    uint8_t navigationMode;
    uint8_t attributes;
} ConfigureGNSSNavigationMode;

typedef struct _ConfigureMessageType{
	uint8_t messageId;
	uint8_t type;
	uint8_t attributes;
} ConfigureMessageType;

typedef struct _ConfigureNavigationDataMessageInterval{
	uint8_t messageId;
	uint8_t navigationMessageInterval;
	uint8_t attributes;
} ConfigureNavigationDataMessageInterval;

typedef struct _NavigationDataMessage{
	uint8_t messageId;
	uint8_t fixMode;
	uint8_t satellitesInFix;
	uint16_t GNSS_week;
	uint32_t GNSS_timeOfWeek;
	int32_t latitude;
	int32_t longitude;
	uint32_t ellipsoid_altitidue;
	uint32_t mean_sea_level_altitude;
	uint16_t GDOP;
	uint16_t PDOP;
	uint16_t HDOP;
	uint16_t VDOP;
	uint16_t TDOP;
	int32_t ECEF_x;
	int32_t ECEF_y;
	int32_t ECEF_z;
	int32_t ECEF_vx;
	int32_t ECEF_vy;
	int32_t ECEF_vz;
} __attribute__((__packed__)) NavigationDataMessage;

typedef struct _GpsMessage{
	uint16_t payloadLength;
	union{
		uint8_t payload[MAX_PAYLOAD_LEN];
		uint8_t messageId;
		Ack ackMsg;
		Nack nackMsg;
		SetFactoryDefaults setFactoryDefaultsMsg;
		QuerySwVersion querySoftwareVersionMsg;
		QueryPositionUpdateRate queryPositionUpdateRate;
		ConfigurePositionUpdateRate configurePositionUpdateRate;
		ConfigureSerialPort configureSerialPort;
		PositionUpdateRate positionUpdateRate;
		ConfigureNmeaMessage configureNmeaMessage;
		ConfigureGNSSNavigationMode configureGnssNavigationMode;
		ConfigureMessageType configureMessageType;
		ConfigureNavigationDataMessageInterval configureNavigationDataMessageInterval;
		NavigationDataMessage navigationDataMessage;

	};
	uint8_t checksum;
}  __attribute__((__packed__)) GpsMessage;

typedef enum{
	GPS_COMMAND_FAIL = 0,
	GPS_COMMAND_SUCCESS
} gps_cmd_result_t;

static uint8_t calculateChecksum(GpsMessage *msg){
	uint8_t checksum = 0;
	if (msg){
		uint16_t len = msg->payloadLength;
		if (len <= MAX_PAYLOAD_LEN){
			uint8_t *payload = msg->payload;
			for (size_t i = 0; i < len; i++){
				checksum ^= payload[i];
			}
		}
	}
	return checksum;
}

static void txGpsMessage(GpsMessage *msg, Serial *serial){
	serial->put_c(0xA0);
	serial->put_c(0xA1);

	uint16_t payloadLength = msg->payloadLength;
	serial->put_c((uint8_t)payloadLength >> 8);
	serial->put_c((uint8_t)payloadLength & 0xFF);

	uint8_t *payload = msg->payload;
	while(payloadLength--){
		serial->put_c(*(payload++));
	}

	serial->put_c(msg->checksum);

	serial->put_c(0x0D);
	serial->put_c(0x0A);
}

static gps_msg_result_t rxGpsMessage(GpsMessage *msg, Serial *serial, uint8_t expectedMessageId){

	gps_msg_result_t result = GPS_MSG_NONE;
	size_t timeoutLen = msToTicks(GPS_MSG_RX_WAIT_MS);
	size_t timeoutStart = xTaskGetTickCount();

	while (result == GPS_MSG_NONE){
		uint8_t som1 = 0, som2 = 0;
		if (serial_read_byte(serial, &som1, timeoutLen) && serial_read_byte(serial, &som2, timeoutLen)){
			if (som1 == 0xA0 && som2 == 0xA1){
				uint8_t len_h = 0, len_l = 0;
				size_t len_hb = serial_read_byte(serial, &len_h, timeoutLen);
				size_t len_lb = serial_read_byte(serial, &len_l, timeoutLen);
				if (!(len_hb && len_lb)){
					result = GPS_MSG_TIMEOUT;
					break;
				}
				uint16_t len = (len_h << 8) + len_l;

				if (len <= MAX_PAYLOAD_LEN){
					msg->payloadLength = len;
					uint8_t c = 0;
					for (size_t i = 0; i < len; i++){
						if (! serial_read_byte(serial, &c, timeoutLen)){
							result = GPS_MSG_TIMEOUT;
							break;
						}
						msg->payload[i] = c;
					}
				}
				uint8_t checksum = 0;
				if (! serial_read_byte(serial, &checksum, timeoutLen)){
					result = GPS_MSG_TIMEOUT;
					break;
				}
				uint8_t calculatedChecksum = calculateChecksum(msg);
				if (calculatedChecksum == checksum){
					uint8_t eos1 = 0, eos2 = 0;
					if (! (serial_read_byte(serial, &eos1, timeoutLen) && serial_read_byte(serial, &eos2, timeoutLen))){
						result = GPS_MSG_TIMEOUT;
						break;
					}
					if (eos1 == 0x0D && eos2 == 0x0A && msg->messageId == expectedMessageId){
						result = GPS_MSG_SUCCESS;
					}
					else{
						pr_info_int(msg->messageId);
						pr_info("unexpected id\r\n");
					}
				}
			}
		}
		if (isTimeoutMs(timeoutStart, GPS_MSG_RX_WAIT_MS)){
			result = GPS_MSG_TIMEOUT;
		}
	}
	pr_info_int(msg->messageId);
	pr_info(" message id received\r\n");
	return result;
}

static void sendSetFactoryDefaults(GpsMessage *gpsMsg, Serial *serial){
	gpsMsg->messageId = MSG_ID_SET_FACTORY_DEFAULTS;
	gpsMsg->setFactoryDefaultsMsg.type = 0x01;
	gpsMsg->checksum = calculateChecksum(gpsMsg);
	txGpsMessage(gpsMsg, serial);
}

static void sendQuerySwVersion(GpsMessage * gpsMsg, Serial * serial){
	gpsMsg->messageId = MSG_ID_QUERY_SW_VERSION;
	gpsMsg->querySoftwareVersionMsg.softwareType = 0x00;
	gpsMsg->payloadLength = sizeof(QuerySwVersion);
	gpsMsg->checksum = calculateChecksum(gpsMsg);
	txGpsMessage(gpsMsg, serial);
}

static void sendQueryPositionUpdateRate(GpsMessage *gpsMsg, Serial *serial){
	gpsMsg->messageId = MSG_ID_QUERY_POSITION_UPDATE_RATE;
	gpsMsg->payloadLength = sizeof(QueryPositionUpdateRate);
	gpsMsg->checksum = calculateChecksum(gpsMsg);
	txGpsMessage(gpsMsg, serial);
}

static void sendConfigureSerialPort(GpsMessage *gpsMsg, Serial *serial, uint8_t baudRateCode){
	gpsMsg->messageId = MSG_ID_CONFIGURE_SERIAL_PORT;
	gpsMsg->configureSerialPort.baudRateCode = baudRateCode;
	gpsMsg->configureSerialPort.comPort = 0;
	gpsMsg->configureSerialPort.attributes = ATTRIBUTE_UPDATE_TO_SRAM;
	gpsMsg->payloadLength = sizeof(ConfigureSerialPort);
	gpsMsg->checksum = calculateChecksum(gpsMsg);
	txGpsMessage(gpsMsg, serial);
}

static void sendDisableNmea(GpsMessage *gpsMsg, Serial *serial){
	gpsMsg->messageId = MSG_ID_CONFIGURE_NMEA_MESSAGE;
	gpsMsg->configureNmeaMessage.GGA_interval = 0;
	gpsMsg->configureNmeaMessage.GSA_interval = 0;
	gpsMsg->configureNmeaMessage.GSV_interval = 0;
	gpsMsg->configureNmeaMessage.GLL_interval = 0;
	gpsMsg->configureNmeaMessage.RMC_interval = 0;
	gpsMsg->configureNmeaMessage.VTG_interval = 0;
	gpsMsg->configureNmeaMessage.ZDA_interval = 0;
	gpsMsg->configureSerialPort.attributes = ATTRIBUTE_UPDATE_TO_SRAM;
	gpsMsg->payloadLength = sizeof(ConfigureNmeaMessage);
	gpsMsg->checksum = calculateChecksum(gpsMsg);
	txGpsMessage(gpsMsg, serial);
}

static void sendConfigureNmea(GpsMessage *gpsMsg, Serial *serial){
	gpsMsg->messageId = MSG_ID_CONFIGURE_NMEA_MESSAGE;
	gpsMsg->configureNmeaMessage.GGA_interval = GGA_INTERVAL;
	gpsMsg->configureNmeaMessage.GSA_interval = GSA_INTERVAL;
	gpsMsg->configureNmeaMessage.GSV_interval = GSV_INTERVAL;
	gpsMsg->configureNmeaMessage.GLL_interval = GLL_INTERVAL;
	gpsMsg->configureNmeaMessage.RMC_interval = RMC_INTERVAL;
	gpsMsg->configureNmeaMessage.VTG_interval = VTG_INTERVAL;
	gpsMsg->configureNmeaMessage.ZDA_interval = ZDA_INTERVAL;
	gpsMsg->configureSerialPort.attributes = ATTRIBUTE_UPDATE_TO_SRAM;
	gpsMsg->payloadLength = sizeof(ConfigureNmeaMessage);
	gpsMsg->checksum = calculateChecksum(gpsMsg);
	txGpsMessage(gpsMsg, serial);
}

static void sendConfigureGnssNavigationMode(GpsMessage *gpsMsg, Serial *serial, uint8_t navigationMode){
    gpsMsg->messageId = MSG_ID_CONFIGURE_GNSS_NAVIGATION_MODE;
    gpsMsg->configureGnssNavigationMode.messageSubId = MSG_SUBID_CONFIGURE_GNSS_NAVIGATION_MODE;
    gpsMsg->configureGnssNavigationMode.navigationMode = navigationMode;
    gpsMsg->configureGnssNavigationMode.attributes = ATTRIBUTE_UPDATE_TO_SRAM;
    gpsMsg->payloadLength = sizeof(ConfigureGNSSNavigationMode);
    gpsMsg->checksum = calculateChecksum(gpsMsg);
    txGpsMessage(gpsMsg, serial);
}

static void sendConfigureMessageType(GpsMessage *gpsMsg, Serial *serial, uint8_t messageType){
	gpsMsg->messageId = MSG_ID_CONFIGURE_MESSAGE_TYPE;
	gpsMsg->configureMessageType.type = messageType;
	gpsMsg->configureMessageType.attributes = ATTRIBUTE_UPDATE_TO_SRAM;
	gpsMsg->payloadLength = sizeof(ConfigureMessageType);
	gpsMsg->checksum = calculateChecksum(gpsMsg);
	txGpsMessage(gpsMsg, serial);
}

static void sendConfigureNavigationDataMessageInterval(GpsMessage *gpsMsg, Serial *serial, uint8_t interval){
	gpsMsg->messageId = MSG_ID_CONFIGURE_NAVIGATION_DATA_MESSAGE_INTERVAL;
	gpsMsg->configureNavigationDataMessageInterval.navigationMessageInterval = interval;
	gpsMsg->configureNavigationDataMessageInterval.attributes = ATTRIBUTE_UPDATE_TO_SRAM;
	gpsMsg->payloadLength = sizeof(ConfigureNavigationDataMessageInterval);
	gpsMsg->checksum = calculateChecksum(gpsMsg);
	txGpsMessage(gpsMsg, serial);
}

static void sendConfigurePositionUpdateRate(GpsMessage *gpsMsg, Serial *serial, uint8_t updateRate){
	gpsMsg->messageId = MSG_ID_CONFIGURE_POSITION_UPDATE_RATE;
	gpsMsg->configurePositionUpdateRate.rate = updateRate;
	gpsMsg->configurePositionUpdateRate.attributes = ATTRIBUTE_UPDATE_TO_SRAM;
	gpsMsg->payloadLength = sizeof(ConfigurePositionUpdateRate);
	gpsMsg->checksum = calculateChecksum(gpsMsg);
	txGpsMessage(gpsMsg, serial);
}

uint32_t detectGpsBaudRate(GpsMessage *gpsMsg, Serial *serial){
	BaudRateCodes baud_rates[BAUD_RATE_COUNT] = BAUD_RATES;

	for (size_t i = 0; i < BAUD_RATE_COUNT; i++){
		uint32_t baudRate = baud_rates[i].baud;
		pr_info("GPS: probing baud rate ");
		pr_info_int(baudRate);
		pr_info("\r\n");
		configure_serial(SERIAL_GPS, 8, 0, 1, baudRate);
		sendQuerySwVersion(gpsMsg, serial);
		if (rxGpsMessage(gpsMsg, serial, MSG_ID_SW_VERSION) == GPS_MSG_SUCCESS){
			return baudRate;
		}
	}
	return 0;
}

static gps_cmd_result_t attemptFactoryDefaults(GpsMessage *gpsMsg, Serial *serial){
	BaudRateCodes baud_rates[BAUD_RATE_COUNT] = BAUD_RATES;

	for (size_t i = 0; i < BAUD_RATE_COUNT; i++){
		uint32_t baudRate = baud_rates[i].baud;
		pr_info("attempting factory defaults at ");
		pr_info_int(baudRate);
		pr_info("\r\n");
		configure_serial(SERIAL_GPS, 8, 0, 1, baudRate);
		serial->flush();
		sendSetFactoryDefaults(gpsMsg, serial);
		if (rxGpsMessage(gpsMsg, serial, MSG_ID_ACK) == GPS_MSG_SUCCESS && gpsMsg->ackMsg.messageId == MSG_ID_SET_FACTORY_DEFAULTS){
			pr_info("Set Factory Defaults Success\r\n");
			return GPS_COMMAND_SUCCESS;
		}
	}
	return GPS_COMMAND_FAIL;
}

static uint8_t getBaudRateCode(uint32_t baudRate){
	BaudRateCodes baud_rates[BAUD_RATE_COUNT] = BAUD_RATES;
	for (size_t i = 0; i < BAUD_RATE_COUNT; i++){
		if (baudRate == baud_rates[i].baud) return baud_rates[i].code;
	}
	return 0;
}

static gps_cmd_result_t configureBaudRate(GpsMessage *gpsMsg, Serial *serial, uint32_t targetBaudRate){
	pr_info("Configuring GPS baud rate to ");
	pr_info_int(targetBaudRate);
	pr_info(": ");
	gps_cmd_result_t result = GPS_COMMAND_FAIL;
	uint8_t baudRateCode = getBaudRateCode(targetBaudRate);
	sendConfigureSerialPort(gpsMsg, serial, baudRateCode);
	if (rxGpsMessage(gpsMsg, serial, MSG_ID_ACK) == GPS_MSG_SUCCESS){
		result = (gpsMsg->ackMsg.ackId == MSG_ID_CONFIGURE_SERIAL_PORT) ? GPS_COMMAND_SUCCESS : GPS_COMMAND_FAIL;
	}
	pr_info(result == GPS_COMMAND_SUCCESS ? "win\r\n" : "fail\r\n");
	return result;
}

static gps_cmd_result_t configureNmeaMessages(GpsMessage *gpsMsg, Serial *serial){
	pr_info("GPS: Configuring NMEA messages: ");

	gps_cmd_result_t result = GPS_COMMAND_FAIL;
	sendConfigureNmea(gpsMsg, serial);
	if (rxGpsMessage(gpsMsg, serial, MSG_ID_ACK) == GPS_MSG_SUCCESS){
		result = (gpsMsg->ackMsg.ackId == MSG_ID_CONFIGURE_NMEA_MESSAGE) ? GPS_COMMAND_SUCCESS : GPS_COMMAND_FAIL;
	}
	pr_info(result == GPS_COMMAND_SUCCESS ? "win\r\n" : "fail\r\n");
	return result;
}

static gps_cmd_result_t configureGnssNavigationMode(GpsMessage *gpsMsg, Serial *serial){
    pr_info("GPS: Configuring Gnss Navigation Mode: ");

    gps_cmd_result_t result = GPS_COMMAND_FAIL;
    sendConfigureGnssNavigationMode(gpsMsg, serial, GNSS_NAVIGATION_MODE);
    if (rxGpsMessage(gpsMsg, serial, MSG_ID_ACK) == GPS_MSG_SUCCESS){
        result = (gpsMsg->ackMsg.ackId == MSG_ID_CONFIGURE_GNSS_NAVIGATION_MODE) ? GPS_COMMAND_SUCCESS : GPS_COMMAND_FAIL;
    }
    pr_info(result == GPS_COMMAND_SUCCESS ? "win\r\n" : "fail\r\n");
    return result;
}

static gps_cmd_result_t disableNmeaMessages(GpsMessage *gpsMsg, Serial *serial){
	pr_info("GPS: Disable NMEA messages: ");

	gps_cmd_result_t result = GPS_COMMAND_FAIL;
	sendDisableNmea(gpsMsg, serial);
	if (rxGpsMessage(gpsMsg, serial, MSG_ID_ACK) == GPS_MSG_SUCCESS){
		result = (gpsMsg->ackMsg.ackId == MSG_ID_CONFIGURE_NMEA_MESSAGE) ? GPS_COMMAND_SUCCESS : GPS_COMMAND_FAIL;
	}
	pr_info(result == GPS_COMMAND_SUCCESS ? "win\r\n" : "fail\r\n");
	return result;
}

static gps_cmd_result_t configureMessageType(GpsMessage *gpsMsg, Serial *serial){
	pr_info("GPS: configure message type: ");
	gps_cmd_result_t result = GPS_COMMAND_FAIL;
	sendConfigureMessageType(gpsMsg, serial, MESSAGE_TYPE_BINARY);
	if (rxGpsMessage(gpsMsg, serial, MSG_ID_ACK) == GPS_MSG_SUCCESS){
		result = (gpsMsg->ackMsg.ackId == MSG_ID_CONFIGURE_MESSAGE_TYPE) ? GPS_COMMAND_SUCCESS : GPS_COMMAND_FAIL;
	}
	pr_info(result == GPS_COMMAND_SUCCESS ? "win\r\n" : "fail\r\n");
	return result;
}

static gps_cmd_result_t configureNavigationDataMessageInterval(GpsMessage *gpsMsg, Serial *serial){
	pr_info("GPS: configure navigation data message interval: ");
	gps_cmd_result_t result = GPS_COMMAND_FAIL;
	sendConfigureNavigationDataMessageInterval(gpsMsg, serial, NAVIGATION_DATA_MESSAGE_INTERVAL);
	if (rxGpsMessage(gpsMsg, serial, MSG_ID_ACK) == GPS_MSG_SUCCESS){
		result = (gpsMsg->ackMsg.ackId == MSG_ID_CONFIGURE_NAVIGATION_DATA_MESSAGE_INTERVAL) ? GPS_COMMAND_SUCCESS : GPS_COMMAND_FAIL;
	}
	pr_info(result == GPS_COMMAND_SUCCESS ? "win\r\n" : "fail\r\n");
	return result;
}

static uint8_t queryPositionUpdateRate(GpsMessage *gpsMsg, Serial *serial){
	uint8_t updateRate = 0;
	sendQueryPositionUpdateRate(gpsMsg, serial);
	if (rxGpsMessage(gpsMsg, serial, MSG_ID_POSITION_UPDATE_RATE) == GPS_MSG_SUCCESS){
		updateRate = gpsMsg->positionUpdateRate.rate;
	}
	return updateRate;
}

static gps_cmd_result_t configureUpdateRate(GpsMessage *gpsMsg, Serial *serial, uint8_t targetUpdateRate){
	pr_info("Configuring GPS update rate to ");
	pr_info_int(targetUpdateRate);
	pr_info(": ");
	gps_cmd_result_t result = GPS_COMMAND_FAIL;
	sendConfigurePositionUpdateRate(gpsMsg, serial, targetUpdateRate);
	if (rxGpsMessage(gpsMsg, serial, MSG_ID_ACK) == GPS_MSG_SUCCESS){
		result = (gpsMsg->ackMsg.ackId == MSG_ID_CONFIGURE_POSITION_UPDATE_RATE) ? GPS_COMMAND_SUCCESS : GPS_COMMAND_FAIL;
	}
	pr_info(result == GPS_COMMAND_SUCCESS ? "win\r\n" : "fail\r\n");
	return result;
}

GpsMessage gpsMsg;

static uint8_t getTargetUpdateRate(uint8_t sampleRate){
    if (sampleRate > 25){
        return 50;
    }
    else if (sampleRate > 10){
        return 25;
    }
    else if (sampleRate > 5){
        return 10;
    }
    else if (sampleRate > 1){
        return 5;
    }
    return 1;
}

int GPS_device_provision(uint8_t sampleRate, Serial *serial){
    size_t attempts = MAX_PROVISIONING_ATTEMPTS;
	size_t provisioned = 0;

	vTaskDelay(msToTicks(500));
	while(attempts-- && !provisioned){
	    while(1){
	        pr_info("GPS: provisioning attempt\r\n");
			uint32_t baudRate = detectGpsBaudRate(&gpsMsg, serial);
			if (baudRate){
			    pr_info("GPS: module detected at ");
				pr_info_int(baudRate);
				pr_info("\r\n");
				if (baudRate != TARGET_BAUD_RATE && configureBaudRate(&gpsMsg, serial, TARGET_BAUD_RATE) == GPS_COMMAND_FAIL){
					pr_error("GPS: Error provisioning - could not configure baud rate\r\n");
					break;
				}
				configure_serial(SERIAL_GPS, 8, 0, 1, TARGET_BAUD_RATE);
				serial->flush();

				uint8_t targetUpdateRate = getTargetUpdateRate(sampleRate);
				uint8_t currentUpdateRate = queryPositionUpdateRate(&gpsMsg, serial);
				if (!currentUpdateRate){
					pr_error("GPS: Error provisioning - could not detect update rate\r\n");
					currentUpdateRate = 0;
				}
				if (currentUpdateRate != targetUpdateRate && configureUpdateRate(&gpsMsg, serial, targetUpdateRate) == GPS_COMMAND_FAIL){
					pr_error("GPS: Error provisioning - could not configure update rate\r\n");
					break;
				}

				if (configureNmeaMessages(&gpsMsg, serial) == GPS_COMMAND_FAIL){
					pr_error("GPS: Error provisioning - could not configure NMEA messages\r\n");
					break;
				}

				if (configureMessageType(&gpsMsg, serial) == GPS_COMMAND_FAIL){
					pr_error("GPS: Error provisioning - could not set binary message mode\r\n");
					break;
				}

				if (configureNavigationDataMessageInterval(&gpsMsg, serial) == GPS_COMMAND_FAIL){
					pr_error("GPS: Error provisioning - could not set navigation data message interval\r\n");
					break;
				}

                if (configureGnssNavigationMode(&gpsMsg, serial) == GPS_COMMAND_FAIL){
                    pr_error("GPS: Error provisioning - could not set navigation mode\r\n");
                    break;
                }

				if (disableNmeaMessages(&gpsMsg, serial) == GPS_COMMAND_FAIL){
					pr_error("GPS: Error provisioning - could not disable NMEA messages\r\n");
					break;
				}

				pr_info("GPS: provisioned\r\n");
				provisioned = 1;
				break;
			}
			else{
				pr_error("GPS: Error provisioning - could not detect GPS module on known baud rates\r\n");
				break;
			}
		}
		if (!provisioned && attempts == MAX_PROVISIONING_ATTEMPTS / 2){
			attemptFactoryDefaults(&gpsMsg, serial);
		}
	}
	return provisioned;
}

gps_msg_result_t GPS_device_get_update(GpsSample *gpsSample, Serial *serial){
   gps_msg_result_t result = rxGpsMessage(&gpsMsg, serial, MSG_ID_NAVIGATION_DATA_MESSAGE);

   if (result != GPS_MSG_SUCCESS) return result;

   gpsSample->quality = gpsMsg.navigationDataMessage.fixMode;
   gpsSample->satellites = gpsMsg.navigationDataMessage.satellitesInFix;

   int32_t latitude_raw = swap_int32(gpsMsg.navigationDataMessage.latitude);
   int32_t longitude_raw = swap_int32(gpsMsg.navigationDataMessage.longitude);
   gpsSample->point.latitude = ((float)latitude_raw) * 0.0000001f;
   gpsSample->point.longitude = ((float)longitude_raw) * 0.0000001f;
   //gpsSample->altitude =((float)gpsMsg.navigationDataMessage.ellipsoid_altitidue) * 0.01;

   float ecef_x_velocity = ((float)swap_int32(gpsMsg.navigationDataMessage.ECEF_vx)) * 0.01;
   float ecef_y_velocity = ((float)swap_int32(gpsMsg.navigationDataMessage.ECEF_vy)) * 0.01;
   float ecef_z_velocity = ((float)swap_int32(gpsMsg.navigationDataMessage.ECEF_vz)) * 0.01;

   float velocity = sqrt((ecef_x_velocity * ecef_x_velocity)
                         + (ecef_y_velocity * ecef_y_velocity)
                         + (ecef_z_velocity * ecef_z_velocity));
   //convert m/sec to km/hour
   gpsSample->speed = velocity * 3.6;
   gpsSample->altitude = (float)gpsMsg.navigationDataMessage.ellipsoid_altitidue * 0.01;

   //convert GNSS_week to milliseconds and add time of week converted to milliseconds
   uint16_t GNSS_week = swap_uint16(gpsMsg.navigationDataMessage.GNSS_week);
   uint32_t timeOfWeekMillis = swap_uint32(gpsMsg.navigationDataMessage.GNSS_timeOfWeek) * 10;
   millis_t time = (((uint64_t)GNSS_week * 60 * 60 * 24 * 7) * 1000) + timeOfWeekMillis;
   //adjust for Jan 6 1980 GNSS epoch
   time += (uint64_t)GNSS_EPOCH_IN_UNIX_EPOCH * 1000;
   gpsSample->time = time;

   return GPS_MSG_SUCCESS;
}
