/*
    This sketch establishes a WiFi connection..
    Recevives distance from us-sensor and send it to mqtt-server
*/

#define VERSION "1.1.3"
#include <SPI.h>
#include <MQTT.h>
#include <time.h>
#include "soc/soc.h"
#include <Preferences.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <stdarg.h>
#include <nvs_flash.h>

#ifdef ARDUINO_LOLIN_C3_MINI
  #include <soc/rtc_cntl_reg.h>
  #define ECHO_PIN 10
  #define TRIG_PIN 9
  #define RESET_PIN 11
  #define BUTTON_PIN_BITMASK 0x4
#elif ARDUINO_NOLOGO_ESP32C3_SUPER_MINI
  #include <soc/rtc_cntl_reg.h>
  #define ECHO_PIN 10
  #define TRIG_PIN 9
  #define RESET_PIN 11
  #define BUTTON_PIN_BITMASK 0x4
#elif ARDUINO_MAKERGO_ESP32C3_SUPERMINI
  #include <soc/rtc_cntl_reg.h>
  #define ECHO_PIN 10
  #define TRIG_PIN 9
  #define RESET_PIN 11
  #define BUTTON_PIN_BITMASK 0x4
#elif ARDUINO_ESP32S3_DEV
  #include <soc/rtc_cntl_reg.h>
  #define ECHO_PIN 47
  #define TRIG_PIN 48
  #define RESET_PIN 49
  #define BUTTON_PIN_BITMASK 0x4
#elif ARDUINO_ESP32_WROOM_DA  // ACHTUNG - Bei dieser Firmware funktioniert nvs Ram nicht richtig!
  #include <soc/rtc_cntl_reg.h>
  #define ECHO_PIN 22
  #define TRIG_PIN 23
  #define LED_BUILTIN 2
  #define RESET_PIN 18
  #define BUTTON_PIN_BITMASK 0x4
#elif ARDUINO_LOLIN32
  #include <soc/rtc_cntl_reg.h>
  #define ECHO_PIN 22
  #define TRIG_PIN 23
  #define LED_BUILTIN 2
  #define RESET_PIN 18
  #define BUTTON_PIN_BITMASK 0x4
#endif


#define uS_TO_S_FACTOR 1000000ULL   // Conversion factor for micro seconds to seconds

#define LogHostNamePre "ESP32"
#define LogTagName "Wasserstandsmelder"
#define SIZEOFLOGBUFFER 1024
#define SERIALSPEED 115200
#define NVSKEYNAME "MY_WS"

MQTTClient mqttclient;
WiFiClient mqttwificlient;
Preferences preferences;
WiFiManager wm;

WiFiManagerParameter custom_mqtt_server("server", "mqtt server name", "", 40);
WiFiManagerParameter custom_mqtt_port("port", "mqtt server port ", "", 5);
WiFiManagerParameter custom_mqtt_user("username", "mqtt server username", "", 40);
WiFiManagerParameter custom_mqtt_pass("password", "mqtt server password", "", 40);
WiFiManagerParameter custom_mqtt_topic("topic", "mqtt server topic", "", 40);
WiFiManagerParameter custom_mqtt_sleeptime("sleeptime", "sleeptime in sec", "", 10);
WiFiManagerParameter custom_dist_min("dmin", "min. distance in cm", "", 5);
WiFiManagerParameter custom_dist_max("dmax", "max. distance in cm", "", 5);

char mqtt_server[40];
int mqtt_port=1883;
char mqtt_user[40];
char mqtt_pass[40];
char mqtt_topic[40];
int mqtt_sleeptime=1800;
int distance_min = 65;
int distance_max = 185;


bool serialInitialised = false;
bool wifiInitialised = false;

const char mqtt_topic_abstand[] = "Wasserstand";
const char mqtt_topic_fuellstand[] = "Fuellstand";
const char mqtt_topic_wifistatus[] = "WiFiStatus";
const char mqtt_topic_localIP[] = "localIP";
const char mqtt_topic_letzte_messung[] = "Letzte_Messung";
const char mqtt_topic_sleeptime[] = "Schlafsekunden";
const char mqtt_topic_jsondata[] = "JSONData";

const int max_messung = 3;

const char* timezone = "CET-1CEST,M3.5.0,M10.5.0/3";
unsigned long lastNTPUpdate = 0; // Timestamp for the last NTP sync
const unsigned long ntpSyncInterval = 30 * 60 * 1000; // Sync every 30 minutes (in ms)

JsonDocument myJSONdoc;

char LogHostName[64];
#include <SimpleSyslog.h>

bool syncTime() {
  int wait_count = 0;
  Serial.println("Synchronizing time with NTP server...");
  configTime(0, 0, "europe.pool.ntp.org", "pool.ntp.org", "time.nist.gov"); // UTC offset set to 0
  time_t now = time(nullptr);
  while (now < 24 * 3600) { // Wait until time is valid
    if (100 < wait_count)
    {
      wait_count = -1;
      break;
    }
    delay(100);
    now = time(nullptr);
    wait_count++;
  }
  // Set timezone
  setenv("TZ", timezone, 1);
  tzset();

  if (wait_count > 0)
  {
    Serial.println(" -> Time synchronized!");    

    lastNTPUpdate = millis(); // Record the time of the last sync
    //Print the time to console
    struct tm timeinfo;
    getLocalTime(&timeinfo);
    char buffer[30]="";
    sprintf(buffer,"Aktuelle Zeit ist: ");
    Serial.print(buffer);
    sprintf(buffer,"%d-%02d-%02d, %02d:%02d:%02d",(timeinfo.tm_year)+1900,( timeinfo.tm_mon)+1, timeinfo.tm_mday, timeinfo.tm_hour , timeinfo.tm_min, timeinfo.tm_sec);
    Serial.println(buffer);
    return true;
  } else {
    Serial.println(" -> Time Out synchronizing time!");
    lastNTPUpdate = 0;
    return false;
  }
  return true;
}

void signal_IamAlive(int count){
  for (int n = 1; n<=count; n++)
  {
    digitalWrite(LED_BUILTIN, LOW);
    delay(500);
    digitalWrite(LED_BUILTIN, HIGH);
    delay(1500);
    digitalWrite(LED_BUILTIN, LOW);
  }
  delay(250);
  digitalWrite(LED_BUILTIN, HIGH);
  delay(250);
  digitalWrite(LED_BUILTIN, LOW);
}

void signal_error(int count){
  for (int n = 0; n<=count; n++)
  {
    digitalWrite(LED_BUILTIN, LOW);
    delay(500);
    digitalWrite(LED_BUILTIN, HIGH);
    delay(500);
    digitalWrite(LED_BUILTIN, LOW);
  }
}

void saveParamCallback(){
  mylogging(PRI_INFO, "[CALLBACK] saveParamCallback fired");

  strcpy(mqtt_server, custom_mqtt_server.getValue());
  mqtt_port = atoi(custom_mqtt_port.getValue());
  strcpy(mqtt_user, custom_mqtt_user.getValue());
  strcpy(mqtt_pass, custom_mqtt_pass.getValue());
  strcpy(mqtt_topic, custom_mqtt_topic.getValue());
  mqtt_sleeptime = atoi(custom_mqtt_sleeptime.getValue());
  distance_min = atoi(custom_dist_min.getValue());
  distance_max = atoi(custom_dist_max.getValue());

  mylogging(PRI_INFO, "---- Data read from config portal----- ");
  mylogging(PRI_INFO, "mqtt_server: %s", mqtt_server);
  mylogging(PRI_INFO, "mqtt_port: %d", mqtt_port);
  mylogging(PRI_INFO, "mqtt_user: %s", mqtt_user);
  mylogging(PRI_INFO, "mqtt_pass: %s", mqtt_pass);
  mylogging(PRI_INFO, "mqtt_topic: %s", mqtt_topic);
  mylogging(PRI_INFO, "mqtt_sleeptime: %d", mqtt_sleeptime);
  mylogging(PRI_INFO,"preferences minimal distance: %d", distance_min);
  mylogging(PRI_INFO,"preferences maximal distance: %d", distance_max);

  preferences.begin(NVSKEYNAME, false); //read and write

  preferences.putString("server", mqtt_server);
  preferences.putInt("port", mqtt_port);
  preferences.putString("username", mqtt_user);
  preferences.putString("password", mqtt_pass);
  preferences.putString("topic", mqtt_topic);
  preferences.putInt("sleept", mqtt_sleeptime);
  preferences.putInt("dmin", distance_min);
  preferences.putInt("dmax", distance_max);

  mylogging(PRI_INFO, "---- Preferences saved----- ");
  preferences.end();  

}

void mylogging(const uint8_t log_level, const char* format, ...){
  char logbuffer[SIZEOFLOGBUFFER];

  if (!serialInitialised ){
    Serial.begin(SERIALSPEED);
    delay(500); //(500millis)
    serialInitialised = true;
    Serial.println("");
  }

  va_list args;
  va_start(args, format);
  vsnprintf(logbuffer, SIZEOFLOGBUFFER, format, args);
  va_end(args);

  Serial.println(logbuffer);
  
  if (wifiInitialised){
    SimpleSyslog syslog(LogHostName, LogTagName, mqtt_server);
    syslog.printf(FAC_LOCAL7, log_level, logbuffer);
  }
}

void setup() {
  bool res;
  pinMode(LED_BUILTIN, OUTPUT);

  signal_IamAlive(1);

  WiFi.mode(WIFI_STA); // explicitly set mode, esp defaults to STA+AP  

  mylogging(PRI_INFO, "Serial intialized");
 
  pinMode(RESET_PIN, INPUT_PULLDOWN);
  digitalWrite(RESET_PIN, LOW);

  digitalWrite(LED_BUILTIN, LOW);

    #ifdef ARDUINO_LOLIN_C3_MINI
      mylogging(PRI_INFO, "Board: Lolin C3 Mini");
    #elif ARDUINO_NOLOGO_ESP32C3_SUPER_MINI
      mylogging(PRI_INFO, "Board: Nologo ESPC3 Super Mini");
    #elif ARDUINO_MAKERGO_ESP32C3_SUPERMINI
      mylogging(PRI_INFO, "Board: MakerGO ESP32 C3 Supermini");
    #elif ARDUINO_ESP32S3_DEV
      mylogging(PRI_INFO, "Board: ESP32 S3");
    #elif ARDUINO_ESP32_WROOM_DA
      mylogging(PRI_INFO, "Board: ESP32 Wroom DA -  nvs not working");
    #elif ARDUINO_LOLIN32
      mylogging(PRI_INFO, "Board: Lolin32");  
    #else
      mylogging(PRI_INFO, "Unknown Board");
    #endif

  //loading config
  preferences.begin(NVSKEYNAME, true); //Readonly

  strcpy(mqtt_server, preferences.getString("server", "192.168.1.10").c_str());
  mylogging(PRI_INFO, "preferences mqtt_server: %s", mqtt_server);
  
  mqtt_port = preferences.getInt("port", 1883);
  mylogging(PRI_INFO,"preferences mqtt_port: %d", mqtt_port);
  
  strcpy(mqtt_user, preferences.getString("username", "username").c_str());
  mylogging(PRI_INFO,"preferences mqtt_user: %s", mqtt_user);
  
  strcpy(mqtt_pass, preferences.getString("password", "").c_str());
  mylogging(PRI_INFO,"preferences mqtt_pass: %s",  mqtt_pass);
  
  strcpy(mqtt_topic, preferences.getString("topic", "/Zisterne").c_str());
  mylogging(PRI_INFO,"preferences mqtt_topic: %s", mqtt_topic);

  mqtt_sleeptime = preferences.getInt("sleept", 1800);
  mylogging(PRI_INFO,"preferences mqtt_sleeptime: %d", mqtt_sleeptime);


  distance_min = preferences.getInt("dmin", 65);
  mylogging(PRI_INFO,"preferences minimal distance: %d", distance_min);

  distance_max = preferences.getInt("dmax", 185);
  mylogging(PRI_INFO,"preferences maximal distancd: %d", distance_max);
  preferences.end();

  signal_IamAlive(2);

  char char_port[5];
  char char_dmin[5];
  char char_dmax[5];
  char char_sleeptime[10];
  sprintf(char_port, "%d", mqtt_port);
  sprintf(char_dmin, "%d", distance_min);
  sprintf(char_dmax, "%d", distance_max);
  sprintf(char_sleeptime, "%d", mqtt_sleeptime);

  custom_mqtt_server.setValue(mqtt_server,40);
  custom_mqtt_port.setValue(char_port, 5);
  custom_mqtt_user.setValue(mqtt_user, 40);
  custom_mqtt_pass.setValue(mqtt_pass, 40);
  custom_mqtt_topic.setValue(mqtt_topic, 40);
  custom_mqtt_sleeptime.setValue(char_sleeptime, 10);
  custom_dist_min.setValue(char_dmin, 5);
  custom_dist_max.setValue(char_dmax, 5);

  wm.addParameter(&custom_mqtt_server);
  wm.addParameter(&custom_mqtt_port);
  wm.addParameter(&custom_mqtt_user);
  wm.addParameter(&custom_mqtt_pass);
  wm.addParameter(&custom_mqtt_topic);
  wm.addParameter(&custom_mqtt_sleeptime);
  wm.addParameter(&custom_dist_min);
  wm.addParameter(&custom_dist_max);

  wm.setConfigPortalTimeout(180); //Wait max 3 min. for a connection to config portal
  wm.setConnectTimeout(180);
  wm.setSaveParamsCallback(saveParamCallback);
  wm.setDebugOutput(false);

  delay(100);

  mylogging(PRI_INFO, "---- Calling WiFiManager ----");
  delay(100);
 
  if (HIGH == digitalRead(RESET_PIN)){
  // is configuration portal requested?
    mylogging(PRI_INFO, "Shall start config portal");
    if (!wm.startConfigPortal("AutoConnectAP","geheim")) {
      mylogging(PRI_INFO, "failed to start config portal, hit timeout");
      delay(500);
      ESP.restart();
    }
    mylogging(PRI_INFO, "Configportal started.");
  } else{
    mylogging(PRI_INFO, "Autoconnecting to wifi");
    if(!wm.autoConnect("AutoConnectAP","geheim")) {
      mylogging(PRI_INFO, "failed to connect and hit timeout");
      delay(500);
      ESP.restart();      
    } 
  }

  delay(500);
  wifiInitialised = true;

  signal_IamAlive(1);
  //Ab hier Logging im Syslog des iot

  byte mac[6];
  WiFi.macAddress(mac);
  snprintf(LogHostName, 64, "%s-%X-%X-%X-%X-%X-%X", LogHostNamePre, mac[5], mac[4], mac[3], mac[2], mac[1], mac[0]);

  mylogging(PRI_INFO,"Connected to WiFi: %s", WiFi.SSID().c_str());
  mylogging(PRI_INFO,"Empfangsstärke: %d", WiFi.RSSI());

  myJSONdoc["Version"]=VERSION;
  myJSONdoc["ssid"]=WiFi.SSID();
  delay(500);

  mylogging(PRI_INFO, "");

  char buffer[32];
  IpAddress2String(WiFi.localIP()).toCharArray(buffer, 32);
  mylogging(PRI_INFO,"WiFi connected, IP address: %s", buffer);

  myJSONdoc["ipaddress"]=IpAddress2String(WiFi.localIP());

  if (syncTime()){
    mylogging(PRI_INFO, "Time synced");
  } else {
    mylogging(PRI_INFO, "Error syncing time");
  }
  
  mylogging(PRI_INFO, "Setting PinMode");
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  mylogging(PRI_INFO, "Connecting to mqtt-Server...");
  mqttclient.begin(mqtt_server, mqtt_port, mqttwificlient);

  mqttclient.connect(mqtt_topic, mqtt_user,mqtt_pass, false); 
  mylogging(PRI_INFO, "Connected to MQTT with topic: %s", mqtt_topic);

  updateDistance();
  mylogging(PRI_INFO, "Distance updated and sent to mqtt-Server");

  mqttclient.disconnect();

  mylogging(PRI_INFO, "Switching off");
  
  mylogging(PRI_INFO, "Going to sleep now for %d seconds", mqtt_sleeptime );
  delay(500);
  Serial.flush(); 
  digitalWrite(LED_BUILTIN, LOW);
  delay(100);
  digitalWrite(LED_BUILTIN, HIGH);
  delay(100);
  digitalWrite(LED_BUILTIN, LOW);

  // Hibernation-Modus starten
  esp_sleep_enable_timer_wakeup(mqtt_sleeptime * uS_TO_S_FACTOR);
  esp_sleep_enable_ext1_wakeup(BUTTON_PIN_BITMASK, ESP_EXT1_WAKEUP_ANY_HIGH);  // Configure external wake-up PIN 2 has to go to high
  delay(1000);  // Adding a 1 second delay to avoid multiple pressed
  esp_deep_sleep_start();
  
}

String IpAddress2String(const IPAddress& ipAddress) {
    return String(ipAddress[0]) + String(".") +
           String(ipAddress[1]) + String(".") +
           String(ipAddress[2]) + String(".") +
           String(ipAddress[3]);
}


void updateDistance() {
  float duration = 0.0;
  float abstand = 0.0;
  float distance = 0.0;
  float fuellstand = 0.0;
  int anzahl_gueltige_messung = 0;

  mylogging(PRI_INFO, "Starting update of distance");

  myJSONdoc["mqtt_server"]=mqtt_server;
  
  JsonArray myJSONdata = myJSONdoc["Messungen"].to<JsonArray>();
  for (int n=1; n <= max_messung; n++) {
    mylogging(PRI_INFO, "%d.te Messung", n);

    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);

    //measure duration of pulse from ECHO pin
    duration = pulseIn(ECHO_PIN, HIGH);
    mylogging(PRI_INFO, "Gemessene Zeit: %20.24f", duration);

    abstand = duration * 0.01715;

    Serial.print("Distance: ");
    Serial.println(abstand);

    mylogging(PRI_INFO, "Gemessene Entfernung: %4.24f", abstand); // war 4 
    if ( abstand > 0 & abstand < 600) {
      distance = distance + abstand;
      anzahl_gueltige_messung++;
    }
    myJSONdata.add(int(abstand));
    mylogging(PRI_INFO, "Sleeping a little bit...");
    delay(250);

  }

  if (anzahl_gueltige_messung > 0) {
    distance = distance / anzahl_gueltige_messung;
  }
  mylogging(PRI_INFO, "Valid are: %d, Distance in float: %.14f", anzahl_gueltige_messung, distance );

  //HGF distance = 55.0;
  //HGF mylogging(PRI_INFO, "DEBUG, Setting distance to: %.1f", distance );

  myJSONdoc["count_gueltige_Messungen"]=anzahl_gueltige_messung;

  myJSONdoc["calculated_distance"]= (int) distance;
  mylogging(PRI_INFO, "Distance (cm): %d", (int) distance );

  myJSONdoc["min_distance"]= (int) distance_min;
  mylogging(PRI_INFO, "Minimal Distance (cm): %d", (int) distance_min );

  myJSONdoc["max_distance"]= (int) distance_max;
  mylogging(PRI_INFO, "Maximal Distance (cm): %d", (int) distance_max );

  if (!mqttclient.connected()){
    mylogging(PRI_INFO, "mqtt Client ist nicht verbunden");
    signal_error(5);
    return;
  }

  char topic_name[256]="";
  char buffer[32]="";

  mqttclient.loop();

  sprintf(topic_name,"%s/%s", mqtt_topic, mqtt_topic_abstand);
  sprintf(buffer,"%.1f", distance);
  mqttclient.publish(topic_name, buffer);

  if (distance > distance_max){
    mylogging(PRI_INFO, "Setting fuellstand to -1");
    fuellstand = -1.0;
  }
  else{
    mylogging(PRI_INFO, "Calculating Füllstand, distmax=%d, distmin=%d, Formel:(distance_max + distance_min - distance) * 100 / distance_max",distance_max,distance_min);
    fuellstand = (distance_max + distance_min - (int) distance) * 100 / distance_max;    
    mylogging(PRI_INFO, "Fuellstand ist %.4f", fuellstand);
  }
  myJSONdoc["Fuellstand"]=fuellstand;

  sprintf(topic_name,"%s/%s", mqtt_topic, mqtt_topic_fuellstand);
  sprintf(buffer, "%.0f", fuellstand);
  mqttclient.publish(topic_name, buffer);

  sprintf(topic_name,"%s/%s", mqtt_topic, mqtt_topic_wifistatus);
  sprintf(buffer,"%d", WiFi.RSSI());
  mqttclient.publish(topic_name, buffer);
  
  IpAddress2String(WiFi.localIP()).toCharArray(buffer, 32);
  sprintf(topic_name,"%s/%s", mqtt_topic, mqtt_topic_localIP);
  mqttclient.publish(topic_name, buffer);

  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  //gmtime_r(&now, &timeinfo);

  getLocalTime(&timeinfo, 5000);
  sprintf(buffer,"%d-%02d-%02dT%02d:%02d:%02d",(timeinfo.tm_year)+1900,( timeinfo.tm_mon)+1, timeinfo.tm_mday, timeinfo.tm_hour , timeinfo.tm_min, timeinfo.tm_sec);

  sprintf(topic_name,"%s/%s", mqtt_topic, mqtt_topic_letzte_messung);
  mqttclient.publish(topic_name, buffer);

  sprintf(topic_name,"%s/%s", mqtt_topic, mqtt_topic_sleeptime);
  sprintf(buffer, "%.d", mqtt_sleeptime);
  mqttclient.publish(topic_name, buffer);

  sprintf(topic_name,"%s/%s", mqtt_topic, mqtt_topic_jsondata);
  char myJson_Data[512];
  serializeJson(myJSONdoc, myJson_Data);
  mqttclient.publish(topic_name, myJson_Data);
    
}

void loop(){

  delay(200);

  // is configuration portal requested?
  if (HIGH == digitalRead(RESET_PIN)){

    //load config
    preferences.begin(NVSKEYNAME, true); //readonly
    
    strcpy(mqtt_server, preferences.getString("server", "192.168.1.10").c_str());
    mylogging(PRI_INFO, "preferences mqtt_server: %s", mqtt_server);
    
    mqtt_port = preferences.getInt("port", 1883);
    mylogging(PRI_INFO, "preferences mqtt_port: %d", mqtt_port);
    
    strcpy(mqtt_user, preferences.getString("username", "username").c_str());
    mylogging(PRI_INFO, "preferences mqtt_user: %s", mqtt_user);
    
    strcpy(mqtt_pass, preferences.getString("password", "").c_str());
    mylogging(PRI_INFO, "preferences mqtt_pass: %s", mqtt_pass);
    
    strcpy(mqtt_topic, preferences.getString("topic", "/Zisterne").c_str());
    mylogging(PRI_INFO, "preferences mqtt_topic: %s", mqtt_topic);

    mqtt_sleeptime = preferences.getInt("sleept", 1800);
    mylogging(PRI_INFO, "preferences mqtt_sleeptime: %d", mqtt_sleeptime);

    distance_min = preferences.getInt("dmin", 65);
    mylogging(PRI_INFO,"preferences minimal distance: %d", distance_min);

    distance_max = preferences.getInt("dmax", 185);
    mylogging(PRI_INFO,"preferences maximal distancd: %d", distance_max);

    preferences.end();

    char char_port[5];
    sprintf(char_port, "%d", mqtt_port);

    wm.setConfigPortalTimeout(120);
    wm.setDebugOutput(false);

    delay(100);

    mylogging(PRI_INFO, "Starting config portal");

    if (!wm.startConfigPortal("AutoConnectAP","geheim")) {
      mylogging(PRI_INFO, "failed to start config portal and hit timeout");
      delay(500);
      //reset and try again, or maybe put it to deep sleep
      ESP.restart();
    } else {
      mylogging(PRI_INFO, "Configportal started. :)");
    }
      
  }

}
