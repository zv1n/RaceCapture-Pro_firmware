/*
 * loggerData.c
 *
 *  Created on: Jun 1, 2012
 *      Author: brent
 */

#include "loggerData.h"
#include "loggerHardware.h"
#include "accelerometer.h"
#include "gps.h"


static void writeAccelerometer(SampleRecord *sampleRecord, size_t currentTicks, LoggerConfig *config){

	for (unsigned int i=0; i < CONFIG_ACCEL_CHANNELS;i++){
		AccelConfig *ac = &(config->AccelConfigs[i]);
		size_t sr = ac->cfg.sampleRate;
		if (sr != SAMPLE_DISABLED && (currentTicks % sr) == 0){
			sampleRecord->AccelSamples[i].floatValue = readAccelerometer(i, ac);
		}
	}
}

static void writeADC(SampleRecord *sampleRecord, size_t currentTicks, LoggerConfig *config){

	unsigned int adc[CONFIG_ADC_CHANNELS];
	readAllADC(&adc[0],&adc[1],&adc[2],&adc[3],&adc[4],&adc[5],&adc[6],&adc[7]);

	for (unsigned int i=0; i < CONFIG_ADC_CHANNELS;i++){
		ADCConfig *ac = &(config->ADCConfigs[i]);
		size_t sr = ac->cfg.sampleRate;
		if (sr != SAMPLE_DISABLED){
			if ((currentTicks % sr) == 0){
				float analogValue = 0;
				switch(ac->scalingMode){
				case SCALING_MODE_RAW:
					analogValue = adc[i];
					break;
				case SCALING_MODE_LINEAR:
					analogValue = (ac->linearScaling * (float)adc[i]);
					break;
				case SCALING_MODE_MAP:
					analogValue = GetMappedValue((float)adc[i],&(ac->scalingMap));
					break;
				}
				if (ac->loggingPrecision == 0){
					sampleRecord->ADCSamples[i].intValue = analogValue;
				}else{
					sampleRecord->ADCSamples[i].floatValue = analogValue;
				}
			}
		}
	}
}

static void writeGPSChannels(SampleRecord *sampleRecord, size_t currentTicks, GPSConfig *config){

	{
		size_t sr = config->timeCfg.sampleRate;
		if (sr != SAMPLE_DISABLED){
			if ((currentTicks % sr) == 0) sampleRecord->GPS_TimeSample.floatValue = getUTCTime();
		}
	}
	{
		//latitude sample rate is a stand-in for position sample rate
		size_t sr = config->latitudeCfg.sampleRate;
		if (sr != SAMPLE_DISABLED){
			if ((currentTicks % sr) == 0){
				sampleRecord->GPS_LatitueSample.floatValue = getLatitude();
				sampleRecord->GPS_LongitudeSample.floatValue = getLongitude();
			}
		}
	}

	{
		size_t sr = config->satellitesCfg.sampleRate;
		if (sr != SAMPLE_DISABLED){
			if ((currentTicks % sr) == 0) sampleRecord->GPS_SatellitesSample.intValue = getSatellitesUsedForPosition();
		}
	}

	{
		size_t sr = config->speedCfg.sampleRate;
		if (sr != SAMPLE_DISABLED){
			if ((currentTicks % sr) == 0) sampleRecord->GPS_SpeedSample.floatValue = getGPSSpeed();
		}
	}
}

static void writeTrackChannels(SampleRecord *sampleRecord, size_t currentTicks, TrackConfig *config){
	{
		size_t sr = config->lapCountCfg.sampleRate;
		if (sr != SAMPLE_DISABLED){
			if ((currentTicks % sr) == 0) sampleRecord->Track_LapCountSample.intValue = getLapCount();
		}
	}
	{
		size_t sr = config->splitTimeCfg.sampleRate;
		if (sr != SAMPLE_DISABLED){
			if ((currentTicks % sr) == 0) sampleRecord->Track_SplitTimeSample.floatValue = getLastSplitTime();
		}
	}
	{
		size_t sr = config->lapTimeCfg.sampleRate;
		if (sr != SAMPLE_DISABLED){
			if ((currentTicks % sr) == 0) sampleRecord->Track_LapTimeSample.floatValue = getLastLapTime();
		}
	}
	{
		size_t sr = config->distanceCfg.sampleRate;
		if (sr != SAMPLE_DISABLED){
			if ((currentTicks % sr) == 0) sampleRecord->Track_DistanceSample.floatValue = getDistance();
		}
	}
	{
		size_t sr = config->predTimeCfg.sampleRate;
		if (sr != SAMPLE_DISABLED){
			if ((currentTicks % sr) == 0) sampleRecord->Track_PredTimeSample.floatValue = 0; //getPredTime();
		}
	}
}

static void writeGPIOs(SampleRecord *sampleRecord, size_t currentTicks, LoggerConfig *loggerConfig){

	unsigned int gpio[CONFIG_GPIO_CHANNELS];
	readGpios(&gpio[0], &gpio[1], &gpio[2]);

	for (int i = 0; i < CONFIG_GPIO_CHANNELS; i++){
		GPIOConfig *c = &(loggerConfig->GPIOConfigs[i]);
		size_t sr = c->cfg.sampleRate;
		if (sr != SAMPLE_DISABLED){
			if ((currentTicks % sr) == 0) sampleRecord->GPIOSamples[i].intValue = gpio[i];
		}
	}
}

static void writeTimerChannels(SampleRecord *sampleRecord, size_t currentTicks, LoggerConfig *loggerConfig){

	unsigned int timers[CONFIG_TIMER_CHANNELS];
	getAllTimerPeriods(timers,timers + 1,timers + 2);

	for (int i = 0; i < CONFIG_TIMER_CHANNELS; i++){
		TimerConfig *c = &(loggerConfig->TimerConfigs[i]);
		size_t sr = c->cfg.sampleRate;
		if (sr != SAMPLE_DISABLED){
			if ((currentTicks % sr) == 0){
				int value = 0;
				unsigned scaling = c->calculatedScaling;
				unsigned int timerValue = timers[i];
				switch (c->mode){
					case MODE_LOGGING_TIMER_RPM:
						value = TIMER_PERIOD_TO_RPM(timerValue, scaling);
						break;
					case MODE_LOGGING_TIMER_FREQUENCY:
						value = TIMER_PERIOD_TO_HZ(timerValue, scaling);
						break;
					case MODE_LOGGING_TIMER_PERIOD_MS:
						value = TIMER_PERIOD_TO_MS(timerValue, scaling);
						break;
					case MODE_LOGGING_TIMER_PERIOD_USEC:
						value = TIMER_PERIOD_TO_USEC(timerValue, scaling);
						break;
					default:
						value = -1;
						break;
				}
				if (c->loggingPrecision == 0){
					sampleRecord->TimerSamples[i].intValue = value;
				}
				else{
					sampleRecord->TimerSamples[i].floatValue = value;
				}
			}
		}
	}
}

static void writePWMChannels(SampleRecord *sampleRecord, size_t currentTicks, LoggerConfig *loggerConfig){

	for (int i = 0; i < CONFIG_PWM_CHANNELS; i++){
		PWMConfig *c = &(loggerConfig->PWMConfigs[i]);
		size_t sr = c->cfg.sampleRate;
		if (sr != SAMPLE_DISABLED){
			if ((currentTicks % sr) == 0){
				float value = 0;
				switch (c->loggingMode){
					case MODE_LOGGING_PWM_PERIOD:
						value = PWM_GetPeriod(i);
						break;
					case MODE_LOGGING_PWM_DUTY:
						value = PWM_GetDutyCycle(i);
						break;
					case MODE_LOGGING_PWM_VOLTS:
						value =  PWM_GetDutyCycle(i) * c->voltageScaling;
						break;
					default:
						break;
				}
				if (c->loggingPrecision == 0){
					sampleRecord->PWMSamples[i].intValue = value;
				}
				else{
					sampleRecord->PWMSamples[i].floatValue = value;
				}
			}
		}
	}
}


void populateSampleRecord(SampleRecord *sr, size_t currentTicks, LoggerConfig *config){

	//perform logging tasks
	unsigned int gpsInstalled = (unsigned int)config->GPSConfigs.GPSInstalled;
	unsigned int accelInstalled = (unsigned int)config->AccelInstalled;

	//Write ADC channels
	writeADC(sr, currentTicks, config);
	//Write GPIO channels
	writeGPIOs(sr,currentTicks, config);
	//Write Timer channels
	writeTimerChannels(sr,currentTicks, config);
	//Write PWM channels
	writePWMChannels(sr,currentTicks, config);

	//Optional hardware
	//Write Accelerometer
	if (accelInstalled) writeAccelerometer(sr,currentTicks, config);
	//Write GPS
	if (gpsInstalled){
		writeGPSChannels(sr,currentTicks, &(config->GPSConfigs));
		writeTrackChannels(sr, currentTicks, &(config->TrackConfig));
	}

}


//linear interpolation routine
//            (y2 - y1)
//  y = y1 +  --------- * (x - x1)
//            (x2 - x1)
static float LinearInterpolate(float x, float x1, float y1, float x2, float y2){
	return y1 + (((y2 - y1))  / (x2 - x1)) * (x - x1);
}


float GetMappedValue(float value, ScalingMap *scalingMap){
	unsigned short *bins;
	unsigned int bin, nextBin;

	bins = scalingMap->rawValues + ANALOG_SCALING_BINS - 1;
	bin = nextBin = ANALOG_SCALING_BINS - 1;

	while (value < *bins && bin > 0){
		bins--;
		bin--;
	}
	if (bin == 0 && value < *bins){
		return scalingMap->scaledValues[0];
	}
	else{
		nextBin = bin;
		if (bin < ANALOG_SCALING_BINS - 1){
			nextBin++;
		}
		else{
			return scalingMap->scaledValues[ANALOG_SCALING_BINS - 1];
		}
	}
	float x1 = (float)scalingMap->rawValues[bin];
	float y1 = scalingMap->scaledValues[bin];
	float x2 = (float)scalingMap->rawValues[nextBin];
	float y2 = scalingMap->scaledValues[nextBin];
	float scaled = LinearInterpolate(value,x1,y1,x2,y2);
	return scaled;
}
