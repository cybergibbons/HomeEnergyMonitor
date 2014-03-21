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

const byte DISPLAY_ID       = 25; // Our ID (in case we send)
const byte INTERNAL_ID      = 19; // The internal EmonTH node
const byte BEDROOM_ID       = 20;
const byte EXTERNAL_ID      = 22; // The external EmonTH node
const byte POWER_ID         = 10; // The EmonTX node
const byte BASE_ID          = 15; // The Raspberry Pi base station (for receiving time updates)
const byte ENERGY_ID        = -1; // This is a fake node for integrating power to energy

const byte RF_FREQ          = RF12_433MHZ;   // frequency - match to same frequency as RFM12B module (change to 868Mhz or 915Mhz if appropriate)
const byte GROUP            = 210; 

const byte greenLED         = 6;         // Green tri-color LED
const byte redLED           = 9;           // Red tri-color LED
const byte LDRpin           = 4;           // analog pin of onboard lightsensor 

const byte BUFFER_SIZE      =  16;     // Used for string buffer - max half screen width
const byte BUFFER2_SIZE     = 4;      // Small buffer for the min/max
const byte DISP_REFRESH_INTERVAL = 200;
const float POWER_TO_ENERGY_FACTOR = ((float)DISP_REFRESH_INTERVAL / 1000.0) / 3600000;

/* ====================================
Strings stored in flash
==================================== */
const prog_char LABEL_MIN[] PROGMEM = "MIN";
const prog_char LABEL_MAX[] PROGMEM = "MAX";
const prog_char LABEL_STATUS[] PROGMEM = "STATUS UPDATE";
const prog_char LABEL_TIMEOUT[] PROGMEM = "TIMED OUT";

const prog_char LABEL_INTERNAL1[] PROGMEM = "LOUNGE TEMP";
const prog_char LABEL_INTERNAL2[] PROGMEM = "BEDROOM TEMP";
const prog_char LABEL_EXTERNAL[] PROGMEM = "OUTSIDE TEMP";
const prog_char LABEL_POWER[] PROGMEM = "POWER";
const prog_char LABEL_ENERGY[] PROGMEM = "ENERGY";
const prog_char LABEL_VOLTAGE[] PROGMEM = "VOLTAGE";
const prog_char LABEL_HUMIDITY[] PROGMEM = "HUMIDITY";
const prog_char LABEL_NOTHING[] = "";

// These need to be stored in the order they need to be displayed in the display
// 
const prog_char* VALUE_STRING_TABLE[] PROGMEM =
{
  LABEL_NOTHING,
  LABEL_POWER, 
  LABEL_INTERNAL1,
  LABEL_INTERNAL2,
  LABEL_EXTERNAL,
  LABEL_ENERGY,
  LABEL_VOLTAGE,
  LABEL_HUMIDITY
};

// Used to index the the string table above
enum{
  I_LABEL_NOTHING,
  I_LABEL_POWER,
  I_LABEL_INTERNAL1,
  I_LABEL_INTERNAL2,
  I_LABEL_EXTERNAL,
  I_LABEL_ENERGY,
  I_LABEL_VOLTAGE,
  I_LABEL_HUMIDITY
};

/* ====================================
End strings stored in flash
==================================== */

// Alightment of text
typedef enum {
  ALIGN_LEFT, // Left of beginneing of string
  ALIGN_RIGHT, // Right of end of string
  ALIGN_CENTRE, // Middle of string
  ALIGN_UNITS, // Left hand side of start of alpha chars after numeric
} align_t;

// Which font to use?
// Note this is very reliant on using the font_metric01/02/03.h fonts
typedef enum{
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

// The type of node
typedef enum {
  NODE_NONE,
  NODE_EMONTX, // Standard EMONTX
  NODE_EMONTH, // Standard EMONTH
  NODE_RFM12PI // Standard RFM12PI sending the time
};

// For I_NODE_VALUE on NODE_EMONTX
enum {
  VALUE_EMONTX_POWER1,
  VALUE_EMONTX_POWER2,
  VALUE_EMONTX_POWER3,
  VALUE_EMONTX_VRMS,
  VALUE_EMONTX_TEMP
};

// For I_NODE_VALUE on NODE_EMONTH
enum {
  VALUE_EMONTH_TEMP1,
  VALUE_EMONTH_TEMP2,
  VALUE_EMONTH_HUMIDITY,
  VALUE_EMONTH_BATTERY
};

// Which position on the screen should the data be
// X is 0-2, Y is 0-1.
enum {
  X0_Y0,
  X1_Y0,
  X2_Y0,
  X0_Y1,
  X1_Y1,
  X2_Y1
};

// Used to index the MAPPING_TABLE
enum {
  I_NODE_ID = 0,
  I_NODE_TYPE = 1,
  I_NODE_VALUE = 2,
  I_NODE_POSITION = 3,
  I_NODE_UNITS = 4,
  I_NODE_LABEL = 5,
  I_NODE_TIMEOUT = 6
};

// This holds all the details about the values we wish to display
FLASH_TABLE(byte, MAPPING_TABLE, 7,
// I_NODE_ID    I_NODE_TYPE   I_NODE_VALUE          I_NODE_POSITION   I_NODE_UNITS      I_NODE_LABEL      I_NODE_TIMEOUT
  {POWER_ID,    NODE_EMONTX,  VALUE_EMONTX_POWER1,  X0_Y0,            UNIT_POWER,       I_LABEL_POWER,    30},
  {INTERNAL_ID, NODE_EMONTH,  VALUE_EMONTH_TEMP1,   X1_Y0,            UNIT_TEMPERATURE, I_LABEL_INTERNAL1, 180},
  {EXTERNAL_ID, NODE_EMONTH,  VALUE_EMONTH_TEMP2,   X2_Y0,            UNIT_TEMPERATURE, I_LABEL_EXTERNAL, 180},
  {BEDROOM_ID,  NODE_EMONTH,  VALUE_EMONTH_TEMP1,   X1_Y1,            UNIT_TEMPERATURE, I_LABEL_INTERNAL2, 180},
  //{POWER_ID,    NODE_EMONTX,  VALUE_EMONTX_VRMS,    X1_Y1,            UNIT_VOLTAGE,     I_LABEL_VOLTAGE,  30},
  // ENERGY is summed from power.
  // Provide the valuye you wish to sum as a position int he display i.e. the power panel
  {ENERGY_ID,   NODE_NONE,    X0_Y0,                X0_Y1,            UNIT_ENERGY,      I_LABEL_ENERGY,   0},
  {INTERNAL_ID, NODE_EMONTH,  VALUE_EMONTH_HUMIDITY,X2_Y1,            UNIT_PERCENTAGE,  I_LABEL_HUMIDITY, 180}
  );

typedef struct {
  bool valid; // Have we received a reading yet? (also used to reset nightly values)
  float currentValue, // Current value of the reading
  minValue, // Minimum
  maxValue; // Maximum
  unsigned long lastUpdate; // Last millis() we received value
} value_t;

value_t values[6];

typedef struct { int power1, power2, power3, power4, Vrms, temp; }  PayloadTX;         // neat way of packaging data for RF comms
typedef struct { int temp1, temp2, humidity, voltage; }             PayloadTH;
typedef struct { char node, hour, minute; }                         PayloadBase;

int hour = 12, minute = 0;

double dailyEnergy = 0;
int currentPower;

byte animateCounter = 0;
bool animate2s = false;
bool animate4s = false;
bool animate10s = false;

unsigned long fastUpdate, slowUpdate;

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
      // 0x30 = 0
      // 0x29 = 9
      // 0x2E = .
      // Align on last character that is not one of these
      while(str[pos] && ((str[pos] >= 0x30 && str[pos] <= 0x39) || str[pos] == 0x2E)) pos++;
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
        displayNumber(mainValue/1000.0,"KW",1,xOffset + 47,yOffset + 6,FONT_MEDIUM,ALIGN_UNITS);  
      }
      else
      {
        // W with 0dp
        displayNumber(mainValue,"W",0,xOffset + 47,yOffset + 6,FONT_MEDIUM,ALIGN_UNITS); 
      }
      break;

    case UNIT_ENERGY:
      if (mainValue < 10) 
        displayNumber(mainValue,"KWH",2,xOffset + 38,yOffset + 6,FONT_MEDIUM,ALIGN_UNITS);
      else
        displayNumber(mainValue,"KWH",1,xOffset + 38,yOffset + 6,FONT_MEDIUM,ALIGN_UNITS);
      break;

    case UNIT_VOLTAGE:
      displayNumber(mainValue,"V",1,xOffset +54,yOffset +6,FONT_MEDIUM,ALIGN_UNITS);
      break;

    case UNIT_PERCENTAGE:
      displayNumber(mainValue,"%",1,xOffset+54,yOffset+6,FONT_MEDIUM,ALIGN_UNITS);
      break;

    default:
      // We haven't implemented this type yet
      displayString("ERR", xOffset+62,yOffset+6,FONT_MEDIUM,ALIGN_RIGHT);
  }
}

// Take a pointer to a string in flash and copy into a buffer
void fromFlash(PGM_P string, char * buffer, byte len)
{
    strncpy_P(buffer, string, len);
    buffer[len - 1] = '\0';
}

// When we receive a packet this copies the data into the values as required
void rf12_process()
{
  if (rf12_crc == 0 && (rf12_hdr & RF12_HDR_CTL) == 0)  // and no rf errors
  {
    int node_id = (rf12_hdr & 0x1F);

    // RF12 Node ID, Type of Node, Index of Value, Position in Display, Units
    for (int i = 0; i < MAPPING_TABLE.rows(); i++)
    {
      // Does this ID exist in our mapping?
      if (node_id == MAPPING_TABLE[i][I_NODE_ID])
      {
        float value;
        
        // Switch on the type of node
        switch (MAPPING_TABLE[i][I_NODE_TYPE])
        {
          case NODE_EMONTH:
          {
            PayloadTH payload = *(PayloadTH*) rf12_data;

            // Chose temp1 or temp2
            if (MAPPING_TABLE[i][I_NODE_VALUE] == 0)
              value = (float)payload.temp1 / 10.0;
            else
              value = (float)payload.temp2 / 10.0;

            switch(MAPPING_TABLE[i][I_NODE_VALUE])
            {
              case VALUE_EMONTH_TEMP1:
                value = (float)payload.temp1 / 10.0;
                break;

              case VALUE_EMONTH_TEMP2:
                value = (float)payload.temp2 / 10.0;
                break;

              case VALUE_EMONTH_HUMIDITY:
                value = (float)payload.humidity / 10.0;
                break;

              case VALUE_EMONTH_BATTERY:
                value = (float)payload.voltage / 10.0;
                break;
            }
          }
          break; // End NODE_EMONTH case

          case NODE_EMONTX:
          {
            PayloadTX payload = *(PayloadTX*) rf12_data;

            switch(MAPPING_TABLE[i][I_NODE_VALUE])
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
                value = (float)payload.temp / 10.0;
                break;
            } // End payload selection
            break; //End NODE_EMONTX
          }

            case NODE_RFM12PI:
            {
              PayloadBase payload = *(PayloadBase*) rf12_data;
              RTC.adjust(DateTime(2014, 1, 1, payload.hour, payload.minute, 0));
              break;
            } // End NODE_RFM12PI

        } // End node type switch

        // messy writing this more than a couple of times.
        byte position = MAPPING_TABLE[i][I_NODE_POSITION];

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

        values[position].lastUpdate = millis();
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

void setup()
{
  delay(500);            //wait for power to settle before firing up the RF
  rf12_initialize(DISPLAY_ID, RF_FREQ,GROUP);
  delay(100);          //wait for RF to settle befor turning on display

  glcd.begin(0x19);
  glcd.backLight(255);

  pinMode(greenLED, OUTPUT); 
  pinMode(redLED, OUTPUT); 

  // Make sure the values are not valid
  for (int i=0; i<6; i++)
    values[i].valid = 0;
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

    // Daily reset
    if (last_hour == 23 && hour == 00)
    {
      dailyEnergy = 0;                //reset Kwh/d counter at midnight

      // Reset MIN/MAX for all values
      for (int i= 0; i < MAPPING_TABLE.rows(); i++)
      {
        byte position = MAPPING_TABLE[i][I_NODE_POSITION];
        values[position].valid = false;
      }
    }

    // Perform the summing for energy values
    for (int i =0; i < MAPPING_TABLE.rows(); i++)
    {
      if (MAPPING_TABLE[i][I_NODE_ID] == ENERGY_ID)
      {
        // Just to make the lower lines look a touch more readable.
        byte position = MAPPING_TABLE[i][I_NODE_POSITION];
        byte positionToSum = MAPPING_TABLE[i][I_NODE_VALUE];

        // This takes the value from another display position and sums it for another position
        // POWER_TO_ENERGY_FACTOR is (DISPLAY_REFRESH_TIME / 1000) / (60 * 60 * 1000)
        if (values[positionToSum].valid)
        {
          values[position].currentValue += values[positionToSum].currentValue * POWER_TO_ENERGY_FACTOR;
          values[position].valid = true;
        }
      }
    }

    //currentPower = currentPower + (emontx.power1 - currentPower)*0.50;        //smooth transitions
    
    glcd.clear();
    glcd.drawLine(63, 0, 63, 57, WHITE); // dividing line

    char str[BUFFER_SIZE];
    char str2[BUFFER2_SIZE];

    // Render panels
    for (int i = 0; i < MAPPING_TABLE.rows(); i++)    
    {
      // This is just to make everything more readable below
      byte position = MAPPING_TABLE[i][I_NODE_POSITION];
      byte labelIndex = MAPPING_TABLE[i][I_NODE_LABEL];
      unit_t units = (unit_t)MAPPING_TABLE[i][I_NODE_UNITS]; 
      byte timeout = MAPPING_TABLE[i][I_NODE_TIMEOUT];

      if (values[position].valid)
      {
        float smallValue;

        if (animate4s)
        {
          smallValue = values[position].maxValue;
          fromFlash(LABEL_MAX,str2,BUFFER2_SIZE);
        }
        else
        {
          smallValue = values[position].minValue;
          fromFlash(LABEL_MIN,str2,BUFFER2_SIZE);
        }

        // Has the time since we last received an update exceeded the timeout?
        // If so, flash the timeout alternately with the normal label
        if (timeout && (((millis() - values[position].lastUpdate) / 1000) > timeout) && animate2s)
        {
            fromFlash(LABEL_TIMEOUT, str, BUFFER_SIZE);
        }
        else
        {
          fromFlash((const char*)pgm_read_word(&(VALUE_STRING_TABLE[labelIndex])),str,BUFFER_SIZE);
        }

        renderPanel(units, values[position].currentValue, str, smallValue, str2, position/3, position%3);
      }
    }

    // Not sure what to do here!
    fromFlash(LABEL_STATUS, str, BUFFER_SIZE);
    displayString(str,64,59,FONT_SMALL, ALIGN_CENTRE);

    glcd.refresh();

    // Dim backlight from LDR reading
    glcd_backlight();
  } 
  
  // Used for toggling slow animations on screen
  if ((millis()-slowUpdate)>1000)
  {
    slowUpdate = millis();
    animateCounter++;

    if (!(animateCounter%10))
      animate10s = !animate10s;

    if (!(animateCounter%4))
      animate4s = !animate4s;

    if (!(animateCounter%2))
      animate2s = !animate2s;
  }
}
