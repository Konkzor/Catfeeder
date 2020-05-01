#include <Servo.h>
#include <Wire.h>
#include <SimpleTimer.h>
#include <ArduinoJson.h>
#include <LiquidCrystal.h>
#include <SoftwareSerial.h>

#undef DEBUG
#define SERVO
#define LCD

#ifdef DEBUG
 #define DEBUG_PRINT(x)  Serial.print (x)
 #define DEBUG_PRINTLN(x)  Serial.println (x)
#else
 #define DEBUG_PRINT(x)
 #define DEBUG_PRINTLN(x)
#endif
            
#define MAX_CONTENT_SIZE  110
char buffer_array[MAX_CONTENT_SIZE];

#define addr_flag 0

typedef struct{
  uint8_t secondes;
  uint8_t minutes;
  uint8_t heures; // format 24h
  uint8_t jourDeLaSemaine; // 1~7 = lundi, mardi, ... (for RTC purpose)
  uint8_t jour;
  uint8_t mois; 
  uint8_t annee; // format yy (ex 2012 -> 12)
}Date;

typedef struct{
  Date date;
  bool state; // 1 is done
  short nbrev; // revolution
}Date_s;

// RTC
#define DS1307_ADDRESS 0x68

// WIFI
#define NomduReseauWifi "Livebox-A536"
#define MotDePasse "4PGK3DGFrhQ5ZqnoWY"
#define IPraspberry "192.168.1.51"
#define PORTraspberry "5000"
#define GETschedule "/catfeeder/schedule"
#define GETlog "/catfeeder/logger"
#define GETtime "/time"
#define HTTP " HTTP/1.1\r\n"

SoftwareSerial ESP8266(10, 11);

// Feeder
#define timeForOneTurn 2000 // ms
Date_s date2feed[5];
int nb_meals = 4;
// Breakfast at 7H
// Lunch at 11H30
// Break at 17H
// Dinner at 21H

// Timer
SimpleTimer timer;
int timerId_time;
int timerId_sec;

// IOs
#ifdef SERVO
Servo myservo;
#endif
const int buttonPin = 2;    // the number of the pushbutton pin
const int ledGreenPin = 12;   // the number of the Greed LED pin

// LCD
#ifdef LCD
LiquidCrystal lcd(8, 7, 6, 5, 4, 3);
#endif

// Variables
short wifiState = 0;
bool flag_feed = true;
bool flag_feed_force = false;
short timeleft = 0;
Date date_t;
Date_s next_date_s;

void setup() {
  bool res = false;
  // ESP8266 baudrate setup
  ESP8266.begin(9600);
  
  // Serial
#ifdef DEBUG
  Serial.begin(115200);
#endif

  // LCD setup
#ifdef LCD
  lcd.begin(20, 4);
#endif
  // RTC setup (I2C)
  Wire.begin();
  
  // IOs
  pinMode(buttonPin, INPUT_PULLUP);
  pinMode(ledGreenPin, OUTPUT);
  //pinMode(ledBluePin, OUTPUT);
  // Interrupt on PushButon
  //attachInterrupt(digitalPinToInterrupt(buttonPin), ISR_feed, FALLING);

  // Connect to Wifi
  wifiState = connect2Wifi(true);
  if(wifiState) log2PI("?code=0");
  delay(2000);

  //RTC synchronization
  if(wifiState){
    res = getNetworkTime(&date_t);
    if (res) writeToRTC(&date_t);
    else readFromRTC(&date_t);
  }
  else readFromRTC(&date_t);
  delay(1000);
  
  // Setup feeder schedule
  next_date_s.date = date_t;
  next_date_s.nbrev = 2;
  setupDefaultFeeder(); // Default
  
  // Get schedule from Raspberry Pi
  if(wifiState){
    res = getScheduleFromRaspberry();
    delay(1000);
  }
  
#ifdef LCD
  printMainPage();
#endif
  // Timer setup
  timerId_time = timer.setInterval(60000, ISR_time); // Every 1 min
  timerId_sec = timer.setInterval(500, ISR_sec); // Every 500ms
}

void setupDefaultFeeder(void){
  nb_meals = 4;
  // Time setups
  date2feed[0].date.heures = 7;
  date2feed[0].date.minutes = 0;
  date2feed[0].nbrev = 2;
  date2feed[1].date.heures = 12;
  date2feed[1].date.minutes = 0;
  date2feed[1].nbrev = 2;
  date2feed[2].date.heures = 17;
  date2feed[2].date.minutes = 0;
  date2feed[2].nbrev = 1;
  date2feed[3].date.heures = 21;
  date2feed[3].date.minutes = 0;
  date2feed[3].nbrev = 2;
}

void loop() {
  if(flag_feed || flag_feed_force){
    timer.disable(timerId_sec);
    timer.disable(timerId_time);
    
    // Feed the cat
#ifdef LCD
    printTime2Eat();
#endif
    digitalWrite(ledGreenPin, HIGH);
    feedTheCat(next_date_s.nbrev);
    digitalWrite(ledGreenPin, LOW);

    // If not connected, try to reconnect
    if(!wifiState) wifiState = connect2Wifi(false);
    
    // If connected to the internet
    if(wifiState){
      String log_str = "?code=1&quantity=";
      log_str += String(next_date_s.nbrev);
      // Send log to server
      log2PI(log_str);
      // Get new schedule from server
      bool res = getScheduleFromRaspberry();
      delay(1000); // To let the message on display
      // If first meal of monday has been served, update RTC time
      if((next_date_s.date.heures == date2feed[0].date.heures) && (next_date_s.date.minutes == date2feed[0].date.minutes) && date_t.jourDeLaSemaine == 1){
        res = getNetworkTime(&date_t);
        delay(1000); // To let the message on display
        if (res) writeToRTC(&date_t);
      }
    }

    if(flag_feed) updateMeals(&next_date_s);
    
#ifdef LCD
    printMainPage();
#endif

    flag_feed = false;
    flag_feed_force = false;
    timer.enable(timerId_time);
    timer.enable(timerId_sec);
  }

  timer.run();
}

void updateMeals(Date_s* date_s){
  // State setup
  for(int i = 0 ; i < nb_meals ; i++){
    if(60*date_s->date.heures+date_s->date.minutes >= 60*date2feed[i].date.heures + date2feed[i].date.minutes){ // Meal is past
      date2feed[i].state = 1; // Served !
      timeleft = 0;
    }
    else{
      date2feed[i].state = 0;
      timeleft = (date2feed[i].date.heures - date_t.heures)*60 + (date2feed[i].date.minutes - date_t.minutes);
      *date_s = date2feed[i]; // For next time
      break;
    }
  }

  // If all meal are served, then reload all of them and set next meal as breakfast
  if (timeleft == 0){
    for(int i = 0 ; i < nb_meals ; i++){
       date2feed[i].state = 0; // Non-served !
    }
    timeleft = (24 - date_t.heures + date2feed[0].date.heures)*60 + (0 - date_t.minutes + date2feed[0].date.minutes);
    if (timeleft > 12*60) timeleft = 12*60;
    *date_s = date2feed[0];
  }
}

/*void ISR_feed(void){
  flag_feed_force = true;
}*/

/* ISR_time is called every 1 min */
void ISR_time(void){
  // Decrementes time left
  timeleft--;
  if(timeleft <= 0){
    // Ask for feed
    flag_feed = true;
  }
  // Update Time
  readFromRTC(&date_t);

  // Print on display
#ifdef LCD
  printMainPage();
#endif
  return;
}

void ISR_sec(void){
#ifdef LCD
  blinkColon();
#endif
}

void feedTheCat(const short revolutions){
#ifdef SERVO
   // Attach the servo to pin 9
  myservo.attach(9);
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
short connect2Wifi(bool reset){
#ifdef LCD
  lcd.clear();
  lcd.setCursor(8,1);
  lcd.print("WIFI");
  lcd.setCursor(4,2);
  lcd.print("CONNECTION...");
#endif

  bool state = false;
  if(reset) envoieAuESP8266("AT+RST");
  else envoieAuESP8266("AT");
  bool res = recoitDuESP8266(2000L, -1);
  envoieAuESP8266("AT+CWMODE=1"); // WIFI MODE STATION
  res = recoitDuESP8266(5000L, -1);
  envoieAuESP8266("AT+CWJAP=\"" + String(NomduReseauWifi) + "\",\"" + String(MotDePasse) + "\""); // JOIN ACCESS POINT
  res = recoitDuESP8266(7000L, -1);
  state = checkWiFi();
  envoieAuESP8266("AT+CIPMUX=0");
  res = recoitDuESP8266(2000L, -1);

#ifdef LCD
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
#endif

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

bool recoitDuESP8266(const long int timeout, char start_char){
  char c;
  char i = 0;
  bool bufferize = false;
  
  if(start_char == 0) bufferize = true;
  
  long int t_start = millis();
  while ((t_start + timeout) > millis())
  {
    if(ESP8266.available()>0){
      c = ESP8266.read();
      if(c == start_char){
        bufferize = true;
        i = 0;
        DEBUG_PRINT(">>");
      }
      DEBUG_PRINT(c);
      if(bufferize){
        buffer_array[i] = c;
        i++;
        if(i == MAX_CONTENT_SIZE){
          DEBUG_PRINTLN("RX buffer is full");
          break;
        }
      }
    }
  }
  return bufferize;
}

short getNetworkTime(Date* date){
#ifdef LCD
  lcd.clear();
  lcd.setCursor(1,1);
  lcd.print("TIME & DATE UPDATE");
#endif
  String request = "GET ";
  request += GETtime; // Get settings
  request += HTTP;
  // Send request. Exit if failed
  bool res = sendRequest(request, true);
  
  if(res){
    DEBUG_PRINTLN("JSON received");
    StaticJsonBuffer<100> jsonBuffer;
    // Get json object
    JsonObject& root = jsonBuffer.parseObject(buffer_array);
    if (!root.success()) {
      DEBUG_PRINTLN("Time: Parsing JSON failed");
      res = false;
    }
    else{
      DEBUG_PRINTLN("Time : Parsing JSON succeeded");
      // Get informations
      date->secondes = root["s"];
      date->minutes = root["mi"];
      date->heures = root["h"]; // GMT + 2
      if(root["js"]==0) date->jourDeLaSemaine = 7; // 0 is sunday, 6 is saturday
      else date->jourDeLaSemaine = root["js"];
      date->jour = root["j"];
      date->mois= root["mo"];
      date->annee = root["y"];
  
      // No need to close the session, already done
      res = true;
    }
  }  
  else{
    DEBUG_PRINTLN("JSON not received.");
    res = false;
  }

#ifdef LCD
  if(res){ // Read from Raspberry Pi
    lcd.setCursor(5,2);
    lcd.print("SUCCEEDED");
  }
  else{
    lcd.setCursor(7,2);
    lcd.print("FAILED");
  }
#endif

  return res;
}

bool log2PI(String msg){
  String request = "GET ";
  request += GETlog; // Get settings
  request += msg;
  request += HTTP;
  
  // Send request. Exit if failed
  bool res = sendRequest(request, false);

  // Answer treatment
  /*if(response.indexOf("OK") == -1){
    DEBUG_PRINTLN("Log sending failed");
    return false;
  }
  DEBUG_PRINTLN("Log successfully sent");*/

  // No need to close the session, already done
  return true;
}

bool getScheduleFromRaspberry(void){
#ifdef LCD
  lcd.clear();
  lcd.setCursor(2,1);
  lcd.print("SCHEDULE UPDATE");
#endif

  String request = "GET ";
  request += GETschedule; // Get settings
  request += HTTP;
  // Send request. Exit if failed
  bool res = sendRequest(request, true);

  if(res){
    DEBUG_PRINTLN("JSON received");
    StaticJsonBuffer<100> jsonBuffer;
    // Get json object
    JsonObject& root = jsonBuffer.parseObject(buffer_array);
    if (!root.success()) {
      DEBUG_PRINTLN("Schedule: Parsing JSON failed");
      res=false;
    }
    else{
      DEBUG_PRINTLN("Schedule : Parsing JSON succeeded");
        
      // Get informations
      nb_meals = root["nb"];
      for(char i =0;i<nb_meals;i++){
        const char* repas = root["r"+String(i+1)];
        date2feed[i].date.heures = (String(repas).substring(0,String(repas).indexOf(':'))).toInt();
        date2feed[i].date.minutes = (String(repas).substring(String(repas).indexOf(':')+1,String(repas).indexOf(','))).toInt();
        date2feed[i].nbrev = (String(repas).substring(String(repas).indexOf(',')+1)).toInt();
      }
      
      // No need to close the session, already done
      res = true;
    }
  }
  else{
    DEBUG_PRINTLN("JSON not received.");
    res = false;
  }

#ifdef LCD
    if(res){ // Read from Raspberry Pi
      lcd.setCursor(5,2);
      lcd.print("SUCCEEDED");
    }
    else{
      lcd.setCursor(7,2);
      lcd.print("FAILED");
      }
#endif

  return res;
}

bool sendRequest(String request, bool waitForJSON){
  bool res = false;
  // Start session
  String cmd = "AT+CIPSTART=\"TCP\",\"";
  cmd += IPraspberry;
  cmd += "\",";
  cmd += PORTraspberry;
  ESP8266.println(cmd);
  res = recoitDuESP8266(2000L, -1);
  
  String cmd_send = "AT+CIPSEND=";
  cmd_send += String(request.length() + 2);
  ESP8266.println(cmd_send);
  delay(2000);
  res = recoitDuESP8266(2000L, -1);

  ESP8266.println(request);
  if(waitForJSON) res = recoitDuESP8266(5000L, '{');
  else res = recoitDuESP8266(5000L, -1);
  
  return res;
}
/******************************************************************************
 *                              LCD FUNCTIONS                                 *
 ******************************************************************************/
#ifdef LCD
void printDateAndHour(Date *date) {
  lcd.setCursor(0, 0); // Place le curseur à (0,0)
  lcd.print(date->jour / 10, DEC);// Affichage du jour sur deux caractéres
  lcd.setCursor(1, 0);
  lcd.print(date->jour % 10, DEC);
  lcd.setCursor(2, 0);
  lcd.print("/");
  lcd.setCursor(3, 0);
  lcd.print(date->mois / 10, DEC);// Affichage du mois sur deux caractéres
  lcd.setCursor(4, 0);
  lcd.print(date->mois % 10, DEC);
  lcd.setCursor(5, 0);
  lcd.print("/");
  lcd.setCursor(6, 0);
  lcd.print(date->annee / 10, DEC);// Affichage de l'année sur deux caractéres
  lcd.setCursor(7, 0);
  lcd.print(date->annee % 10, DEC);
  
  lcd.setCursor(15, 0);
  lcd.print(date->heures / 10, DEC); // Affichage de l'heure sur deux caractéres
  lcd.setCursor(16, 0);
  lcd.print(date->heures % 10, DEC);
  lcd.setCursor(17, 0);
  lcd.print(":");
  lcd.setCursor(18, 0);
  lcd.print(date->minutes / 10, DEC); // Affichage des minutes sur deux caractéres
  lcd.setCursor(19, 0);
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
  lcd.setCursor(10,0);
  if(state ==1){
    lcd.print("WF");
  }
  else{
    lcd.print("__");
  }
}

void printTimeLeft(void){
  lcd.setCursor(1,2);
  lcd.print("Next meal in:");
  lcd.setCursor(15, 2);
  lcd.print((timeleft/60) / 10, DEC); // Affichage de l'heure sur deux caractéres
  lcd.setCursor(16, 2);
  lcd.print((timeleft/60) % 10, DEC);
  lcd.setCursor(17, 2);
  lcd.print(":");
  lcd.setCursor(18, 2);
  lcd.print((timeleft % 60) / 10, DEC); // Affichage des minutes sur deux caractéres
  lcd.setCursor(19, 2);
  lcd.print((timeleft % 60) % 10, DEC);
}

void printMainPage(){
  lcd.clear();
  printDateAndHour(&date_t);
  printTimeLeft();
  printWifiState(wifiState);
}
#endif
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
