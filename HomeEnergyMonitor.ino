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
const int BUFFER2_SIZE  = 4;      // Small buffer for the min/max
const int DISP_REFRESH_INTERVAL = 200;
const int POWER_TO_ENERGY_FACTOR = DISP_REFRESH_INTERVAL / 1000 / 3600000;

const float ILLEGAL_TEMP  = -127;
const float ILLEGAL_POWER = -1; // This is not designed for import!





// Alightment of text
typedef enum 
{
  ALIGN_LEFT,
  ALIGN_RIGHT,
  ALIGN_CENTRE,
  ALIGN_UNITS,
} align_t;

typedef enum
{
  FONT_SMALL,
  FONT_MEDIUM,
  FONT_LARGE
} font_t;

// Type of value
typedef enum {
  UNIT_TEMPERATURE,
  UNIT_POWER,
  UNIT_ENERGY,
  UNIT_VOLTAGE,    // Not implemented
  UNIT_PERCENTAGE,  // Not implemented
  UNIT_TIME
} unit_t;

typedef enum {
  EMONTX_NODE,
  EMONTH_NODE,
  BASE_NODE
} node_t;


const byte POWER_DISPLAY = 0;
const byte INT_TEMP_DISPLAY = 1;
const byte EXT_TEMP_DISPLAY = 2;

const prog_char LABEL_MIN[] PROGMEM = "MIN";
const prog_char LABEL_MAX[] PROGMEM = "MAX";
const prog_char LABEL_STATUS[] PROGMEM = "STATUS UPDATE";

const prog_char LABEL_INTERNAL[] PROGMEM = "INT TEMP";
const prog_char LABEL_EXTERNAL[] PROGMEM = "EXT TEMP";
const prog_char LABEL_NOTIMPLEMENTED[] = "NOT IMP";
const prog_char LABEL_POWER[] PROGMEM = "POWER";
const prog_char LABEL_ENERGY[] PROGMEM = "ENERGY";
const prog_char LABEL_VOLTAGE[] PROGMEM = "VOLTAGE";

const prog_char* VALUE_STRING_TABLE[] PROGMEM =
{
  LABEL_POWER,
  LABEL_INTERNAL,
  LABEL_EXTERNAL,
  LABEL_VOLTAGE
};

// RF12 Node ID, Type of Node, Index of Value, Position in Display, Units
FLASH_TABLE(byte, MAPPING_TABLE, 5,
  {POWER_ID, EMONTX_NODE, 0, POWER_DISPLAY, UNIT_POWER},
  {INTERNAL_ID, EMONTH_NODE, 0, INT_TEMP_DISPLAY, UNIT_TEMPERATURE},
  {EXTERNAL_ID, EMONTH_NODE, 1, EXT_TEMP_DISPLAY, UNIT_TEMPERATURE},
  {POWER_ID, EMONTX_NODE, 4, 3, UNIT_VOLTAGE}
  );

typedef struct {
  bool valid;
  float currentValue,
  minValue,
  maxValue;
} value_t;

value_t values[6];

typedef struct { int power1, power2, power3, power4, Vrms, temp; } PayloadTX;         // neat way of packaging data for RF comms
typedef struct { int temp1, temp2, humidity, voltage; } PayloadTH;
typedef struct { char node, hour, minute; } PayloadBase;

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

  // Make sure the values are not valid
  for (int i=0; i<6; i++)
    values[i].valid = 0;
}

void displayString(const char* str, byte xpos, byte ypos, font_t font, align_t align)
{  
  byte width;

  // The only supported font currently is a monospaced font
  // Too much work to align with others
  switch (font)
  {
    case FONT_SMALL:
      glcd.setFont(font_metric01);
      // 3 wide + 1 for space
      width = 4;
      break;

    case FONT_MEDIUM:
      glcd.setFont(font_metric02);
      // 7 wide + 1 for space
      width = 8;
      break;

    case FONT_LARGE:
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
void displayNumber(float value, const char* units, byte decimalPlaces, byte xpos, byte ypos, font_t font, align_t align)
{
  char str[BUFFER_SIZE];

  dtostrf(value,0,decimalPlaces,str);
  strcat(str,units);

  displayString(str,xpos,ypos,font,align); 
}

// Render a pane with a large value, large label, small value and small label
void renderPanel(unit_t typeValue, float mainValue, const char* mainLabel, float smallValue, const char* smallLabel, byte column, byte row)
{
  // A two column, threw row layout
  // No checking that col/row is in bounds
  byte xOffset = 65 * column;
  byte yOffset = 20 * row;

  // The main label at the top in small font
  // 15 characters max
  displayString(mainLabel,xOffset+55,yOffset,FONT_SMALL,ALIGN_RIGHT);

  switch (typeValue)
  { 
    case UNIT_TEMPERATURE:
      // * is deg symbol in the monospaced font used
      displayNumber(mainValue,"*",1,xOffset+54,yOffset+6,FONT_MEDIUM,ALIGN_UNITS);
      
      // Small label - 4 characters max but 3 looks less cramped
      displayString(smallLabel,xOffset,yOffset+6,FONT_SMALL,ALIGN_LEFT);
      displayNumber(smallValue,"*",1,xOffset,yOffset+13,FONT_SMALL,ALIGN_LEFT);
      break;

    case UNIT_POWER:
      if((mainValue > 1000) || (mainValue< -1000))
      {
        // KW with 1dp
        displayNumber(mainValue,"KW",1,xOffset + 47,yOffset + 6,FONT_MEDIUM,ALIGN_UNITS);  
      }
      else
      {
        // W with 0dp
        displayNumber(mainValue,"W",0,xOffset + 47,yOffset + 6,FONT_MEDIUM,ALIGN_UNITS); 
      }
      break;

    case UNIT_ENERGY:
      displayNumber(mainValue,"KWH",1,xOffset + 38,yOffset + 6,FONT_MEDIUM,ALIGN_UNITS);
      break;

    case UNIT_VOLTAGE:
      displayNumber(mainValue,"V",1,xOffset + 47,yOffset +6,FONT_MEDIUM,ALIGN_UNITS);
      break;

    default:
      // We haven't implemented this type yet
      displayString("ERR 2", xOffset+62,yOffset+6,FONT_MEDIUM,ALIGN_RIGHT);
  }
}

void fromFlash(PGM_P string, char * buffer, byte len)
{
    strncpy_P(buffer, string, len);
    buffer[len - 1] = '\0';
}

void rf12_process()
{
  if (rf12_crc == 0 && (rf12_hdr & RF12_HDR_CTL) == 0)  // and no rf errors
  {
    int node_id = (rf12_hdr & 0x1F);

    // RF12 Node ID, Type of Node, Index of Value, Position in Display, Units
    for (int i = 0; i < MAPPING_TABLE.rows(); i++)
    {
      // Does this ID exist in our mapping?
      if (node_id == MAPPING_TABLE[i][0])
      {
        float value;
        
        // Switch on the type of node
        switch (MAPPING_TABLE[i][1])
        {
          case EMONTH_NODE:
          {
            PayloadTH payload = *(PayloadTH*) rf12_data;

            // Chose temp1 or temp2
            if (MAPPING_TABLE[i][2] == 0)
              value = (float)payload.temp1 / 10.0;
            else
              value = (float)payload.temp2 / 10.0;
          }
          break; // End EMONTH_NODE case

          case EMONTX_NODE:
          {
            PayloadTX payload = *(PayloadTX*) rf12_data;

            switch(MAPPING_TABLE[i][2])
            {
              case 0:
                value = payload.power1;
                break;

              case 1:
                value = payload.power2;
                break;

              case 2:
                value = payload.power3;
                break;

              case 3:
                value = payload.power4;
                break;

              case 4:
                value = (float)payload.Vrms / 100.0;
                break;

              case 5:
                value = payload.temp;
                break;
            } // End payload selection
            break; //End EMONTX_NODE
          }

            case BASE_NODE:
            {
              PayloadBase payload = *(PayloadBase*) rf12_data;
              RTC.adjust(DateTime(2014, 1, 1, payload.hour, payload.minute, 0));
              break;
            } // End BASE_NODE

        } // End node type switch

        // messy writing this more than a couple of times.
        byte position = MAPPING_TABLE[i][3];

        values[position].currentValue = value;

        // If this is the first valid reading, reset min/max
        // Also daily reset
        if (!values[position].valid)
        {
          values[position].minValue = value;
          values[position].maxValue = value;
        }

        if (value < values[position].minValue)
          values[position].minValue = value;

        if (value > values[position].maxValue)
          values[position].maxValue = value;

        values[position].valid = true;
      }
    }
  }
}

void glcd_backlight()
{
  int LDR = analogRead(LDRpin);                     // Read the LDR Value so we can work out the light level in the room.
  int LDRbacklight = map(LDR, 0, 1023, 50, 250);    // Map the data from the LDR from 0-1023 (Max seen 1000) to var GLCDbrightness min/max
  LDRbacklight = constrain(LDRbacklight, 0, 255);   // Constrain the value to make sure its a PWM value 0-255)
  glcd.backLight(LDRbacklight);  
}

void loop()
{
  if (rf12_recvDone())
  {
    rf12_process();
  }
  
  if ((millis()-fastUpdate)>DISP_REFRESH_INTERVAL)
  {
    fastUpdate = millis();
    
    DateTime now = RTC.now();
    int last_hour = hour;
    hour = now.hour();
    minute = now.minute();

    dailyEnergy += values[POWER_DISPLAY].currentValue * POWER_TO_ENERGY_FACTOR;
    
    if (last_hour == 23 && hour == 00)
    {
      dailyEnergy = 0;                //reset Kwh/d counter at midnight
      values[INT_TEMP_DISPLAY].valid = false;
      values[EXT_TEMP_DISPLAY].valid = false;
    }

    //currentPower = currentPower + (emontx.power1 - currentPower)*0.50;        //smooth transitions
    
    glcd.clear();
    glcd.drawLine(63, 0, 63, 57, WHITE); // dividing line

    char str[BUFFER_SIZE];
    char str2[BUFFER2_SIZE];

    for (int i = 0; i < MAPPING_TABLE.rows(); i++)    
    {
      byte position = MAPPING_TABLE[i][3];
      unit_t units = (unit_t)MAPPING_TABLE[i][4]; 

      if (values[position].valid)
      {
        float smallValue;

        if (animate10s)
        {
          smallValue = values[position].maxValue;
          fromFlash(LABEL_MAX,str2,BUFFER2_SIZE);
        }
        else
        {
          smallValue = values[position].minValue;
          fromFlash(LABEL_MIN,str2,BUFFER2_SIZE);
        }

        fromFlash((const char*)pgm_read_word(&(VALUE_STRING_TABLE[position])),str,BUFFER_SIZE);

        renderPanel(units, values[position].currentValue, str, smallValue, str2, position/3, position%3);
      }
    }

    fromFlash(LABEL_STATUS, str, BUFFER_SIZE);
    displayString(str,64,59,FONT_SMALL, ALIGN_CENTRE);

    glcd.refresh();

    // Dim backlight from LDR reading
    glcd_backlight();
  } 
  
  // Used for toggling slow animations on screen
  if ((millis()-slowUpdate)>10000)
  {
    slowUpdate = millis();
    animate10s = !animate10s;
  }
}
