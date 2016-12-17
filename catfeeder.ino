#include <EEPROM.h>
#include <Servo.h>
#include <Wire.h>
#include <SimpleTimer.h>
#include <ArduinoJson.h>
#include <LiquidCrystal.h>
#include <SoftwareSerial.h>

//#define  DEBUG
#define MAX_CONTENT_SIZE  110
#define addr_flag 0
//addr_nbrepas addr_flag+1
//addr_h1 addr_flag+2
//addr_min1 addr_flag+3
//addr_tours1 addr_flag+4

typedef struct{
  uint8_t secondes;
  uint8_t minutes;
  uint8_t heures; // format 24h
  uint8_t jourDeLaSemaine; // 1~7 = lundi, mardi, ...
  uint8_t jour;
  uint8_t mois; 
  uint8_t annee; // format yy (ex 2012 -> 12)
}Date;

typedef struct{
  char heures; //[0;23]
  char minutes; //[0;59]
  char nb_rev; //[0;10]
}Meal;

// RTC
#define DS1307_ADDRESS 0x68

// WIFI
#define NomduReseauWifi "Bbox-SandyEtMat"
#define MotDePasse "576CC166AEC4AF5CA513334FEF7DD2"
#define IPraspberry "192.168.1.96"
#define Host "Host: www.atloupiotte.ddns.net\r\n\r\n"
#define GETschedule "/schedule"
#define GETlog "/logger"
#define GETtime "/time"
#define HTTP " HTTP/1.1\r\n"
SoftwareSerial ESP8266(10, 11);

// Feeder
#define timeForOneTurn 2000 // ms
//#define weightPerFeed 20 // g
Meal meals[5];
char nextmeal = 0;
char nb_meals = 4; // Default

// Timer
SimpleTimer timer;
char timerId_time;
char timerId_sec;

// IOs
#define buttonPin  2    // the number of the pushbutton pin
#define ledGreenPin  12   // the number of the Greed LED pin
//const int ledBluePin = ??;

// LCD
LiquidCrystal lcd(8, 7, 6, 5, 4, 3);

// Variables
bool wifiState = true;
bool flag_feed = true;
//bool flag_button = false;
//short weightPerDay = 0;
short timeleft = 0;
Date date_t;

void setup() {
  // ESP8266 baudrate setup
  ESP8266.begin(115200);
  envoieAuESP8266("AT+CIOBAUD=9600");
  recoitDuESP8266(4000);
  ESP8266.begin(9600);
  
  // Serial
#ifdef DEBUG
  Serial.begin(9600);
#endif
  // LCD setup
  lcd.begin(20, 4);

  // RTC setup (I2C)
  Wire.begin();
  
  // IOs
  pinMode(buttonPin, INPUT_PULLUP);
  pinMode(ledGreenPin, OUTPUT);
  //pinMode(ledBluePin, OUTPUT);
  // Interrupt on PushButon
  //attachInterrupt(digitalPinToInterrupt(buttonPin), ISR_feed, FALLING);

  // Connect to Wifi
  lcd.setCursor(8,1);
  lcd.print("WIFI");
  lcd.setCursor(4,2);
  lcd.print("CONNECTION...");
  wifiState = connect2Wifi();
  lcd.clear();
  lcd.setCursor(2,1);
  lcd.print("WIFI CONNECTION");
  if(wifiState){
      lcd.setCursor(5,2);
      lcd.print("SUCCEEDED");
      log2PI("?code=0");
  }
  else {
      lcd.setCursor(7,2);
      lcd.print("FAILED");
  }
  delay(2000);

  //RTC synchronization
  if(wifiState){
    if (getNetworkTime(&date_t)) writeToRTC(&date_t);
    else readFromRTC(&date_t);
  }
  else readFromRTC(&date_t);
  
  // Setup feeder schedule
  setupDefaultFeeder(); // Default
  // Read from EEPROM
  lcd.clear();
  lcd.setCursor(2,1);
  lcd.print("SCHEDULE UPDATE");
  if(readSettingsFromEEPROM()){ 
    lcd.setCursor(3,2);
    lcd.print("EEPROM");
  }
  delay(2000);
  // Get schedule from Raspberry Pi
  if(wifiState){
    if(getSettingsFromRaspberry()){ // Read from Raspberry Pi
      lcd.setCursor(12,2);
      lcd.print("WIFI");
      writeSettingsToEEPROM(); // Write it in EEPROM once received
    }
  }
  delay(1000);
  printMainPage();
  delay(1000);
  // Timer setup
  timerId_time = timer.setInterval(60000, ISR_time); // Every 1 min
  timerId_sec = timer.setInterval(500, ISR_sec); // Every 500ms
}

void setupDefaultFeeder(void){
  nb_meals = 4;
  // Time setups
  meals[0].heures = 7;
  meals[0].minutes = 0;
  meals[0].nb_rev = 3;
  meals[1].heures = 11;
  meals[1].minutes = 30;
  meals[1].nb_rev = 2;
  meals[2].heures = 17;
  meals[2].minutes = 0;
  meals[2].nb_rev = 2;
  meals[3].heures = 21;
  meals[3].minutes = 0;
  meals[3].nb_rev = 2;
}

void loop() {
  if(flag_feed){
    timer.disable(timerId_sec);
    timer.disable(timerId_time);
    
    // Feed the cat
    printTime2Eat();
    digitalWrite(ledGreenPin, HIGH);
    feedTheCat(meals[nextmeal].nb_rev);
    digitalWrite(ledGreenPin, LOW);
    
    // Send new state to Raspberry PI
    //weightPerDay+=weightPerFeed;
    if(!wifiState) wifiState = connect2Wifi(); // Try to reconnect
    if(wifiState){
      if(meals[nextmeal].nb_rev == 1) log2PI("?code=1&quantity=1");
      else if (meals[nextmeal].nb_rev == 2) log2PI("?code=1&quantity=2");
      else if (meals[nextmeal].nb_rev == 3) log2PI("?code=1&quantity=3");
      else if (meals[nextmeal].nb_rev == 4) log2PI("?code=1&quantity=4");
      else if (meals[nextmeal].nb_rev == 5) log2PI("?code=1&quantity=5");
      else if (meals[nextmeal].nb_rev == 6) log2PI("?code=1&quantity=6");
      else if (meals[nextmeal].nb_rev == 7) log2PI("?code=1&quantity=7");
      else if (meals[nextmeal].nb_rev == 8) log2PI("?code=1&quantity=8");
      else if (meals[nextmeal].nb_rev == 9) log2PI("?code=1&quantity=9");
      else if (meals[nextmeal].nb_rev == 10) log2PI("?code=1&quantity=10");
      if(getSettingsFromRaspberry()){ // Check new schedule
        writeSettingsToEEPROM(); // Write it in EEPROM once received
      }
    }
    
    findNextMeal();
    /*if (nextmeal == 0){ // If next meal to serve is the breakfast
      weightPerDay = 0;
    }*/

    updateTimeLeft(); // To be sure timeleft has been computed
    printMainPage();

    flag_feed = false;
    timer.enable(timerId_time);
    timer.enable(timerId_sec);
  }

  timer.run();
}

void findNextMeal(){
  // State setup
  nextmeal = -1;
  for(char i = 0 ; i < nb_meals ; i++){
    if(60*date_t.heures+date_t.minutes <= 60*meals[i].heures + meals[i].minutes){ // If Meal is not past, nextmeal is found
      nextmeal = i;
      break;
    }
  }
  // If all meal are past (it is between last meal of the day and midnight)
  if(nextmeal == -1) nextmeal = 0;
}

/*void ISR_feed(void){
  //flag_button = true;
}*/

/* ISR_time is called every 1 min */
void ISR_time(void){
  // Update Time
  readFromRTC(&date_t);
  
  // Compute time left every minute ==> no more shift
  updateTimeLeft();
  
  if(timeleft < 0){
    // Ask for feed
    flag_feed = true;
  }

  // Print on display
  printMainPage();
  return;
}

void ISR_sec(void){
  blinkColon();
}

void updateTimeLeft(){
  if (nextmeal == 0 && date_t.heures >= meals[3].heures) // Next meal is breakfast the day after
    timeleft = (24 + meals[nextmeal].heures - date_t.heures)*60 + (meals[nextmeal].minutes - date_t.minutes);
  else
    timeleft = (meals[nextmeal].heures - date_t.heures)*60 + (meals[nextmeal].minutes - date_t.minutes);
  return;
}

void feedTheCat(const char revolutions){
  Servo myservo;
   // Attach the servo to pin 9
  myservo.attach(9);
  // First turn backward to avoid jamming
  myservo.write(85);
  delay(500);
  // Then turn forward
  for(char i = 0 ; i < revolutions ; i++){
    myservo.write(95);
    delay(timeForOneTurn);
  }
  myservo.write(90);
  // Detach the servo to rest
  myservo.detach();
}

/******************************************************************************
 *                              WIFI FUNCTIONS                                *
 ******************************************************************************/
bool connect2Wifi(void){
  envoieAuESP8266("AT");//+RST"); // WORKING ?
  String res = recoitDuESP8266(2000);
#ifdef DEBUG
  Serial.print(res);
#endif
  envoieAuESP8266("AT+CWMODE=1"); // WIFI MODE STATION
  res = recoitDuESP8266(7000);
#ifdef DEBUG
  Serial.print(res);
#endif
  envoieAuESP8266("AT+CWJAP=\"" + String(NomduReseauWifi) + "\",\"" + String(MotDePasse) + "\""); // JOIN ACCESS POINT
  res = recoitDuESP8266(5000);
#ifdef DEBUG
  Serial.print(res);
#endif
  if (res.indexOf("WIFI GOT IP") != -1) return true;
  else return false;
}

/*char checkWiFi(void){
  envoieAuESP8266("AT+CIFSR");
  delay(2000);
  if(ESP8266.find("OK")){
    return 1;
  }
  else{
    return 0;
  }
}*/

void envoieAuESP8266(String commande){
  ESP8266.println(commande);
}

String recoitDuESP8266(const short timeout){
  String reponse = "res : ";
  unsigned long time = millis();
  while ( (time + timeout) > millis())
  {
    while (ESP8266.available())
    {
      char c = ESP8266.read();
      reponse += c;
    }
  }
  return reponse;
}

bool getNetworkTime(Date* date){
  // Start session
  String cmd = "AT+CIPSTART=\"TCP\",\"";
  cmd += IPraspberry;
  cmd += "\",80";
  String request = "GET ";
  request += GETtime; // Get settings
  request += HTTP;
  // Send request. Exit if failed
  if(!sendRequest(cmd, request)) return false;
  
  // Get json object
  char buffer_array[MAX_CONTENT_SIZE];
  if(!decodeJson("\"h\": ", buffer_array)) return false;
  StaticJsonBuffer<100> jsonBuffer;
  JsonObject& root = jsonBuffer.parseObject(buffer_array);
  if (!root.success()) {
#ifdef DEBUG
    Serial.println("Time : parseObject() failed");
#endif
    return false;
  }
#ifdef DEBUG
  Serial.println("Time : parseObject() succeeded");
#endif
  // Get informations
  date->secondes = root["s"];
  date->minutes = root["mi"];
  date->heures = root["h"]; // GMT + 2
  if(root["js"]==0) date->jourDeLaSemaine = 7;
  else date->jourDeLaSemaine = root["js"];
  date->jour = root["j"];
  date->mois= root["mo"];
  date->annee = root["y"];
  
  // Close session
  sendDebug("AT+CIPCLOSE");
  return true;
}

bool log2PI(String msg){
  // Start session
  String cmd = "AT+CIPSTART=\"TCP\",\"";
  cmd += IPraspberry;
  cmd += "\",80";
  String request = "GET ";
  request += GETlog; // Get settings
  request += msg;
  request += HTTP;
  
  // Send request. Exit if failed
  if(!sendRequest(cmd, request)) return false;

  // Answer treatment
  if(ESP8266.find("OK")){
#ifdef DEBUG
    Serial.println("RECEIVED: OK");
#endif
  }
  else{
#ifdef DEBUG
    Serial.println("RECEIVED: Error");
#endif
    return false;
  }

  // Close session
  sendDebug("AT+CIPCLOSE");
  return true;
}

bool getSettingsFromRaspberry(void){
  // Start session
  String cmd = "AT+CIPSTART=\"TCP\",\"";
  cmd += IPraspberry;
  cmd += "\",80";
  String request = "GET ";
  request += GETschedule; // Get settings
  request += HTTP;
  // Send request. Exit if failed
  if(!sendRequest(cmd, request)) return false;
  
  // Get json object
  char buffer_array[MAX_CONTENT_SIZE];
  if(!decodeJson("\"nb\": ", buffer_array)) return false;
  StaticJsonBuffer<100> jsonBuffer;
  JsonObject& root = jsonBuffer.parseObject(buffer_array);
  if (!root.success()) {
#ifdef DEBUG
    Serial.println("Schedule : parseObject() failed");
#endif
    return false;
  }
#ifdef DEBUG
  Serial.println("Schedule parseObject() succeeded");
#endif
  // Get informations
  nb_meals = root["nb"];
  for(char i =0;i<nb_meals;i++){
    const char* repas = root["r"+String(i+1)];
    meals[i].heures = (String(repas).substring(0,String(repas).indexOf(':'))).toInt();
    meals[i].minutes = (String(repas).substring(String(repas).indexOf(':')+1,String(repas).indexOf(','))).toInt();
    meals[i].nb_rev = (String(repas).substring(String(repas).indexOf(',')+1)).toInt();
  }
  // Close session
  sendDebug("AT+CIPCLOSE");
  return true;
}

bool decodeJson(char* start, char* buffer_array)
{
  sprintf(buffer_array, "{%s", start);
  char i;
  if(ESP8266.find(start))
  {
    for (i = String(start).length(); i < MAX_CONTENT_SIZE; i++)
    {
      if (ESP8266.available())  //new characters received?
      {
        char c = ESP8266.read();
        buffer_array[i] = c;
        if(c == '}') break;
      }
      else i--;  //if not, keep going round loop until we've got all the characters
    }
  }
  if(i == MAX_CONTENT_SIZE-1){ // The JSon object is too big
    sendDebug("AT+CIPCLOSE");
    return false;
  }
  return true;
}

bool sendRequest(String cmd, String request){
  sendDebug(cmd);
  delay(2000);
  if(ESP8266.find("Error")){
#ifdef DEBUG
    Serial.print("RECEIVED: Error");
#endif
    return false;
  }
  
  ESP8266.print("AT+CIPSEND=");
  ESP8266.println((request+Host).length());
  if(ESP8266.find(">")){
#ifdef DEBUG
    Serial.print(">");
    Serial.print(request);
#endif
    ESP8266.print(request+Host);
  }else{
    sendDebug("AT+CIPCLOSE");
    return false;
  }
  return true;
}

void sendDebug(String cmd){
#ifdef DEBUG
  Serial.print("SEND: ");
  Serial.println(cmd);
#endif
  ESP8266.println(cmd);
}
/******************************************************************************
 *                              LCD FUNCTIONS                                 *
 ******************************************************************************/
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
  static bool state = true;
  lcd.setCursor(17, 0);
  
  if(state) lcd.print(":");
  else lcd.print(" ");

  state=!state;
}

void printWifiState(bool state){
  lcd.setCursor(10,0);
  if(state) lcd.print("WF");
  else lcd.print("__");
}

void printTimeLeft(void){
  lcd.setCursor(1 ,2);
  lcd.print("Next meal in ");
  lcd.setCursor(14, 2);
  lcd.print((timeleft/60) / 10, DEC); // Affichage de l'heure sur deux caractéres
  lcd.setCursor(15, 2);
  lcd.print((timeleft/60) % 10, DEC);
  lcd.setCursor(16, 2);
  lcd.print(":");
  lcd.setCursor(17, 2);
  lcd.print((timeleft % 60) / 10, DEC); // Affichage des minutes sur deux caractéres
  lcd.setCursor(18, 2);
  lcd.print((timeleft % 60) % 10, DEC);
}

void printMainPage(){
  lcd.clear();
  printDateAndHour(&date_t);
  printTimeLeft();
  printWifiState(wifiState);
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
bool readSettingsFromEEPROM(){
  if(EEPROM.read(addr_flag) != 0){
    nb_meals = EEPROM.read(addr_flag+1);
    for(char i=0;i<nb_meals;i++){
      meals[i].heures = EEPROM.read(addr_flag+2+3*i);
      meals[i].minutes = EEPROM.read(addr_flag+3+3*i);
      meals[i].nb_rev = EEPROM.read(addr_flag+4+3*i);
    }
    return true;
  }
  return false;
}
void writeSettingsToEEPROM(){
  EEPROM.write(addr_flag+1, nb_meals);
  for(char i=0;i<nb_meals;i++){
      EEPROM.write(addr_flag+2+3*i, meals[i].heures);
      EEPROM.write(addr_flag+3+3*i, meals[i].minutes);
      EEPROM.write(addr_flag+4+3*i, meals[i].nb_rev);
    }
  EEPROM.write(addr_flag, 1);
}

