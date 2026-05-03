/*****************************************************************************
* | File      	:   PCF85063.c
* | Author      :   Waveshare team
* | Function    :   PCF85063 driver
* | Info        :
*----------------
* |	This version:   V1.0
* | Date        :   2024-02-02
* | Info        :   Basic version
*
******************************************************************************/
#include "PCF85063.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

datetime_t datetime = {0};

static SemaphoreHandle_t s_pcf85063_mutex;

static uint8_t decToBcd(int val);
static int bcdToDec(uint8_t val);

static void pcf85063_lock(void)
{
    configASSERT(s_pcf85063_mutex != NULL);
    (void)xSemaphoreTake(s_pcf85063_mutex, portMAX_DELAY);
}

static void pcf85063_unlock(void)
{
    (void)xSemaphoreGive(s_pcf85063_mutex);
}

uint8_t PCF85063_Weekday_Sunday0(uint16_t year, uint8_t month, uint8_t day)
{
    if (month < 1U || month > 12U) {
        return 0U;
    }
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int y = (int)year;
    int m = (int)month;
    int d = (int)day;
    y -= (m < 3) ? 1 : 0;
    int w = y + y / 4 - y / 100 + y / 400 + t[m - 1] + d;
    w %= 7;
    if (w < 0) {
        w += 7;
    }
    return (uint8_t)w;
}

const unsigned char MonthStr[12][4] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov","Dec"};
/******************************************************************************
function:	PCF85063 initialized
parameter:
            
Info:Initiate Normal Mode, RTC Run, NO reset, No correction , 24hr format, Internal load capacitane 12.5pf
******************************************************************************/
void PCF85063_Init()
{
	if (s_pcf85063_mutex == NULL) {
		s_pcf85063_mutex = xSemaphoreCreateMutex();
		configASSERT(s_pcf85063_mutex != NULL);
	}

	pcf85063_lock();
	uint8_t Value = RTC_CTRL_1_DEFAULT|RTC_CTRL_1_CAP_SEL;

	ESP_ERROR_CHECK(I2C_Write(PCF85063_ADDRESS, RTC_CTRL_1_ADDR, &Value, 1));
	pcf85063_unlock();

	// datetime_t Now_datetime= {0};
	// Now_datetime.year = 2024;
	// Now_datetime.month = 9;
	// Now_datetime.day = 20;
	// Now_datetime.dotw = 5;
	// Now_datetime.hour = 9;
	// Now_datetime.minute = 50;
	// Now_datetime.second = 0;
	// PCF85063_Set_All(Now_datetime);
}

void PCF85063_Loop(void)
{
  PCF85063_Read_Time(&datetime);
}
/******************************************************************************
function:	Reset PCF85063
parameter:
Info:		
******************************************************************************/
void PCF85063_Reset()
{
	pcf85063_lock();
	uint8_t Value = RTC_CTRL_1_DEFAULT|RTC_CTRL_1_CAP_SEL|RTC_CTRL_1_SR;
	ESP_ERROR_CHECK(I2C_Write(PCF85063_ADDRESS, RTC_CTRL_1_ADDR, &Value, 1));
	pcf85063_unlock();
}

/******************************************************************************
function:	Set Time 
parameter:
Info:		
******************************************************************************/
void PCF85063_Set_Time(datetime_t time)
{
	pcf85063_lock();
	uint8_t buf[3] = {decToBcd(time.second),
					  decToBcd(time.minute),
					  decToBcd(time.hour)};
	ESP_ERROR_CHECK(I2C_Write(PCF85063_ADDRESS, RTC_SECOND_ADDR, buf, 3));
	pcf85063_unlock();
}

/******************************************************************************
function:	Set Date
parameter:
Info:		
******************************************************************************/
void PCF85063_Set_Date(datetime_t date)
{
	pcf85063_lock();
	uint8_t buf[4] = {decToBcd(date.day),
					  decToBcd(date.dotw),
					  decToBcd(date.month),
					  decToBcd(date.year - YEAR_OFFSET)};
	ESP_ERROR_CHECK(I2C_Write(PCF85063_ADDRESS, RTC_DAY_ADDR, buf, 4));
	pcf85063_unlock();
}

/******************************************************************************
function:	Set Time And Date
parameter:
Info:		
******************************************************************************/
void PCF85063_Set_All(datetime_t time)
{
	pcf85063_lock();
	uint8_t buf[7] = {decToBcd(time.second),
					  decToBcd(time.minute),
					  decToBcd(time.hour),
					  decToBcd(time.day),
					  decToBcd(time.dotw),
					  decToBcd(time.month),
					  decToBcd(time.year - YEAR_OFFSET)};
	ESP_ERROR_CHECK(I2C_Write(PCF85063_ADDRESS, RTC_SECOND_ADDR, buf, 7));
	pcf85063_unlock();
}

/******************************************************************************
function:	Read Time And Date
parameter:
Info:		
******************************************************************************/
void PCF85063_Read_Time(datetime_t *time)
{
	pcf85063_lock();
	uint8_t buf[7] = {0};
	ESP_ERROR_CHECK(I2C_Read(PCF85063_ADDRESS, RTC_SECOND_ADDR, buf, 7));
	time->second = bcdToDec(buf[0] & 0x7F);
	time->minute = bcdToDec(buf[1] & 0x7F);
	time->hour = bcdToDec(buf[2] & 0x3F);
	time->day = bcdToDec(buf[3] & 0x3F);
	time->dotw = bcdToDec(buf[4] & 0x07);
	time->month = bcdToDec(buf[5] & 0x1F);
	time->year = bcdToDec(buf[6])+YEAR_OFFSET;
	pcf85063_unlock();
}

/******************************************************************************
function:	Enable Alarm and Clear Alarm flag
parameter:			
Info:		
******************************************************************************/
void PCF85063_Enable_Alarm()
{
	pcf85063_lock();
	uint8_t Value = RTC_CTRL_2_DEFAULT | RTC_CTRL_2_AIE;
	Value &= ~RTC_CTRL_2_AF;
	ESP_ERROR_CHECK(I2C_Write(PCF85063_ADDRESS, RTC_CTRL_2_ADDR, &Value, 1));
	pcf85063_unlock();
}

/******************************************************************************
function:	Get Alarm flay
parameter:			
Info:		
******************************************************************************/
uint8_t PCF85063_Get_Alarm_Flag()
{
	pcf85063_lock();
	uint8_t Value = 0;
	ESP_ERROR_CHECK(I2C_Read(PCF85063_ADDRESS, RTC_CTRL_2_ADDR, &Value, 1));
	//printf("Value = 0x%x",Value);
	Value &= RTC_CTRL_2_AF | RTC_CTRL_2_AIE;
	pcf85063_unlock();
	return Value;
}

/******************************************************************************
function:	Set Alarm
parameter:			
Info:		
******************************************************************************/
void PCF85063_Set_Alarm(datetime_t time)
{
	pcf85063_lock();
	uint8_t buf[5] ={
		decToBcd(time.second)&(~RTC_ALARM),
		decToBcd(time.minute)&(~RTC_ALARM),
		decToBcd(time.hour)&(~RTC_ALARM),
		//decToBcd(time.day)&(~RTC_ALARM),
		//decToBcd(time.dotw)&(~RTC_ALARM)
		RTC_ALARM, 	//disalbe day
		RTC_ALARM	//disalbe weekday
	};
	ESP_ERROR_CHECK(I2C_Write(PCF85063_ADDRESS, RTC_SECOND_ALARM, buf, 6));
	pcf85063_unlock();
}

/******************************************************************************
function:	
parameter:			
Info:		
******************************************************************************/
void PCF85063_Read_Alarm(datetime_t *time)
{
	pcf85063_lock();
	uint8_t bufss[6] = {0};
	ESP_ERROR_CHECK(I2C_Read(PCF85063_ADDRESS, RTC_SECOND_ALARM, bufss, 6));
	time->second = bcdToDec(bufss[0] & 0x7F);
	time->minute = bcdToDec(bufss[1] & 0x7F);
	time->hour = bcdToDec(bufss[2] & 0x3F);
	time->day = bcdToDec(bufss[3] & 0x3F);
	time->dotw = bcdToDec(bufss[4] & 0x07);
	pcf85063_unlock();
}


/******************************************************************************
function:	Convert normal decimal numbers to binary coded decimal
parameter:			
Info:		
******************************************************************************/
static uint8_t decToBcd(int val)
{
	return (uint8_t)((val / 10 * 16) + (val % 10));
}

/******************************************************************************
function:	Convert binary coded decimal to normal decimal numbers
parameter:			
Info:		
******************************************************************************/
static int bcdToDec(uint8_t val)
{
	return (int)((val / 16 * 10) + (val % 16));
}

/******************************************************************************
function:	
parameter:	
Info:		
******************************************************************************/
void datetime_to_str(char *datetime_str,datetime_t time)
{
	sprintf(datetime_str, " %d.%d.%d  %d %d:%d:%d ", time.year, time.month, 
			time.day, time.dotw, time.hour, time.minute, time.second);
} 