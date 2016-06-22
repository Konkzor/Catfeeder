#include <Servo.h>
#include <Wire.h>
#include <SimpleTimer.h>
#include <stdlib.h>
#include <LiquidCrystal.h>
#include <SoftwareSerial.h>

typedef struct{
  uint8_t secondes;
  uint8_t minutes;
  uint8_t heures; // format 24h
  uint8_t jourDeLaSemaine; // 0~7 = lundi, mardi, ...
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
#define IProuter "192.168.1.254"
#define IPthingspeak "api.thingspeak.com" //"184.106.153.149"
String NomduReseauWifi = "Bbox-SandyEtMat";
String MotDePasse = "576CC166AEC4AF5CA513334FEF7DD2";
String GET = "GET /update?api_key=V5SYYI8KDAHKK5AX&field1=";

SoftwareSerial ESP8266(10, 11);

// Feeder
#define timeForOneTurn 2000 // ms
#define weightPerFeed 20 // g
Date_s date2feed[4];
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
Servo myservo;
const int buttonPin = 2;    // the number of the pushbutton pin
const int ledGreenPin = 12;   // the number of the Greed LED pin
//const int ledBluePin = ??;

// LCD
LiquidCrystal lcd(8, 7, 6, 5, 4, 3);

// Variables
short wifiState = 0;
bool flag_feed = true;
short weightPerDay = 0;
short timeleft = 0;
Date date_t;
Date_s next_date_s;

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
  lcd.setCursor(3,1);
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
    if (state == 1) writeToRTC(&date_t);
  }
  else readFromRTC(&date_t);
  
  next_date_s.date = date_t;
  next_date_s.nbrev = 3;
  setupFeeder();
  
  delay(1000);
  printMainPage();
  delay(1000);
  // Timer setup
  timerId_time = timer.setInterval(60000, ISR_time); // Every 1 min
  timerId_sec = timer.setInterval(500, ISR_sec); // Every 500ms
}

void setupFeeder(void){
  // Time setups
  date2feed[0].date.heures = 7;
  date2feed[0].date.minutes = 0;
  date2feed[0].nbrev = 3;
  date2feed[1].date.heures = 11;
  date2feed[1].date.minutes = 30;
  date2feed[1].nbrev = 2;
  date2feed[2].date.heures = 17;
  date2feed[2].date.minutes = 0;
  date2feed[2].nbrev = 2;
  date2feed[3].date.heures = 21;
  date2feed[3].date.minutes = 0;
  date2feed[3].nbrev = 2;
}

void loop() {
  if(flag_feed){
    timer.disable(timerId_sec);
    timer.disable(timerId_time);
    
    // Feed the cat
    printTime2Eat();
    digitalWrite(ledGreenPin, HIGH);
    feedTheCat(next_date_s.nbrev);
    digitalWrite(ledGreenPin, LOW);
    
    // Send new value through WiFi
    weightPerDay+=weightPerFeed;
    if (wifiState == 0) wifiState = connect2Wifi(); // Try to reconnect
    send2TP(String(weightPerDay));
    
    updateMeals(&next_date_s);
    if (next_date_s.date.heures == date2feed[0].date.heures && next_date_s.date.minutes == date2feed[0].date.minutes){ // If next meal to serve is the breakfast
      weightPerDay = 0;
    }
    
    printMainPage();

    flag_feed = false;
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
      timeleft = (date2feed[i].date.heures - date_s->date.heures)*60 + (date2feed[i].date.minutes - date_s->date.minutes);
      *date_s = date2feed[i]; // For next time
      break;
    }
  }

  // If all meal are served, then reload all of them and set next meal as breakfast
  if (timeleft == 0){
    for(int i = 0 ; i < nb_meals ; i++){
       date2feed[i].state = 0; // Non-served !
    }
    timeleft = (24 - date_s->date.heures + date2feed[0].date.heures)*60 + (0 - date_s->date.minutes + date2feed[0].date.minutes);
    if (timeleft > 12*60) timeleft = 12*60;
    *date_s = date2feed[0];
  }
}

void ISR_feed(void){
  flag_feed = true;
}
/* ISR_time is called every 1 min */
void ISR_time(void){
  // Decrementes time left
  timeleft--;
  if(timeleft <= 0){
    // Ask for feed
    flag_feed = true;
  }
  // Check if Wi-Fi is available
  //wifiState = checkWiFi(); PROBLEMS
  // Update Time
  //wifiState = getNetworkTime(&date_t);
  readFromRTC(&date_t);

  // Print on display
  printMainPage();
  return;
}

void ISR_sec(void){
  blinkColon();
}

void feedTheCat(const short revolutions){
   // Attach the servo to pin 9
  myservo.attach(9);
  // First turn backward to avoid jamming
  myservo.write(85);
  delay(500);
  // Then turn forward
  for(short i = 0 ; i < revolutions ; i++){
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
short connect2Wifi(void){
  envoieAuESP8266("AT");//+RST"); // WORKING ?
  Serial.print(recoitDuESP8266(2000));
  envoieAuESP8266("AT+CWMODE=1"); // WIFI MODE STATION
  Serial.print(recoitDuESP8266(7000));
  envoieAuESP8266("AT+CWJAP=\"" + NomduReseauWifi + "\",\"" + MotDePasse + "\""); // JOIN ACCESS POINT
  String res = recoitDuESP8266(5000);
  Serial.print(res);
  if (res.indexOf("WIFI GOT IP") != -1) return 1;
  else return 0;
}

short checkWiFi(void){
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
  Serial.print("SEND: ");
  Serial.println(cmd);
  ESP8266.println(cmd);
}

short getNetworkTime(Date* date){
  // Send served food during the day
  // Start session
  String cmd = "AT+CIPSTART=\"TCP\",\"";
  cmd += IProuter;
  cmd += "\",80";
  sendDebug(cmd);
  delay(2000);
  if(ESP8266.find("Error")){
    Serial.print("RECEIVED: Error");
    return 0;
  }
  cmd = "GET /index.html HTTP/1.1\r\n\r\n";
  ESP8266.print("AT+CIPSEND=");
  ESP8266.println(cmd.length());
  if(ESP8266.find(">")){
    Serial.print(">");
    Serial.print(cmd);
    ESP8266.print(cmd);
  }else{
    sendDebug("AT+CIPCLOSE");
    return 0;
  }

  char date_string[31];
  if (ESP8266.find("Date: ")) //get the date line from the http header (for example)
  {
    int i;
    for (i = 0; i < 31; i++) //31 this should capture the 'Date: ' line from the header
    {
      if (ESP8266.available())  //new characters received?
      {
        char c = ESP8266.read(); //print to console
        Serial.write(c);
        date_string[i] = c;
      }
      else i--;  //if not, keep going round loop until we've got all the characters
    }
  }
  else return 0;
  
  // Close session
  sendDebug("AT+CIPCLOSE");

  // Retrieve hour and date
  char dayweek[4];
  char month[4];
  int day, years, hours, minutes, seconds;
  sscanf (date_string,"%3s%*1s %d %3s %4d %d:%d:%d",dayweek,&day, month, &years, &hours, &minutes, &seconds);
  date->secondes = seconds;
  date->minutes = minutes;
  date->heures = (hours+2)%24; // GMT + 2
  
  if(strncmp(dayweek,"Mon",3) == 0) date->jourDeLaSemaine=1;
  else if(strncmp(dayweek,"Tue",3) == 0) date->jourDeLaSemaine=2;
  else if(strncmp(dayweek,"Wed",3) == 0) date->jourDeLaSemaine=3;
  else if(strncmp(dayweek,"Thu",3) == 0) date->jourDeLaSemaine=4;
  else if(strncmp(dayweek,"Fri",3) == 0) date->jourDeLaSemaine=5;
  else if(strncmp(dayweek,"Sat",3) == 0) date->jourDeLaSemaine=6;
  else if(strncmp(dayweek,"Sun",3) == 0) date->jourDeLaSemaine=7;

  date->jour = day;
  
  if(strncmp(month,"Jan",3) == 0) date->mois=1;
  else if(strncmp(month,"Feb",3) == 0) date->mois=2;
  else if(strncmp(month,"Mar",3) == 0) date->mois=3;
  else if(strncmp(month,"Apr",3) == 0) date->mois=4;
  else if(strncmp(month,"May",3) == 0) date->mois=5;
  else if(strncmp(month,"Jun",3) == 0) date->mois=6;
  else if(strncmp(month,"Jul",3) == 0) date->mois=7;
  else if(strncmp(month,"Aug",3) == 0) date->mois=8;
  else if(strncmp(month,"Sep",3) == 0) date->mois=9;
  else if(strncmp(month,"Oct",3) == 0) date->mois=10;
  else if(strncmp(month,"Nov",3) == 0) date->mois=11;
  else if(strncmp(month,"Dec",3) == 0) date->mois=12;
  date->annee = years%2000;

  /*Serial.print("Sec :");
  Serial.println(date->secondes);
  Serial.print("Min :");
  Serial.println(date->minutes);
  Serial.print("Hou :");
  Serial.println(date->heures);
  Serial.print("Day :");
  Serial.println(date->jour);
  Serial.print("Month :");
  Serial.println(date->mois);
  Serial.print("Year :");
  Serial.println(date->annee);*/
  
  return 1;
}

short send2TP(String value){
  // Send served food during the day
  // Start session
  String cmd = "AT+CIPSTART=\"TCP\",\"";
  cmd += IPthingspeak;
  cmd += "\",80";
  sendDebug(cmd);
  delay(2000);
  if(ESP8266.find("Error")){
    Serial.print("RECEIVED: Error");
    return 0;
  }
  cmd = GET;
  cmd += value;
  cmd += "\r\n";
  ESP8266.print("AT+CIPSEND=");
  ESP8266.println(cmd.length());
  if(ESP8266.find(">")){
    Serial.print(">");
    Serial.print(cmd);
    ESP8266.print(cmd);
  }
  else{
    sendDebug("AT+CIPCLOSE");
    return 0;
  }
  
  if(ESP8266.find("OK")){
    Serial.println("RECEIVED: OK");
  }
  else{
    Serial.println("RECEIVED: Error");
    return 0;
  }

  // Close session
  sendDebug("AT+CIPCLOSE");
  return 1;
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
  lcd.setCursor(3,2);
  lcd.print("Time Left: ");
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
