#include <JeeLib.h>
#include <GLCD_ST7565.h>
#include <avr/pgmspace.h>
//#include <string.h>
GLCD_ST7565 glcd;

#include <RTClib.h>                 // Real time clock (RTC) - used for software RTC to reset kWh counters at midnight
#include <Wire.h>                   // Part of Arduino libraries - needed for RTClib
RTC_Millis RTC;

#include "font_metric01.h"
#include "font_metric02.h"
#include "font_metric04.h"

#define DISPLAY_NODE  25         // Should be unique on network, node ID 30 reserved for base station
#define INTERNAL_NODE 19
#define EXTERNAL_NODE 22
#define POWER_NODE    10
#define BASE_NODE     15

#define RF_FREQ RF12_433MHZ     // frequency - match to same frequency as RFM12B module (change to 868Mhz or 915Mhz if appropriate)
#define GROUP 210 

typedef enum 
{
  ALIGN_LEFT,
  ALIGN_RIGHT,
  ALIGN_CENTRE,
  ALIGN_UNITS,
} align_t;

unsigned long fast_update, slow_update;

//---------------------------------------------------
// Data structures for transfering data between units
//---------------------------------------------------
typedef struct { int power1, power2, power3, Vrms; } PayloadTX;         // neat way of packaging data for RF comms
PayloadTX emontx;
float powerMax;
float powerMin;
bool powerValid = false;
unsigned long powerLastUpdate = 0;

typedef struct { int temp1, temp2, humidity, voltage; } PayloadTH;
PayloadTH internalth;
float internalTemp;
float internalTempMax;
float internalTempMin;
bool internalValid = false;
unsigned long internalLastUpdate = 0;

PayloadTH externalth;
float externalTemp;
float externalTempMax;
float externalTempMin;
bool externalValid = false;
unsigned long externalLastUpdate = 0;

typedef struct {char node, hour, minute; } PayloadBase;
PayloadBase base;
unsigned long baseLastUpdate = 0;

int hour = 12, minute = 0;
double usekwh = 0;

const int greenLED=6;               // Green tri-color LED
const int redLED=9;                 // Red tri-color LED
const int LDRpin=4;    		    // analog pin of onboard lightsensor 
int cval_use;

bool animate10s = true;

enum value_t {
  VALUE_TEMPERATURE,
  VALUE_POWER,
  VALUE_ENERGY,
  VALUE_VOLTAGE,
  VALUE_PERCENTAGE
};

typedef struct 
{
  value_t valueType;
  float   lastValue;
  long  lastValueUpdate;
  float minValue;
  long  minValueUpdate;
  float maxValue;
  long  maxValueUpdate;
  char textLabel[16];
} SensorValue_t;

const int BUFFER_SIZE=16;
const int DISP_REFRESH_INTERVAL = 200;

//--------------------------------------------------------------------------------------------
// Setup
//--------------------------------------------------------------------------------------------
void setup()
{
  delay(500); 				   //wait for power to settle before firing up the RF
  rf12_initialize(DISPLAY_NODE, RF_FREQ,GROUP);
  delay(100);				   //wait for RF to settle befor turning on display
  glcd.begin(0x19);
  glcd.backLight(200);

  pinMode(greenLED, OUTPUT); 
  pinMode(redLED, OUTPUT); 
}

void displayString(char* str, byte xpos, byte ypos, byte font, align_t align)
{  
  byte width;

  switch (font)
  {
    case 1:
      glcd.setFont(font_metric01);
      width = 4;
      break;

    case 2:
      glcd.setFont(font_metric02);
      width = 8;
      break;

    case 3:
      glcd.setFont(font_metric04);
      width = 16;
  }

  int pos = 0;

  switch(align)
  {
    case ALIGN_RIGHT:
      xpos = xpos - strlen(str) * width;
    break;

    case ALIGN_UNITS:
      while(str[pos] && (str[pos] <= 0x39 && str[pos] != 0x2A)) pos++;
      xpos = xpos - pos * width;
      break;

    case ALIGN_CENTRE:
      xpos = xpos - strlen(str) * (width/2);

    case ALIGN_LEFT:
    break;
  }   

  glcd.drawString(xpos,ypos,str); 
}

void displayNumber(float value, char* units, byte decimalPlaces, byte xpos, byte ypos, byte font, align_t align)
{
  char str[BUFFER_SIZE];

  dtostrf(value,0,decimalPlaces,str);
  strcat(str,units);

  displayString(str,xpos,ypos,font,align); 
}

void displayValue(SensorValue_t sensorValue, byte column, byte row)
{
  // No bounds checking
  byte xOffset = 64 * column;
  byte yOffset = 20 * row;

  displayString(sensorValue.textLabel,xOffset+63,yOffset,1,ALIGN_RIGHT);

  switch (sensorValue.valueType)
  {
    case VALUE_TEMPERATURE:
        displayNumber(sensorValue.lastValue,"*",1,xOffset + 55,yOffset + 6,2,ALIGN_UNITS); 
    break;

    case VALUE_POWER:
        if((sensorValue.lastValue > 1000) || (sensorValue.lastValue< -1000))
        {
          displayNumber(sensorValue.lastValue,"KW",1,xOffset + 47,yOffset + 6,2,ALIGN_UNITS);  
        }
        else
        {
          displayNumber(sensorValue.lastValue,"W",0,xOffset + 47,yOffset + 6,2,ALIGN_UNITS); 
        }
    break;

    case VALUE_ENERGY:
        displayNumber(sensorValue.lastValue,"KWH",0,xOffset + 39,yOffset + 6,2,ALIGN_UNITS); 
    break;

    case VALUE_VOLTAGE:
        displayNumber(sensorValue.lastValue,"V",0,xOffset + 55,yOffset + 6,2,ALIGN_UNITS); 
    break;

    case VALUE_PERCENTAGE:
    break;
  }

  switch (sensorValue.valueType)
  {
    case VALUE_TEMPERATURE:
        if (animate10s)
        {
          displayString("MAX",xOffset,yOffset+6,1,ALIGN_LEFT);
          displayNumber(sensorValue.maxValue,"*",1,xOffset,yOffset+13,1,ALIGN_LEFT);          
        }
        else
        {
          displayString("MIN",xOffset,yOffset+6,1,ALIGN_LEFT);
          displayNumber(sensorValue.minValue,"*",1,xOffset,yOffset+13,1,ALIGN_LEFT); 
        }
    break;

    case VALUE_POWER:
    break;

    case VALUE_ENERGY:
    break;

    case VALUE_VOLTAGE:
    break;

    case VALUE_PERCENTAGE:
    break;
  }

}



void loop()
{
  if (rf12_recvDone())
  {
    if (rf12_crc == 0 && (rf12_hdr & RF12_HDR_CTL) == 0)  // and no rf errors
    {
      int node_id = (rf12_hdr & 0x1F);

      if (node_id == POWER_NODE) {
          emontx = *(PayloadTX*) rf12_data;
          if (!powerValid)
          {
            powerMin = emontx.power1;
            powerMax = emontx.power1;
            powerValid = true;
          }

          if (emontx.power1 < powerMin)
            powerMin = emontx.power1;

          if (emontx.power1 > powerMax)
            powerMax = emontx.power1;

          powerLastUpdate = millis();
      }  
      
      if (node_id == BASE_NODE)
      {
        base = *(PayloadBase*) rf12_data;
        RTC.adjust(DateTime(2014, 1, 1, base.hour, base.minute, 0));
        baseLastUpdate = millis();
      } 
      
      if (node_id == EXTERNAL_NODE) 
      {
        externalth = *(PayloadTH*) rf12_data;
        externalTemp = (double)externalth.temp2 / 10.0;

        if (!externalValid)
        {
          externalTempMin = externalTemp;
          externalTempMax = externalTemp;
          externalValid = true;
        }

        if (externalTemp < externalTempMin)
          externalTempMin = externalTemp;

        if (externalTemp > externalTempMax)
          externalTempMax = externalTemp;

        externalLastUpdate = millis();
      }
      
      if (node_id == INTERNAL_NODE) 
      {
        internalth = *(PayloadTH*) rf12_data;
        internalTemp = (double)internalth.temp1 / 10.0;

        if (!internalValid)
        {
          internalTempMin = internalTemp;
          internalTempMax = internalTemp;
          internalValid = true;
        }

        if (internalTemp < internalTempMin)
          internalTempMin = internalTemp;

        if (internalTemp > externalTempMax)
          internalTempMax = internalTemp;

        internalLastUpdate = millis();
      }
    }
  }
  
  if ((millis()-fast_update)>DISP_REFRESH_INTERVAL)
  {
    fast_update = millis();
    
    DateTime now = RTC.now();
    int last_hour = hour;
    hour = now.hour();
    minute = now.minute();

    usekwh += (emontx.power1 * 0.2) / 3600000;
    
    if (last_hour == 23 && hour == 00)
    {
      usekwh = 0;                //reset Kwh/d counter at midnight
      internalValid = false;
      externalValid = false;
    }

    cval_use = cval_use + (emontx.power1 - cval_use)*0.50;        //smooth transitions
    
    glcd.clear();
    glcd.drawLine(64, 0, 64, 57, WHITE); // dividing line

    SensorValue_t sensorValue;

    sensorValue.lastValue = cval_use;
    sensorValue.valueType = VALUE_POWER;
    strcpy(sensorValue.textLabel,"POWER");


    displayValue(sensorValue,0,0);

    sensorValue.lastValue = externalTemp;
    sensorValue.minValue = externalTempMin;
    sensorValue.maxValue = externalTempMax;
    strcpy(sensorValue.textLabel,"EXTERNAL TEMP");
    sensorValue.valueType = VALUE_TEMPERATURE;

    displayValue(sensorValue,0,1);

    sensorValue.lastValue = internalTemp;
    sensorValue.minValue = internalTempMin;
    sensorValue.maxValue = internalTempMax;
    strcpy(sensorValue.textLabel,"INTERNAL TEMP");
    sensorValue.valueType = VALUE_TEMPERATURE;

    displayValue(sensorValue,0,2);

    sensorValue.lastValue = usekwh;
    sensorValue.valueType = VALUE_ENERGY;
    strcpy(sensorValue.textLabel,"ENERGY");


    displayValue(sensorValue,1,0);


    displayString("STATUS UPDATE",64,59,1, ALIGN_CENTRE);
    glcd.refresh();

    int LDR = analogRead(LDRpin);                     // Read the LDR Value so we can work out the light level in the room.
    int LDRbacklight = map(LDR, 0, 1023, 50, 250);    // Map the data from the LDR from 0-1023 (Max seen 1000) to var GLCDbrightness min/max
    LDRbacklight = constrain(LDRbacklight, 0, 255);   // Constrain the value to make sure its a PWM value 0-255)
    glcd.backLight(LDRbacklight);  
  } 
  
  if ((millis()-slow_update)>10000)
  {
    slow_update = millis();

    animate10s = !animate10s;
  }
}
