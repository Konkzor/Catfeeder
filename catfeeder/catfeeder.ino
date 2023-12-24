#include <EEPROM.h>
#include <Servo.h>
#include <Wire.h>
#include <arduino-timer.h>
#include <LiquidCrystal_I2C.h>
#include <SoftwareSerial.h>

#define SIZEOFARRAY(ARRAY) sizeof(ARRAY)/sizeof(ARRAY[0])

#if defined(ARDUINO) && ARDUINO >= 100
#define printByte(args)  write(args);
#else
#define printByte(args)  print(args,BYTE);
#endif

#undef DEBUG
#define SERVO

#ifdef DEBUG
 #define DEBUG_PRINT(x)  Serial.print(x)
 #define DEBUG_PRINTLN(x)  Serial.println(x)
#else
 #define DEBUG_PRINT(x)
 #define DEBUG_PRINTLN(x)
#endif

#define addr_flag 0

typedef struct{
  uint8_t secondes;
  uint8_t minutes;
  uint8_t heures; // format 24h
  uint8_t jourDeLaSemaine; // 1~7 = monday, ...
  uint8_t jour;
  uint8_t mois; 
  uint8_t annee; // format yy (ex 2012 -> 12)
}Date;
String days_short[8] = {"NC", "MO", "TU", "WE", "TH", "FR", "SA", "SU"};

typedef struct{
  Date date;
  short nbrev; // revolution
}Date_s;

// RTC
#define DS1307_ADDRESS 0x68

// WIFI
typedef struct{
  bool enable;
  unsigned char accesspoint[17];
  unsigned char key[18];
  char state;
}Network_s;
Network_s myNetwork;

#define HTTP " HTTP/1.1\r\n"
SoftwareSerial ESP8266(10, 11);

// Feeder
#define timeForOneTurn 2000 // ms
Date_s date2feed[6];

// Timer
auto timer = timer_create_default(); // create a timer with default settings
auto timerId_time = timer.in(1, NULL);
auto timerId_sec = timer.in(1, NULL);

// IOs
#ifdef SERVO
Servo myservo;
const int servoPin = 7;
#endif
const int button1 = 2;    // the number of the pushbutton pin
const int button2 = 3;    // the number of the pushbutton pin
const int button3 = 4;    // the number of the pushbutton pin
const int ledPin = 1;     // the number of the LED pin
bool flag_button1 = false;
bool flag_button2 = false;
bool flag_button3 = false;
const int ledIRPin = 5;
const int receivIRPin = 6;

// LCD
const int enLCD = 13;
LiquidCrystal_I2C lcd(0x27,20,4);
uint8_t up[8]  = {0x00,0x00,0x04,0x0e,0x1f,0x00,0x00,0x00};
uint8_t down[8]  = {0x00,0x00,0x00,0x1f,0x0e,0x04,0x00,0x00};
uint8_t wifi[8]  = {0x00,0x00,0x00,0x1e,0x03,0x19,0x05,0x15};

// MENU
typedef enum{
  HOME = 0,
  MENU,
  DATETIME,
  MEALS,
  NETWORK,
  SETTINGS
}mainState_type;
mainState_type mainState = HOME; 
String menuItems[] = {"DATE & TIME", "MEALS", "NETWORK", "SETTINGS", "BACK"};
char menuIndex = 0;
char menuDisplayedIndex = 0;
char menuTimeSettingsIndex = 1; // First digit of day value
char menuMealsCursor = 2;
char menuMealsIndex = 0;
char menuNetworkColCursor = 14;
char menuNetworkRowCursor = 1;
char menuSettingsColCursor = 9;
char menuSettingsRowCursor = 1;

// Variables
bool flag_updateDisplay = false;
bool flag_feed = true;
bool flag_time = false;
short timeleft = 0;
Date date_t;
Date_s next_date_s;
int inactivity_counter = 0; // min
// Week end mode
typedef struct{
  bool enable;
  Date date;
}WeekendMode_s;
WeekendMode_s weekendMode;
bool reservoir_empty = false;

/* ISR_time is called every 1 min */
bool ISR_time(void *){
  flag_time = true;
  return true;
}

bool ISR_sec(void *){
  // Blink colon
  blinkColon();

  // Blink LED if reservoir is empty
  if(reservoir_empty){
    digitalWrite(ledPin, !digitalRead(ledPin));
  }
  else{
    digitalWrite(ledPin, LOW);
  }

  return true;
}

void setup() {
  bool res = false;

  // IOs
  pinMode(button1, INPUT_PULLUP);
  pinMode(button2, INPUT_PULLUP);
  pinMode(button3, INPUT_PULLUP);
  pinMode(enLCD, OUTPUT);
  pinMode(ledPin, OUTPUT);
  pinMode(receivIRPin, INPUT);
  
  digitalWrite(ledPin, LOW);
  digitalWrite(enLCD, HIGH);
  tone(ledIRPin, 56000);
  
  // Serial
#ifdef DEBUG
  Serial.begin(9600);
  DEBUG_PRINTLN("******** Hello Catfeeder! ********");
#endif

  startESP();

  // LCD setup
  lcd.init();
  lcd.backlight();
  lcd.createChar(0, up);
  lcd.createChar(1, down);
  lcd.createChar(2, wifi);
  // RTC setup (I2C)
  Wire.begin();

  // Connect to Wifi
  getNetworkSettingsFromEEPROM();
  if(myNetwork.enable){
    myNetwork.state = connect2Wifi();
    delay(2000);
  }

  //RTC synchronization
  if(myNetwork.state){
    res = getNetworkTime(&date_t);
    if (res) writeToRTC(&date_t);
    else readFromRTC(&date_t);
  }
  else readFromRTC(&date_t);
  delay(1000);
  
  // Setup feeder schedule
  next_date_s.date = date_t;
  next_date_s.nbrev = 2;
  getMealsFromEEPROM();
  getWeekendModeSettingsFromEEPROM();
  // Init timeleft until next meal
  flag_feed = true;
  updateMeal(&next_date_s);
  
  printMainPage();
  
  // Timer setup
  timerId_time = timer.every(60000, ISR_time); // Every 1 min
  timerId_sec = timer.every(500, ISR_sec); // Every 500ms
}

void loop() {
  if(flag_time){
    // Update display if time is displayed
    flag_updateDisplay = true;
    // Inactivity counter increment
    inactivity_counter++;
    // Update Time
    readFromRTC(&date_t);
    // Update next meal every 1 min
    updateMeal(&next_date_s);
    // Update RTC time at 4:00 every day
    if(myNetwork.state && (date_t.heures == 4) && (date_t.minutes == 0)){ // 1h after an hypothetical time shift
      bool res = getNetworkTime(&date_t);
      if (res) writeToRTC(&date_t);
      else readFromRTC(&date_t);
      delay(1000); // To let the message on display
      lcd.clear();
    }
    // Update food reservoir status
    reservoir_empty = updateReservoirStatus();
    flag_time = false;
  }
  
  // Detect button push
  ISR_button1();
  ISR_button2();
  ISR_button3();

  // Wake-up the screen if one button is pushed
  if(flag_button1 || flag_button2 || flag_button3){
    inactivity_counter = 0;
    lcd.backlight();
  }
  
  switch(mainState){
    case HOME:{
      // Sleep after N min of no user interaction
      if(inactivity_counter >= 5){
        lcd.noBacklight();
      }
      
      // Manage time update on display
      if(flag_updateDisplay){
        // Update date, time and timeleft on display
        printDate(&date_t, 0, 0);
        printTime(&date_t, 0, 15);
        printTimeLeft();
        flag_updateDisplay = false;
      }

      // Manage food service
      if(flag_feed){
        timer.cancel(timerId_sec);
        //timer.cancel(timerId_time);
        
        // Feed the cat
        printTime2Eat();
        feedTheCat(next_date_s.nbrev);
    
        // If not connected, try to reconnect
        if(myNetwork.enable && !myNetwork.state) myNetwork.state = connect2Wifi();

        // Update next meal settings (time and nbrev)
        updateMeal(&next_date_s);
        printMainPage();
        flag_feed = false;
        //timerId_time = timer.every(60000, ISR_time);
        timerId_sec = timer.every(500, ISR_sec);
      }

      // Transition
      if(flag_button1 || flag_button3){ // Discard button 1 and 3
        flag_button1 = false;
        flag_button3 = false;
      }
      if(flag_button2){
        //timer.cancel(timerId_time);
        timer.cancel(timerId_sec);
        
        mainState = MENU;
        lcd.clear();
        printMenu();
        flag_button2 = false;
      }
      break;
    }
    
    case MENU:{
      // Go HOME after N min of inactivity
      if(inactivity_counter >= 2){
        inactivity_counter = 0;
        menuIndex = 0;
        menuDisplayedIndex = 0;
        mainState = HOME;
        printMainPage();
        timerId_sec = timer.every(500, ISR_sec);
      }
      // Up button
      if(flag_button1){
        if(menuIndex > 0)
          menuIndex--;
        else
          menuIndex = SIZEOFARRAY(menuItems) -1;
        // Manage auto scrolling
        if(menuIndex - menuDisplayedIndex > 2) menuDisplayedIndex = menuIndex - 2;
        else if(menuIndex - menuDisplayedIndex < 0) menuDisplayedIndex = menuIndex;
        printMenu();
        flag_button1 = false;
      }
      
      // Validation button
      else if(flag_button2){
        // Last item is a Return
        if(menuIndex == SIZEOFARRAY(menuItems)-1){
          menuIndex = 0;
          menuDisplayedIndex = 0;
          mainState = HOME;
          printMainPage();
          timerId_sec = timer.every(500, ISR_sec);
        }
        else{
          mainState = 2 + menuIndex;
          switch(mainState){
            case DATETIME:
              lcd.blink();
              lcd.clear();
              printDateTimeMenu(&date_t);
              break;
           case MEALS:
              lcd.blink();
              lcd.clear();
              printMealsMenu();
              break;
           case NETWORK:
              lcd.blink();
              lcd.clear();
              printNetworkMenu();
              break;
           case SETTINGS:
              lcd.blink();
              lcd.clear();
              printSettingsMenu();
              break;
           default:
              menuIndex = 0;
              menuDisplayedIndex = 0;
              mainState = HOME;
              printMainPage();
              timerId_sec = timer.every(500, ISR_sec);
              break;
          }
        }
        flag_button2 = false;
      }

      // Down button
      else if(flag_button3){
        if(menuIndex < (SIZEOFARRAY(menuItems)-1))
          menuIndex++;
        else
          menuIndex = 0;
        // Manage auto scrolling
        if(menuIndex - menuDisplayedIndex > 2) menuDisplayedIndex = menuIndex - 2;
        else if(menuIndex - menuDisplayedIndex < 0) menuDisplayedIndex = menuIndex;
        printMenu();
        flag_button3 = false;
      }
      break;
    }

    case DATETIME:{
      // Go HOME after N min of inactivity
      if(inactivity_counter >= 2){
        inactivity_counter = 0;
        menuIndex = 0;
        menuDisplayedIndex = 0;
        mainState = HOME;
        printMainPage();
        timerId_sec = timer.every(500, ISR_sec);
      }
      // Down button
      if(flag_button1){
        if(menuTimeSettingsIndex == 1){
          if(--date_t.jourDeLaSemaine == 255) date_t.jourDeLaSemaine = 7;
        }
        else if(menuTimeSettingsIndex == 4){
          if(--date_t.jour == 0) date_t.jour = 31;
        }
        else if(menuTimeSettingsIndex == 7){
          if(--date_t.mois == 0) date_t.mois = 12;
        }
        else if(menuTimeSettingsIndex == 10){
          if(--date_t.annee == 0) date_t.annee = 99;
        }
        if(menuTimeSettingsIndex == 16){
          if(--date_t.heures == 255) date_t.heures = 23;
        }
        else if(menuTimeSettingsIndex == 19){
          if(--date_t.minutes == 255) date_t.minutes = 59;
        }
        printDateTimeMenu(&date_t);
        flag_button1 = false;
      }
      
      // Next value button
      else if(flag_button2){
        flag_button2 = false;
        if(menuTimeSettingsIndex == 1) menuTimeSettingsIndex = 4;
        else if(menuTimeSettingsIndex == 4) menuTimeSettingsIndex = 7;
        else if(menuTimeSettingsIndex == 7) menuTimeSettingsIndex = 10;
        else if(menuTimeSettingsIndex == 10) menuTimeSettingsIndex = 16;
        else if(menuTimeSettingsIndex == 16) menuTimeSettingsIndex = 19;
        else if(menuTimeSettingsIndex == 19){
          // Write new time settings to RTC
          writeToRTC(&date_t);
          updateMeal(&next_date_s);
          // Reset context for next entry
          menuTimeSettingsIndex = 1;
          lcd.noBlink();
          lcd.clear();
          // Set new state
          mainState = MENU;
          printMenu();
          break;
        }
        printDateTimeMenu(&date_t);
      }

      // Up button
      else if(flag_button3){
        if(menuTimeSettingsIndex == 1){
          if(++date_t.jourDeLaSemaine == 8) date_t.jourDeLaSemaine = 1;
        }
        else if(menuTimeSettingsIndex == 4){
          if(++date_t.jour == 32) date_t.jour = 1;
        }
        else if(menuTimeSettingsIndex == 7){
          if(++date_t.mois == 13) date_t.mois = 1;
        }
        else if(menuTimeSettingsIndex == 10){
          if(++date_t.annee == 100) date_t.annee = 0;
        }
        if(menuTimeSettingsIndex == 16){
          if(++date_t.heures == 24) date_t.heures = 0;
        }
        else if(menuTimeSettingsIndex == 19){
          if(++date_t.minutes == 60) date_t.minutes = 0;
        }
        printDateTimeMenu(&date_t);
        flag_button3 = false;
      }
      break;
    }

    case MEALS:{
      // Go HOME after N min of inactivity
      if(inactivity_counter >= 2){
        inactivity_counter = 0;
        menuIndex = 0;
        menuDisplayedIndex = 0;
        mainState = HOME;
        printMainPage();
        timerId_sec = timer.every(500, ISR_sec);
      }
      // Down button
      if(flag_button1){
        flag_button1 = false;
        if(menuMealsCursor == 2){
          if(--date2feed[menuMealsIndex].date.heures == 255) date2feed[menuMealsIndex].date.heures = 23;
        }
        else if(menuMealsCursor == 5){
          if(--date2feed[menuMealsIndex].date.minutes == 255) date2feed[menuMealsIndex].date.minutes = 59;
        }
        else if(menuMealsCursor == 7){
          if(--date2feed[menuMealsIndex].nbrev < 0) date2feed[menuMealsIndex].nbrev = 9;
        }
        printMealsMenu();
      }
      
      // Next value button
      else if(flag_button2){
        flag_button2 = false;
        
        if(menuMealsCursor == 2) menuMealsCursor = 5;
        else if(menuMealsCursor == 5) menuMealsCursor = 7;
        else if(menuMealsCursor == 7){
          menuMealsCursor = 2;
          if(++menuMealsIndex == SIZEOFARRAY(date2feed)){
            // Save new settings
            setMealsToEEPROM();
            updateMeal(&next_date_s);
            // Reset context
            menuMealsIndex = 0;
            lcd.noBlink();
            lcd.clear();
            // Set new state
            mainState = MENU;
            printMenu();
            break;
          }
        }
        printMealsMenu();
      }

      // Up button
      else if(flag_button3){
        flag_button3 = false;
        if(menuMealsCursor == 2){
          if(++date2feed[menuMealsIndex].date.heures == 24) date2feed[menuMealsIndex].date.heures = 0;
        }
        else if(menuMealsCursor == 5){
          if(++date2feed[menuMealsIndex].date.minutes == 60) date2feed[menuMealsIndex].date.minutes = 0;
        }
        else if(menuMealsCursor == 7){
          if(++date2feed[menuMealsIndex].nbrev == 10) date2feed[menuMealsIndex].nbrev = 0;
        }
        printMealsMenu();
      }
      break;
    }

    case NETWORK:{
      // Go HOME after N min of inactivity
      if(inactivity_counter >= 2){
        inactivity_counter = 0;
        menuIndex = 0;
        menuDisplayedIndex = 0;
        mainState = HOME;
        printMainPage();
        timerId_sec = timer.every(500, ISR_sec);
      }
      if(flag_button1){
        flag_button1 = false;
        if(menuNetworkRowCursor == 1){
          myNetwork.enable = !myNetwork.enable;
        }
        else if(menuNetworkRowCursor == 2){
          if(--myNetwork.accesspoint[menuNetworkColCursor-3] < 0x20) myNetwork.accesspoint[menuNetworkColCursor-3] = 0x7F; // Discard useless characters
        }
        else if(menuNetworkRowCursor == 3){
          if(--myNetwork.key[menuNetworkColCursor-2] < 0x20) myNetwork.key[menuNetworkColCursor-2] = 0x7F; // Discard useless characters
        }
        printNetworkMenu();
      }
      
      else if(flag_button2){
        flag_button2 = false;
        
        if(menuNetworkRowCursor == 1){
          if(!myNetwork.enable){
            // Save new settings
            setNetworkSettingsToEEPROM();
            // Reset context
            menuNetworkRowCursor = 1;
            menuNetworkColCursor = 14;
            lcd.noBlink();
            lcd.clear();
            // Set new state
            mainState = MENU;
            printMenu();
            break;
          }
          menuNetworkRowCursor = 2;
          menuNetworkColCursor = 3;
        }
        else if(menuNetworkRowCursor == 2){
          if(++menuNetworkColCursor == 20){
            menuNetworkRowCursor = 3;
            menuNetworkColCursor = 2;
          }
        }
        else if(menuNetworkRowCursor == 3){
          if(++menuNetworkColCursor == 20){
            // Save new settings
            setNetworkSettingsToEEPROM();
            // Reset context
            menuNetworkRowCursor = 1;
            menuNetworkColCursor = 14;
            lcd.noBlink();
            lcd.clear();
            // Set new state
            mainState = MENU;
            printMenu();
            break;
          }
        }
        printNetworkMenu();
      }
      
      else if(flag_button3){
        flag_button3 = false;
        if(menuNetworkRowCursor == 1){
          myNetwork.enable = !myNetwork.enable;
        }
        else if(menuNetworkRowCursor == 2){
          if(++myNetwork.accesspoint[menuNetworkColCursor-3] >= 0x80) myNetwork.accesspoint[menuNetworkColCursor-3] = 0x20; // Discard useless characters
        }
        else if(menuNetworkRowCursor == 3){
          if(++myNetwork.key[menuNetworkColCursor-2] >= 0x80) myNetwork.key[menuNetworkColCursor-2] = 0x20; // Discard useless characters
        }
        printNetworkMenu();
      }
      break;
    }
    case SETTINGS:{
      // Go HOME after N min of inactivity
      if(inactivity_counter >= 2){
        inactivity_counter = 0;
        menuIndex = 0;
        menuDisplayedIndex = 0;
        mainState = HOME;
        printMainPage();
        timerId_sec = timer.every(500, ISR_sec);
      }
      if(flag_button1){
        flag_button1 = false;
        if(menuSettingsRowCursor == 1){ // Weekend mode
          if(menuSettingsColCursor == 9){ // State selection
            weekendMode.enable = !weekendMode.enable;
          }
          else if(menuSettingsColCursor == 14){ // Hour selection
            if(--weekendMode.date.heures == 255) weekendMode.date.heures = 23;
          }
          else if(menuSettingsColCursor == 17){ // Minute selection
            if(--weekendMode.date.minutes == 255) weekendMode.date.minutes = 59;
          }
        }
       printSettingsMenu();
      }
      
      else if(flag_button2){
        flag_button2 = false;
        
        if(menuSettingsColCursor == 9){
          if(!weekendMode.enable){
            // Save new settings
            setWeekendModeSettingsToEEPROM();
            updateMeal(&next_date_s);
            // Reset context
            menuSettingsRowCursor = 1;
            menuSettingsColCursor = 9;
            lcd.noBlink();
            lcd.clear();
            // Set new state
            mainState = MENU;
            printMenu();
            break;
          }
          menuSettingsColCursor = 14;
        }
        else if(menuSettingsColCursor == 14){
          menuSettingsColCursor = 17;
        }
        else if(menuSettingsColCursor == 17){
          // Save new settings
          setWeekendModeSettingsToEEPROM();
          // Reset context
          menuSettingsRowCursor = 1;
          menuSettingsColCursor = 9;
          lcd.noBlink();
          lcd.clear();
          // Set new state
          mainState = MENU;
          printMenu();
          break;
        }
        printSettingsMenu();
      }
      
      else if(flag_button3){
        flag_button3 = false;
        if(menuSettingsRowCursor == 1){ // Weekend mode
          if(menuSettingsColCursor == 9){ // State selection
            weekendMode.enable = !weekendMode.enable;
          }
          else if(menuSettingsColCursor == 14){ // Hour selection
            if(++weekendMode.date.heures == 24) weekendMode.date.heures = 0;
          }
          else if(menuSettingsColCursor == 17){ // Minute selection
            if(++weekendMode.date.minutes == 60) weekendMode.date.minutes = 0;
          }
        }
        printSettingsMenu();
      }
      break;
    }
  }

  timer.tick();
}

void updateMeal(Date_s* date_s){
  Date_s date2feed_masked[6]; // Array with meal time masked by week end minimal time (if needed)
  Date_s first_meal;
  bool masked = false;
  int masked_meal_index = -1;
  bool no_meal = true;

  // If weekend mode is enabled, determine if meals must be masked or not
  if(weekendMode.enable){
    if(((date_t.jourDeLaSemaine == 5) && ((60*date_t.heures+date_t.minutes) > (60*weekendMode.date.heures+weekendMode.date.minutes))) || // Friday after weekend minimal time
      (date_t.jourDeLaSemaine == 6) || // Saturday
      ((date_t.jourDeLaSemaine == 7) && ((60*date_t.heures+date_t.minutes) < (60*weekendMode.date.heures+weekendMode.date.minutes)))){ // Sunday before weekend minimal time
        masked = true;
    }
  }

  // Fill the meal array with masked meals (or not)
  for(int i = 0 ; i < SIZEOFARRAY(date2feed_masked) ; i++){
    date2feed_masked[i] = date2feed[i];
    // If meals must be masked, set all meals before minimal time to minimal time
    if(masked && (60*date2feed_masked[i].date.heures+date2feed_masked[i].date.minutes) < (60*weekendMode.date.heures+weekendMode.date.minutes)){
      // If no meal already masked
      if(masked_meal_index == -1){
        date2feed_masked[i].date.heures = weekendMode.date.heures;
        date2feed_masked[i].date.minutes = weekendMode.date.minutes;
        masked_meal_index = i;
      }
      // Else, remove the meal and add its nbrev to the already masked meal
      else{
        date2feed_masked[masked_meal_index].nbrev += date2feed_masked[i].nbrev;
        date2feed_masked[i].nbrev = 0;
        if(date2feed_masked[masked_meal_index].nbrev > 9)
          date2feed_masked[masked_meal_index].nbrev = 9;
      }
    }
  }

  // Initialise first_meal value and detect if no meal is activaed
  for(int i = 0 ; i < SIZEOFARRAY(date2feed_masked) ; i++){
    if(date2feed_masked[i].nbrev != 0){
      first_meal = date2feed_masked[i];
      no_meal = false;
      break;
    }
  }

  // TODO Manage situation where no meal is activated
  if(no_meal){
    return;
  }
  
  // Get the next meal
  timeleft = 24*60; // By default (the max value)
  short timeleft_temp;
  for(int i = 0 ; i < SIZEOFARRAY(date2feed) ; i++){ 
    if((date2feed_masked[i].nbrev == 0) || // Meal is not activated, discard it
      (60*date_t.heures+date_t.minutes > 60*date2feed_masked[i].date.heures + date2feed_masked[i].date.minutes)) // Meal is past
      continue;
    else{ // Meal is later in the day
      timeleft_temp = (date2feed_masked[i].date.heures - date_t.heures)*60 + (date2feed_masked[i].date.minutes - date_t.minutes);
      // If flag_feed is already set and timeleft is zero, discard the corresponding meal
      if(flag_feed && (timeleft_temp == 0)) continue;
      else if(timeleft_temp < timeleft){
        timeleft = timeleft_temp;
        *date_s = date2feed_masked[i];
      }
    }
  }
  
  // If all meal are served, set next meal as first_meal
  if (timeleft == 24*60){
    // Found the first meal of the day
    for(int i = 0 ; i < SIZEOFARRAY(date2feed_masked) ; i++){
      if(date2feed_masked[i].nbrev == 0) // Meal is not activated, discard it
        continue;
      else if(60*date2feed_masked[i].date.heures + date2feed_masked[i].date.minutes < 60*first_meal.date.heures + first_meal.date.minutes){
        first_meal = date2feed_masked[i];
      }
    }
    *date_s = first_meal;
    timeleft = (24 - date_t.heures + date_s->date.heures)*60 + (0 - date_t.minutes + date_s->date.minutes);
    if (timeleft > 12*60) timeleft = 12*60;
  }

  // Ask for a feed if timeleft is 0
  if(!flag_feed && (timeleft <= 0)){
    // Ask for feed
    flag_feed = true;
  }
}

bool updateReservoirStatus(void){
  return (digitalRead(receivIRPin) == 0);
}

void ISR_button1(void){
  if(digitalRead(button1) == 0){
    delay(100);
    if(digitalRead(button1) == 0) flag_button1 = true;
  }
}

void ISR_button2(void){
  if(digitalRead(button2) == 0){
    delay(100);
    if(digitalRead(button2) == 0) flag_button2 = true;
  }
}

void ISR_button3(void){
  if(digitalRead(button3) == 0){
    delay(100);
    if(digitalRead(button3) == 0) flag_button3 = true;
  }
}

void feedTheCat(const short revolutions){
#ifdef SERVO
   // Attach the servo to pin
  myservo.attach(servoPin);
  // First turn backward to avoid jamming
  myservo.write(85);
  delay(500);
  // Then turn forward
  for(short i = 0 ; i < revolutions ; i++){
    myservo.write(95);
    delay(timeForOneTurn);
    myservo.write(85);
    delay(250);
  }
  myservo.write(90);
  // Detach the servo to rest
  myservo.detach();
#endif
}

/******************************************************************************
 *                              WIFI FUNCTIONS                                *
 ******************************************************************************/
void startESP(){
  // In case of a Arduino reboot, ESP is already set to 9600 baups
  ESP8266.begin(9600);
  envoieAuESP8266("AT+RST");
  recoitDuESP8266(2000L, -1);
  
  // Reboot in 115200 baups if ESP has been rebooted
  ESP8266.begin(115200);
  envoieAuESP8266("AT+RST");
  recoitDuESP8266(2000L, -1);

  envoieAuESP8266("AT+UART_CUR=9600,8,1,0,0");
  ESP8266.begin(9600);
  recoitDuESP8266(2000L, -1);
  
  envoieAuESP8266("AT");
  recoitDuESP8266(2000L, -1);
}

bool connect2Wifi(){
  lcd.clear();
  lcd.setCursor(8,1);
  lcd.print("WIFI");
  lcd.setCursor(4,2);
  lcd.print("CONNECTION...");

  bool state = false;
  envoieAuESP8266("AT+CWMODE=1"); // WIFI MODE STATION
  recoitDuESP8266(5000L, -1);
  String ap = myNetwork.accesspoint;
  String key = myNetwork.key;
  envoieAuESP8266("AT+CWJAP=\"" + ap + "\",\"" + key + "\""); // JOIN ACCESS POINT
  recoitDuESP8266(7000L, -1);
  state = checkWiFi();
  envoieAuESP8266("AT+CIPMUX=0");
  recoitDuESP8266(2000L, -1);

  lcd.clear();
  lcd.setCursor(2,1);
  lcd.print("WIFI CONNECTION");
  if(state){
      lcd.setCursor(5,2);
      lcd.print("SUCCEEDED");
  }
  else {
      lcd.setCursor(7,2);
      lcd.print("FAILED");
  }

  return state;
}

bool checkWiFi(void){
  envoieAuESP8266("AT+CIPSTATUS");
  bool res = recoitDuESP8266(3000, '2'); // 2 means not connected bu got IP
  return res;
}

void envoieAuESP8266(String commande){
  ESP8266.println(commande);
}

bool recoitDuESP8266(const long int timeout, char searched_char){
  char c;
  bool char_found = false;
  
  if(searched_char == -1) searched_char = true;
  
  long int t_start = millis();
  while ((t_start + timeout) > millis())
  {
    if(ESP8266.available()>0){
      c = ESP8266.read();
      if(c == searched_char){
        char_found = true;
      }
      DEBUG_PRINT(c);
    }
  }
  return char_found;
}

bool recoitDateEtHeureDuESP8266(const long int timeout, String &datetime, String &dayOfWeek){
  char c;

  char key_datetime[] = "datetime: ";
  char key_datetime_i = 0;
  bool datetime_locked = false;
  bool datetime_found = false;

  char key_dayOfWeek[] = "day_of_week: ";
  char key_dayOfWeek_i = 0;
  bool dayOfWeek_locked = false;
  bool dayOfWeek_found = false;
  
  long int t_start = millis();
  while (((t_start + timeout) > millis()) && (!datetime_found || !dayOfWeek_found))
  {
    if(ESP8266.available()>0){
      c = ESP8266.read();
      DEBUG_PRINT(c);
      
      // Search for datetime
      if(!datetime_found){ //Search only first occurrence
        if(c == key_datetime[key_datetime_i]){ // Search for the keyword
          if(key_datetime_i == 9){
            // end of keyword found
            datetime_locked = true;
          }
          else key_datetime_i++;
        }
        else if (!datetime_locked){ // Reset pointer on keyword if keyword does not match
          key_datetime_i = 0;
        }
        else{ // Get all characters of interest until the last one
          if(c == '.'){
            datetime_locked = false;
            datetime_found = true;
          }
          else{
            datetime += c;
          }
        }
      }

      // Search for day of week
      if(!dayOfWeek_found){ //Search only first occurrence
        if(c == key_dayOfWeek[key_dayOfWeek_i]){
          if(key_dayOfWeek_i == 12){
            // end of keyword found
            dayOfWeek_locked = true;
          }
          else key_dayOfWeek_i++;
        }
        else if (!dayOfWeek_locked){ // Reset pointer on keyword if keyword does not match
          key_dayOfWeek_i = 0;
        }
        else{ // Get all characters of interest until the last one
          if(c == '\n'){
            dayOfWeek_locked = false;
            dayOfWeek_found = true;
          }
          else{
            dayOfWeek += c;
          }
        }
      }
    }
  }
  
  return (dayOfWeek_found && datetime_found);
}


bool getNetworkTime(Date* date){
  lcd.clear();
  lcd.setCursor(1,1);
  lcd.print("TIME & DATE UPDATE");

  // Start session
  String cmd = "AT+CIPSTART=\"TCP\",\"";
  cmd += "worldtimeapi.org";
  cmd += "\",";
  cmd += "80";
  ESP8266.println(cmd);
  recoitDuESP8266(2000L, -1);

  // Send request
  String request = "GET ";
  request += "/api/ip.txt"; // Get settings
  request += HTTP;
  String cmd_send = "AT+CIPSEND=";
  cmd_send += String(request.length() + 2);
  ESP8266.println(cmd_send);
  delay(2000);
  recoitDuESP8266(2000L, -1);

  String datetime_str = "";
  String dayOfWeek_str = "";
  ESP8266.println(request);
  bool res = recoitDateEtHeureDuESP8266(15000L, datetime_str, dayOfWeek_str);
  
  if(res){
    char date_array[20];
    short annee, mois, jour, heures, minutes, secondes = 0;
    datetime_str.toCharArray(date_array, 20);
    int nb_assignments = sscanf(date_array, "%d-%d-%dT%d:%d:%d", &annee, &mois, &jour, &heures, &minutes, &secondes);
    
    if(nb_assignments == 6){
      date->annee = annee%100;
      date->mois = mois;
      date->jour = jour;
      date->heures = heures;
      date->minutes = minutes;
      date->secondes = secondes;
      if(dayOfWeek_str[0] == '0'){ //dayOfWeek_str[0] = 0 on Sunday, 1 on Monday, etc.
        date->jourDeLaSemaine = 7;
      }
      else date->jourDeLaSemaine = dayOfWeek_str[0] - '0';
    }
    else{
      res = false;
    }
  }

  if(res){
    lcd.setCursor(5,2);
    lcd.print("SUCCEEDED");
  }
  else{
    lcd.setCursor(7,2);
    lcd.print("FAILED");
  }

  return res;
}
/******************************************************************************
 *                              LCD FUNCTIONS                                 *
 ******************************************************************************/
void printDate(Date *date, char row, char col_0) {
  lcd.setCursor(col_0, row);
  lcd.print(days_short[date->jourDeLaSemaine]); // 2 lettres
  lcd.setCursor(col_0+2, row);
  lcd.print("-");
  lcd.setCursor(col_0+3, row); // Place le curseur à (0,0)
  lcd.print(date->jour / 10, DEC);// Affichage du jour sur deux caractéres
  lcd.setCursor(col_0+4, row);
  lcd.print(date->jour % 10, DEC);
  lcd.setCursor(col_0+5, row);
  lcd.print("/");
  lcd.setCursor(col_0+6, row);
  lcd.print(date->mois / 10, DEC);// Affichage du mois sur deux caractéres
  lcd.setCursor(col_0+7, row);
  lcd.print(date->mois % 10, DEC);
  lcd.setCursor(col_0+8, row);
  lcd.print("/");
  lcd.setCursor(col_0+9, row);
  lcd.print(date->annee / 10, DEC);// Affichage de l'année sur deux caractéres
  lcd.setCursor(col_0+10, row);
  lcd.print(date->annee % 10, DEC);
}

void printTime(Date *date, char row, char col_0) {
  lcd.setCursor(col_0, row);
  lcd.print(date->heures / 10, DEC); // Affichage de l'heure sur deux caractéres
  lcd.setCursor(col_0+1, row);
  lcd.print(date->heures % 10, DEC);
  lcd.setCursor(col_0+2, row);
  lcd.print(":");
  lcd.setCursor(col_0+3, row);
  lcd.print(date->minutes / 10, DEC); // Affichage des minutes sur deux caractéres
  lcd.setCursor(col_0+4, row);
  lcd.print(date->minutes % 10, DEC);
}

void printTime2Eat(void){
  lcd.clear();
  lcd.setCursor(1,1);
  lcd.print("IT'S TIME TO EAT !");
}

void blinkColon(void){
  static char state = 1;
  lcd.setCursor(17, 0);
  
  if(state == 1) lcd.print(":");
  else lcd.print(" ");

  state = -state;
}

void printWifiState(short state){
  lcd.setCursor(16,1);
  if(state == 1){
    lcd.printByte(2);
  }
  else{
    lcd.print("x");
  }
}

void printTimeLeft(void){
  lcd.setCursor(3,2);
  lcd.print(next_date_s.nbrev, DEC); // Affichage nombre de tours < 9
  lcd.setCursor(4,2);
  lcd.print(" P. in ");
  lcd.setCursor(11,2);
  lcd.print((timeleft/60) / 10, DEC); // Affichage de l'heure sur deux caractéres
  lcd.setCursor(12,2);
  lcd.print((timeleft/60) % 10, DEC);
  lcd.setCursor(13,2);
  lcd.print(":");
  lcd.setCursor(14,2);
  lcd.print((timeleft % 60) / 10, DEC); // Affichage des minutes sur deux caractéres
  lcd.setCursor(15,2);
  lcd.print((timeleft % 60) % 10, DEC);
}

void printMainPage(){
  lcd.clear();
  printDate(&date_t, 0, 0);
  printTime(&date_t, 0, 15);
  printTimeLeft();
  if(myNetwork.enable)
    printWifiState(myNetwork.state);
}

void printMenu(){
  lcd.clear();
  lcd.home();
  lcd.print("------- MENU -------");
  char row_index = 1;
  for(int i = menuDisplayedIndex ; i < SIZEOFARRAY(menuItems) ; i++){
     if(menuIndex == i){
        lcd.setCursor((20 - (menuItems[i].length()-1))/2 - 2, row_index);
        lcd.print("> ");
        lcd.print(menuItems[i]);
        lcd.print(" <");
     }
     else{
        lcd.setCursor((20 - (menuItems[i].length()-1))/2, row_index);
        lcd.print(menuItems[i]);
     }
     if(++row_index > 3) break;
  }
  // Print up and down char
  if(menuDisplayedIndex > 0){
    lcd.setCursor(0, 1);
    lcd.printByte(0);
  }
  if(SIZEOFARRAY(menuItems) - menuDisplayedIndex > 3){
    lcd.setCursor(0, 3);
    lcd.printByte(1);
  }
}

void printDateTimeMenu(Date *date){
  lcd.home();
  lcd.print("---- DATE & TIME ---");
  printDate(date, 2, 0);
  printTime(date, 2, 15);
  lcd.setCursor(menuTimeSettingsIndex, 2);
}

void printMealsMenu(){
  lcd.home();
  lcd.print("------- MEALS ------");
  char row_index = 1;
  char col_index = 0;

  for(int i = 0 ; i < SIZEOFARRAY(date2feed) ; i++){
    lcd.setCursor(col_index+1, row_index);
    lcd.print(date2feed[i].date.heures / 10, DEC); // Affichage de l'heure sur deux caractéres
    lcd.setCursor(col_index+2, row_index);
    lcd.print(date2feed[i].date.heures % 10, DEC);
    lcd.setCursor(col_index+3, row_index);
    lcd.print(":");
    lcd.setCursor(col_index+4, row_index);
    lcd.print(date2feed[i].date.minutes / 10, DEC); // Affichage des minutes sur deux caractéres
    lcd.setCursor(col_index+5, row_index);
    lcd.print(date2feed[i].date.minutes % 10, DEC);
    lcd.setCursor(col_index+6, row_index);
    lcd.print("[");
    lcd.setCursor(col_index+7, row_index);
    lcd.print(date2feed[i].nbrev, DEC); // Affichage du nb de revolutions
    lcd.setCursor(col_index+8, row_index);
    lcd.print("]");

    if(i%2 == 0){
      col_index += 10;
    }
    else{
      col_index = 0;
      row_index++;
    }
  }
  
  lcd.setCursor((menuMealsIndex%2)*10+menuMealsCursor, 1+(menuMealsIndex/2));
}

void printNetworkMenu(){
  lcd.home();
  lcd.print("------ NETWORK -----");
  lcd.setCursor(0,1);
  lcd.print("WIFI STATE:");
  lcd.setCursor(14,1);
  if(myNetwork.enable){
    char curs = 3;
    lcd.print("ON ");
    lcd.setCursor(0,2);
    lcd.print("AP:");
    for(int i = 0 ; i < SIZEOFARRAY(myNetwork.accesspoint); i++){
      lcd.setCursor(curs++, 2);
      lcd.printByte(myNetwork.accesspoint[i]);
    }
    lcd.setCursor(0,3);
    lcd.print("K:");
    curs = 2;
    for(int i = 0 ; i < SIZEOFARRAY(myNetwork.key); i++){
      lcd.setCursor(curs++, 3);
      lcd.printByte(myNetwork.key[i]);
    }
  }
  else{
    lcd.print("OFF");
    lcd.setCursor(0,2);
    lcd.print("                    ");
    lcd.setCursor(0,3);
    lcd.print("                    ");
  }
  
  lcd.setCursor(menuNetworkColCursor, menuNetworkRowCursor);
}

void printSettingsMenu(){
  lcd.home();
  lcd.print("----- SETTINGS -----");
  lcd.setCursor(0,1);
  lcd.print("WE MODE:");
  lcd.setCursor(9,1);
  if(weekendMode.enable){
    lcd.print("ON ");
    // Print minimal time
    char curs = 13;
    lcd.setCursor(curs,1);
    lcd.print(weekendMode.date.heures / 10, DEC); // Affichage de l'heure sur deux caractéres
    lcd.setCursor(curs+1, 1);
    lcd.print(weekendMode.date.heures % 10, DEC);
    lcd.setCursor(curs+2, 1);
    lcd.print(":");
    lcd.setCursor(curs+3, 1);
    lcd.print(weekendMode.date.minutes / 10, DEC); // Affichage des minutes sur deux caractéres
    lcd.setCursor(curs+4, 1);
    lcd.print(weekendMode.date.minutes % 10, DEC);
  }
  else{
    lcd.print("OFF      ");
  }
  
  lcd.setCursor(menuSettingsColCursor, menuSettingsRowCursor);
}
/******************************************************************************
 *                              RTC FUNCTIONS                                 *
 ******************************************************************************/
 
// Fonction configurant le DS1307 avec la date/heure fourni
void writeToRTC(Date *date) {
  Wire.beginTransmission(DS1307_ADDRESS); // Début de transaction I2C
  Wire.write(0); // Arrête l'oscillateur du DS1307
  Wire.write(dec2bcd(date->secondes)); // Envoi des données
  Wire.write(dec2bcd(date->minutes));
  Wire.write(dec2bcd(date->heures));
  Wire.write(dec2bcd(date->jourDeLaSemaine));
  Wire.write(dec2bcd(date->jour));
  Wire.write(dec2bcd(date->mois));
  Wire.write(dec2bcd(date->annee));
  Wire.write(0); // Redémarre l'oscillateur du DS1307
  Wire.endTransmission(); // Fin de transaction I2C
}
 
// Fonction récupérant l'heure et la date courante à partir du DS1307
void readFromRTC(Date *date) {
  Wire.beginTransmission(DS1307_ADDRESS); // Début de transaction I2C
  Wire.write(0); // Demande les info à partir de l'adresse 0 (soit toutes les info)
  Wire.endTransmission(); // Fin de transaction I2C
 
  Wire.requestFrom(DS1307_ADDRESS, 7); // Récupère les info (7 octets = 7 valeurs correspondant à l'heure et à la date courante)
 
  date->secondes = bcd2dec(Wire.read()); // stockage et conversion des données reçu
  date->minutes = bcd2dec(Wire.read());
  date->heures = bcd2dec(Wire.read() & 0b111111);
  date->jourDeLaSemaine = bcd2dec(Wire.read());
  date->jour = bcd2dec(Wire.read());
  date->mois = bcd2dec(Wire.read());
  date->annee = bcd2dec(Wire.read());
}

byte bcd2dec(byte bcd) {
  return ((bcd / 16 * 10) + (bcd % 16)); 
}
 
byte dec2bcd(byte dec) {
  return ((dec / 10 * 16) + (dec % 10));
}

/******************************************************************************
 *                              EEPROM FUNCTIONS                              *
 ******************************************************************************/
void getMealsFromEEPROM(void){
  int value = 0;
  int address = 0;
  
  // Read first address to determine if EEPROM has already be written to
  value = EEPROM.read(address);
  if(value != 255){ // EEPROM has already be written
    for(int i = 0 ; i < SIZEOFARRAY(date2feed) ; i++){
      date2feed[i].date.heures = EEPROM.read(address++);
      date2feed[i].date.minutes = EEPROM.read(address++);
      date2feed[i].nbrev = EEPROM.read(address++);
    }
  }
  else{ // EEPROM never modified
    // Default setup (4 meals)
    date2feed[0].date.heures = 7;
    date2feed[0].date.minutes = 0;
    date2feed[0].nbrev = 2;
    date2feed[1].date.heures = 12;
    date2feed[1].date.minutes = 0;
    date2feed[1].nbrev = 1;
    date2feed[2].date.heures = 17;
    date2feed[2].date.minutes = 0;
    date2feed[2].nbrev = 1;
    date2feed[3].date.heures = 21;
    date2feed[3].date.minutes = 0;
    date2feed[3].nbrev = 2;

    setMealsToEEPROM();
  }
}

void setMealsToEEPROM(void){
  int address = 0;
  for(int i = 0 ; i < SIZEOFARRAY(date2feed) ; i++){
    EEPROM.update(address++, date2feed[i].date.heures);
    EEPROM.update(address++, date2feed[i].date.minutes);
    EEPROM.update(address++, date2feed[i].nbrev);
  }
}

void getNetworkSettingsFromEEPROM(void){
  int address = 18;
  // Enable
  char enable = EEPROM.read(address);
  if((enable == 0) || (enable == -1)){
    myNetwork.enable = false;
  }
  else{
    myNetwork.enable = true;
  }
  // Accespoint and key (from 19 to 54)
  address = 19;
  for(int i = 0 ; i < SIZEOFARRAY(myNetwork.accesspoint); i++){
    myNetwork.accesspoint[i] = EEPROM.read(address++);
    if(myNetwork.accesspoint[i] >= 0x80) myNetwork.accesspoint[i] = 0x20; // Discard useless characters
  }
  for(int i = 0 ; i < SIZEOFARRAY(myNetwork.key); i++){
    myNetwork.key[i] = EEPROM.read(address++);
    if(myNetwork.key[i] >= 0x80) myNetwork.key[i] = 0x20; // Discard useless characters
  }
}

void setNetworkSettingsToEEPROM(void){
  int address = 18;
  // Enable
  EEPROM.update(address++, (myNetwork.enable ? 1 : 0));
  // Accespoint and key
  for(int i = 0 ; i < SIZEOFARRAY(myNetwork.accesspoint); i++){
     EEPROM.update(address++, myNetwork.accesspoint[i]);
  }
  for(int i = 0 ; i < SIZEOFARRAY(myNetwork.key); i++){
     EEPROM.update(address++, myNetwork.key[i]);
  }
}

void getWeekendModeSettingsFromEEPROM(void){
  int address = 55;
  // Enable
  char enable = EEPROM.read(address);
  if((enable == 0) || (enable == -1)){
   weekendMode.enable = false;
  }
  else{
    weekendMode.enable = true;
  }
  
  address = 56;
  if(enable != -1){ // EEPROM has already be written
    weekendMode.date.heures = EEPROM.read(address++);
    weekendMode.date.minutes = EEPROM.read(address++);
  }
  else{ // EEPROM never modified
    // Default setup (09:00)
    weekendMode.date.heures = 9;
    weekendMode.date.minutes = 0;

    setWeekendModeSettingsToEEPROM();
  }
}

void setWeekendModeSettingsToEEPROM(void){
  int address = 55;
  // Enable
  EEPROM.update(address++, (weekendMode.enable ? 1 : 0));
  // Time
  EEPROM.update(address++, weekendMode.date.heures);
  EEPROM.update(address++, weekendMode.date.minutes);
}
