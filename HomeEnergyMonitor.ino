#include <JeeLib.h>
#include <GLCD_ST7565.h>
#include <avr/pgmspace.h>
GLCD_ST7565 glcd;

#include <RTClib.h>                 // Real time clock (RTC) - used for software RTC to reset kWh counters at midnight
#include <Wire.h>                   // Part of Arduino libraries - needed for RTClib
RTC_Millis RTC;

#include <Flash.h>

#include "font_metric01.h"
#include "font_metric02.h"
#include "font_metric04.h"

const int DISPLAY_ID  = 25; // Our ID (in case we send)
const int INTERNAL_ID = 19; // The internal EmonTH node
const int EXTERNAL_ID = 22; // The external EmonTH node
const int POWER_ID    = 10; // The EmonTX node
const int BASE_ID     = 15; // The Raspberry Pi base station (for receiving time updates)
const int ENERGY_ID   = -1; // This is a fake node for integrating power to energy

const int RF_FREQ      = RF12_433MHZ;   // frequency - match to same frequency as RFM12B module (change to 868Mhz or 915Mhz if appropriate)
const int GROUP         = 210; 

const int greenLED      = 6;         // Green tri-color LED
const int redLED        = 9;           // Red tri-color LED
const int LDRpin        = 4;           // analog pin of onboard lightsensor 

const int BUFFER_SIZE   =  16;     // Used for string buffer - max half screen width
const int DISP_REFRESH_INTERVAL = 200;

const float ILLEGAL_TEMP  = -127;
const float ILLEGAL_POWER = -1; // This is not designed for import!

const prog_char LABEL_INTERNAL[] PROGMEM = "INT TEMP";
const prog_char LABEL_STATUS[] PROGMEM = "STATUS UPDATE";


// Alightment of text
typedef enum 
{
  ALIGN_LEFT,
  ALIGN_RIGHT,
  ALIGN_CENTRE,
  ALIGN_UNITS,
} align_t;

// Type of value
typedef enum {
  VALUE_TEMPERATURE,
  VALUE_POWER,
  VALUE_ENERGY,
  VALUE_VOLTAGE,    // Not implemented
  VALUE_PERCENTAGE  // Not implemented
} value_t;

enum {
  SENSOR_EMONTH,
  SENSOR_EMONTX,
  SENSOR_ENERGY
};

// Node ID, type of sensor, index of value, type of value, x, y
FLASH_TABLE(byte, LAYOUT_TABLE,6,
  {INTERNAL_ID, SENSOR_EMONTH, 0, VALUE_TEMPERATURE, 0, 0},
  {EXTERNAL_ID, SENSOR_EMONTH, 1, VALUE_TEMPERATURE, 1, 0},
  {POWER_ID, SENSOR_EMONTX, 0, VALUE_POWER, 0,1},
  {ENERGY_ID, SENSOR_ENERGY, 0, VALUE_ENERGY, 1, 1}
);

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

double dailyEnergy = 0;
int currentPower;

bool animate10s = true;

unsigned long fastUpdate, slowUpdate;

void setup()
{
  delay(500); 				   //wait for power to settle before firing up the RF
  rf12_initialize(DISPLAY_ID, RF_FREQ,GROUP);
  delay(100);				   //wait for RF to settle befor turning on display

  glcd.begin(0x19);
  glcd.backLight(255);

  pinMode(greenLED, OUTPUT); 
  pinMode(redLED, OUTPUT); 
}

void displayString(const char* str, byte xpos, byte ypos, byte font, align_t align)
{  
  byte width;

  // The only supported font currently is a monospaced font
  // Too much work to align with others
  switch (font)
  {
    case 1:
      glcd.setFont(font_metric01);
      // 3 wide + 1 for space
      width = 4;
      break;

    case 2:
      glcd.setFont(font_metric02);
      // 7 wide + 1 for space
      width = 8;
      break;

    case 3:
      glcd.setFont(font_metric04);
      // 15 wide + 1 for space
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

// Add units to the value and display string
void displayNumber(float value, const char* units, byte decimalPlaces, byte xpos, byte ypos, byte font, align_t align)
{
  char str[BUFFER_SIZE];

  dtostrf(value,0,decimalPlaces,str);
  strcat(str,units);

  displayString(str,xpos,ypos,font,align); 
}

// Render a pane with a large value, large label, small value and small label
void renderPanel(value_t typeValue, float mainValue, const char* mainLabel, float smallValue, const char* smallLabel, byte column, byte row)
{
  // A two column, threw row layout
  // No checking that col/row is in bounds
  byte xOffset = 65 * column;
  byte yOffset = 20 * row;

  // The main label at the top in small font
  // 15 characters max
  displayString(mainLabel,xOffset+55,yOffset,1,ALIGN_RIGHT);

  switch (typeValue)
  { 
    case VALUE_TEMPERATURE:
      // * is deg symbol in the monospaced font used
      displayNumber(mainValue,"*",1,xOffset+54,yOffset+6,2,ALIGN_UNITS);
      
      // Small label - 4 characters max but 3 looks less cramped
      displayString(smallLabel,xOffset,yOffset+6,1,ALIGN_LEFT);
      displayNumber(smallValue,"*",1,xOffset,yOffset+13,1,ALIGN_LEFT);
      break;

    case VALUE_POWER:
      if((mainValue > 1000) || (mainValue< -1000))
      {
        // KW with 0dp
        displayNumber(mainValue,"KW",1,xOffset + 47,yOffset + 6,2,ALIGN_UNITS);  
      }
      else
      {
        // W with 0dp
        displayNumber(mainValue,"W",0,xOffset + 47,yOffset + 6,2,ALIGN_UNITS); 
      }
      break;

    case VALUE_ENERGY:
      displayNumber(mainValue,"KWH",1,xOffset + 38,yOffset + 6,2,ALIGN_UNITS);
      break;

    default:
      // We haven't implemented this type yet
      displayString("ERR 2", xOffset+62,yOffset+6,2,ALIGN_RIGHT);
  }
}

void fromFlash(PGM_P string, char * buffer, byte len)
{
    strncpy_P(buffer, string, len);
    buffer[len - 1] = '\0';
}

void loop()
{
  if (rf12_recvDone())
  {
    if (rf12_crc == 0 && (rf12_hdr & RF12_HDR_CTL) == 0)  // and no rf errors
    {
      int node_id = (rf12_hdr & 0x1F);

      if (node_id == POWER_ID) {
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
      
      if (node_id == BASE_ID)
      {
        base = *(PayloadBase*) rf12_data;
        RTC.adjust(DateTime(2014, 1, 1, base.hour, base.minute, 0));
        baseLastUpdate = millis();
      } 
      
      if (node_id == EXTERNAL_ID) 
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
      
      if (node_id == INTERNAL_ID) 
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
  
  if ((millis()-fastUpdate)>DISP_REFRESH_INTERVAL)
  {
    fastUpdate = millis();
    
    DateTime now = RTC.now();
    int last_hour = hour;
    hour = now.hour();
    minute = now.minute();

    dailyEnergy += (emontx.power1 * 0.2) / 3600000;
    
    if (last_hour == 23 && hour == 00)
    {
      dailyEnergy = 0;                //reset Kwh/d counter at midnight
      internalValid = false;
      externalValid = false;
    }

    currentPower = currentPower + (emontx.power1 - currentPower)*0.50;        //smooth transitions
    
    glcd.clear();
    glcd.drawLine(63, 0, 63, 57, WHITE); // dividing line

    char str[BUFFER_SIZE];


    fromFlash(LABEL_INTERNAL, str, BUFFER_SIZE);
    renderPanel(VALUE_TEMPERATURE, internalTemp, str, animate10s?internalTempMax:internalTempMin,animate10s?"MAX":"MIN",0,0);
    renderPanel(VALUE_TEMPERATURE, externalTemp, "EXT TEMP", animate10s?externalTempMax:externalTempMin,animate10s?"MAX":"MIN",1,0);
    renderPanel(VALUE_POWER, currentPower, "POWER", animate10s?powerMax:powerMin,animate10s?"MAX":"MIN",0,1);
    renderPanel(VALUE_ENERGY, dailyEnergy, "DAILY ENERGY", 0.0, "TOT",1,1);


    

    fromFlash(LABEL_STATUS, str, BUFFER_SIZE);

    displayString(str,64,59,1, ALIGN_CENTRE);
    glcd.refresh();

    int LDR = analogRead(LDRpin);                     // Read the LDR Value so we can work out the light level in the room.
    int LDRbacklight = map(LDR, 0, 1023, 50, 250);    // Map the data from the LDR from 0-1023 (Max seen 1000) to var GLCDbrightness min/max
    LDRbacklight = constrain(LDRbacklight, 0, 255);   // Constrain the value to make sure its a PWM value 0-255)
    glcd.backLight(LDRbacklight);  
  } 
  
  if ((millis()-slowUpdate)>10000)
  {
    slowUpdate = millis();

    animate10s = !animate10s;
  }
}
