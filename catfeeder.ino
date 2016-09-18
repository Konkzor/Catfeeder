#include <EEPROM.h>
#include <Servo.h>
#include <Wire.h>
#include <SimpleTimer.h>
#include <ArduinoJson.h>
#include <LiquidCrystal.h>
#include <SoftwareSerial.h>

size_t MAX_CONTENT_SIZE = 110;
const char addr_flag = 0;
const char addr_nbrepas = 1;
const char addr_h1 = 2;
const char addr_min1 = 3;
const char addr_tours1 = 4;

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
String NomduReseauWifi="Bbox-SandyEtMat";
String MotDePasse="576CC166AEC4AF5CA513334FEF7DD2";
#define IPraspberry "192.168.1.96"
#define GETfromPI "GET /schedule HTTP/1.1\r\n\r\n"
#define GET2PI "GET /log/"
#define GETtime "GET /time HTTP/1.1\r\n\r\n"

SoftwareSerial ESP8266(10, 11);

// Feeder
#define timeForOneTurn 2000 // ms
#define weightPerFeed 20 // g
Meal meals[10];
char nextmeal = 0;
char nb_meals = 4; // Default
// Breakfast (0) at 7H
// Lunch (1) at 11H30
// Break (2) at 17H
// Dinner (3) at 21H

// Timer
SimpleTimer timer;
int timerId_time;
int timerId_sec;

// IOs
Servo myservo;
const char buttonPin = 2;    // the number of the pushbutton pin
const char ledGreenPin = 12;   // the number of the Greed LED pin
//const int ledBluePin = ??;

// LCD
LiquidCrystal lcd(8, 7, 6, 5, 4, 3);

// Variables
char wifiState = 0;
bool flag_feed = true;
bool flag_button = false;
short weightPerDay = 0;
short timeleft = 0;
Date date_t;

void setup() {
  // ESP8266 baudrate setup
  ESP8266.begin(115200);
  envoieAuESP8266("AT+CIOBAUD=9600");
  recoitDuESP8266(4000);
  ESP8266.begin(9600);
  
  // Serial
  Serial.begin(9600);

  // LCD setup
  lcd.begin(20, 4);

  // RTC setup (I2C)
  Wire.begin();
  
  // IOs
  pinMode(buttonPin, INPUT_PULLUP);
  pinMode(ledGreenPin, OUTPUT);
  //pinMode(ledBluePin, OUTPUT);
  // Interrupt on PushButon
  attachInterrupt(digitalPinToInterrupt(buttonPin), ISR_feed, FALLING);

  // Connect to Wifi
  lcd.setCursor(8,1);
  lcd.print("WIFI");
  lcd.setCursor(4,2);
  lcd.print("CONNECTION...");
  wifiState = connect2Wifi();
  lcd.clear();
  lcd.setCursor(2,1);
  lcd.print("WIFI CONNECTION");
  if(wifiState == 1){
      lcd.setCursor(5,2);
      lcd.print("SUCCEEDED");
  }
  else {
      lcd.setCursor(7,2);
      lcd.print("FAILED");
  }
  delay(2000);

  //RTC synchronization
  if(wifiState == 1){
    short state = getNetworkTime(&date_t);
    //if (state == 1) writeToRTC(&date_t);
  }
  else readFromRTC(&date_t);

  // Setup feeder schedule
  setupDefaultFeeder(); // Default
  //readSettingsFromEEPROM(); // Read from EEPROM
  if(wifiState == 1){
    lcd.clear();
    lcd.setCursor(2,1);
    lcd.print("SCHEDULE UPDATE");
    if(getSettingsFromRaspberry() == 1){ // Read from Raspberry Pi
      lcd.setCursor(9,2);
      lcd.print("OK");
      //writeSettingsToEEPROM();
    }
    else {
      lcd.setCursor(6,2);
      lcd.print("FAILURE");
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
    weightPerDay+=weightPerFeed;
    if (wifiState == 0) wifiState = connect2Wifi(); // Try to reconnect
    send2PI(String(weightPerDay));
    
    findNextMeal();
    if (nextmeal == 0){ // If next meal to serve is the breakfast
      weightPerDay = 0;
    }

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
  for(int i = 0 ; i < nb_meals ; i++){
    if(60*date_t.heures+date_t.minutes <= 60*meals[i].heures + meals[i].minutes){ // If Meal is not past, nextmeal is found
      nextmeal = i;
      break;
    }
  }
  // If all meal are past (it is between last meal of the day and midnight)
  if(nextmeal == -1) nextmeal = 0;
}

void ISR_feed(void){
  //flag_button = true;
}

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
char connect2Wifi(void){
  envoieAuESP8266("AT");//+RST"); // WORKING ?
  Serial.print(recoitDuESP8266(2000));
  envoieAuESP8266("AT+CWMODE=1"); // WIFI MODE STATION
  Serial.print(recoitDuESP8266(7000));
  envoieAuESP8266("AT+CWJAP=\"" + NomduReseauWifi + "\",\"" + MotDePasse + "\""); // JOIN ACCESS POINT
  String res = recoitDuESP8266(5000);
  //Serial.print(res);
  if (res.indexOf("WIFI GOT IP") != -1) return 1;
  else return 0;
}

char checkWiFi(void){
  envoieAuESP8266("AT+CIFSR");
  delay(2000);
  if(ESP8266.find("OK")){
    return 1;
  }
  else{
    return 0;
  }
}

void envoieAuESP8266(String commande){
  ESP8266.println(commande);
}

String recoitDuESP8266(const int timeout){
  String reponse = "res : ";
  long int time = millis();
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

void sendDebug(String cmd){
  //Serial.print("SEND: ");
  //Serial.println(cmd);
  ESP8266.println(cmd);
}

char getNetworkTime(Date* date){
  char buffer_array[MAX_CONTENT_SIZE];
  sprintf(buffer_array, "{\"h\": ");
  // Start session
  String cmd = "AT+CIPSTART=\"TCP\",\"";
  cmd += IPraspberry;
  cmd += "\",80";
  String request = GETtime; // Get settings
  
  // Send request. Exit if failed
  if(sendRequest(cmd, request)==0) return 0;
// Get json object
  int i;
  if(ESP8266.find("\"h\": "))
  {
    for (i = 5; i < MAX_CONTENT_SIZE; i++)
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
  Serial.println(buffer_array);
  if(i == MAX_CONTENT_SIZE-1){ // The JSon object is too big
    sendDebug("AT+CIPCLOSE");
    return 0;
  }
  // Answer treatment
  // Parse content
  StaticJsonBuffer<85> jsonBuffer;
  JsonObject& root = jsonBuffer.parseObject(buffer_array);

  // Test if parsing succeeded
  if (!root.success()) {
    Serial.println("Time : parseObject() failed");
    return 0;
  }
  Serial.println("Time : parseObject() succeeded");
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
  return 1;
}

char send2PI(String value){
  // Start session
  String cmd = "AT+CIPSTART=\"TCP\",\"";
  cmd += IPraspberry;
  cmd += "\",80";
  String request = GET2PI; // Get settings
  request += value;
  request +=  " HTTP/1.1\r\n\r\n";
  
  // Send request. Exit if failed
  if(sendRequest(cmd, request)==0) return 0;

  // Answer treatment
  if(ESP8266.find("OK")){
    //Serial.println("RECEIVED: OK");
  }
  else{
    //Serial.println("RECEIVED: Error");
    return 0;
  }

  // Close session
  sendDebug("AT+CIPCLOSE");
  return 1;
}

char getSettingsFromRaspberry(void){
  char buffer_array[MAX_CONTENT_SIZE];
  sprintf(buffer_array, "{\"nb\": ");
  // Start session
  String cmd = "AT+CIPSTART=\"TCP\",\"";
  cmd += IPraspberry;
  cmd += "\",80";
  String request = GETfromPI; // Get settings
  
  // Send request. Exit if failed
  if(sendRequest(cmd, request)==0) return 0;
  // Get json object
  int i;
  if(ESP8266.find("\"nb\": "))
  {
    for (i = 6; i < MAX_CONTENT_SIZE; i++)
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
  //Serial.println(buffer_array);
  if(i == MAX_CONTENT_SIZE-1){ // The JSon object is too big
    sendDebug("AT+CIPCLOSE");
    return 0;
  }
  // Answer treatment
  // Parse content
  StaticJsonBuffer<100> jsonBuffer;
  JsonObject& root = jsonBuffer.parseObject(buffer_array);

  // Test if parsing succeeded
  if (!root.success()) {
    Serial.println("Schedule : parseObject() failed");
    return 0;
  }
  Serial.println("Schedule : parseObject() succeeded");
  // Get informations
  nb_meals = root["nb"];
  for(int i =0;i<nb_meals;i++){
    const char* repas = root["r"+String(i+1)];
    meals[i].heures = (String(repas).substring(0,String(repas).indexOf(':'))).toInt();
    meals[i].minutes = (String(repas).substring(String(repas).indexOf(':')+1,String(repas).indexOf(','))).toInt();
    meals[i].nb_rev = (String(repas).substring(String(repas).indexOf(',')+1)).toInt();
  }
  // Close session
  sendDebug("AT+CIPCLOSE");
  return 1;
}

char sendRequest(String cmd, String request){
  sendDebug(cmd);
  delay(2000);
  if(ESP8266.find("Error")){
    //Serial.print("RECEIVED: Error");
    return 0;
  }
  
  ESP8266.print("AT+CIPSEND=");
  ESP8266.println(request.length());
  if(ESP8266.find(">")){
    //Serial.print(">");
    //Serial.print(request);
    ESP8266.print(request);
  }else{
    sendDebug("AT+CIPCLOSE");
    return 0;
  }
  return 1;
}
/******************************************************************************
 *                              LCD FUNCTIONS                                 *
 ******************************************************************************/
void printDateAndHour(Date *date) {
  lcd.setCursor(0, 0); // Place le curseur à (0,0)
  /*char datetime[20];
  sprintf(datetime, "%d%d/%d%d/%d%d       %d%d:%d%d",date->jour/10,date->jour % 10,date->mois/10,date->mois%10,date->annee/10,date->annee%10,date->heures/10,date->heures%10, date->minutes/10,date->minutes%10);
  lcd.print(datetime);*/
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
char readSettingsFromEEPROM(){
  if(EEPROM.read(addr_flag) != 0){
    nb_meals = EEPROM.read(addr_nbrepas);
    for(int i=0;i<nb_meals;i++){
      meals[i].heures = EEPROM.read(addr_h1+3*i);
      meals[i].minutes = EEPROM.read(addr_min1+3*i);
      meals[i].nb_rev = EEPROM.read(addr_tours1+3*i);
    }
    return 1;
  }
  return 0;
}
void writeSettingsToEEPROM(){
  EEPROM.write(addr_nbrepas, nb_meals);
  for(int i=0;i<nb_meals;i++){
      EEPROM.write(addr_h1+3*i, meals[i].heures);
      EEPROM.write(addr_min1+3*i, meals[i].minutes);
      EEPROM.write(addr_tours1+3*i, meals[i].nb_rev);
    }
  EEPROM.write(addr_flag, 1);
}

