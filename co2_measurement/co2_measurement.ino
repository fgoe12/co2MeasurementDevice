/*
author: Felix Göhner, Tim Göhner
version: 3.2
date: 14.10.2020
license: GNU

schematics: D1 => piezo buzzer alarm sensor
			D6 => mh-z19b co2-sensor pwm signal
			D7 => LED yellow
			D8 => LED red
*/


#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <FS.h>
#include <EEPROM.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <time.h>
#include <ArduinoJson.h>


//current datetime
struct TDatetime {
	unsigned long currentEpochTime;
	int currentDay;
	int currentMonth;
	int currentYear;
	int currentHour;
	int currentMinute;
	int currentSecond;
	String currentDatetimeStr;
};

//scanned wlan
struct TWlanScanned {
	unsigned int length = 0;
	String ssid[100];
	String rssi[100];
	boolean open[100];
};
//save last wlan-scan
TWlanScanned wlanList;


//eeprom
unsigned int eeprom_size = 512;

//home-wlan configuration
const unsigned int ssidHomeWlanSize = 256;
char ssidHomeWlan[ssidHomeWlanSize];
const unsigned int passwordHomeWlanSize = 256;
char passwordHomeWlan[passwordHomeWlanSize];

//standalone-wlan configuration
char* ssidStandaloneWlan = "CO2-Measurement-";
const char* passwordStandaloneWlan = "";
IPAddress local_ip(10,10,10,1);
IPAddress gateway(10,10,10,1);
IPAddress subnet(255,255,255,0);

//wlan mode
enum wlanMode {HOMEWLAN,STANDALONEWLAN};
enum wlanMode wlanMode;

//webserver
AsyncWebServer server(80);

//esp8266-pin connected to co2-sensor mh-z19b pwm-pin
const int pinCo2Sensor = 12;
//co2SensorRange of mh-z19b (0-5000)
const int co2SensorRange = 5000;

//measured co2-value
int co2PpmLastMeasurement;
TDatetime timestampLastMeasurement;
String timestampLastMeasurementStr;
int co2Ppm4LastMeasurements[4] = {413,413,413,413};
int co2PpmMean;

//alarms set
unsigned long alarmsSetTime[3] = {0,0,0};
boolean co2ok = true;
String airQuality = "green";

//alarm led
const int pinLedRed = 15;
const int pinLedYellow = 13;

//timer
const unsigned long duration = 30000;	//time in ms
unsigned long previousMillis = 0;
unsigned long currentMillis = duration;

//time server
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org");

//time since first measurement for standalone wlan without global time
unsigned long millisSinceFirstMeasurement = 0;
unsigned long millisStartUp = millis();

//esp8266 modem sleeping
boolean modemSleeping = false;
unsigned long millisSinceModemSleep = 0;
unsigned long millisModemSleepStartUp = 0;

//flags
boolean resetFlag = false;
boolean wlanScanFlag = false;


void setup() {

	//co2-sensor input
	pinMode(pinCo2Sensor, INPUT);

	//alarm leds
	pinMode(pinLedRed, OUTPUT);
	pinMode(pinLedYellow, OUTPUT);

	//EEPROM
	EEPROM.begin(eeprom_size);

	//SPIFFS
	SPIFFS.begin();

	//debugging ouput
	Serial.begin(9600);

	//try to connect to home-wlan
	WiFi.mode(WIFI_STA);
	wlanMode = HOMEWLAN;
	String ssidHomeWlanStr = readStringFromEeprom(0);	//start address: 0
	ssidHomeWlanStr.toCharArray(ssidHomeWlan,ssidHomeWlanSize);
	String passwordHomeWlanStr = readStringFromEeprom(256);	//start address: 256
	passwordHomeWlanStr.toCharArray(passwordHomeWlan,passwordHomeWlanSize);
	Serial.print("connecting to ");
	Serial.print(ssidHomeWlan);

	//for testing standalone wlan
	//const char* ssidHomeWlan = "test";
	//const char* passwordHomeWlan = "test";

	WiFi.begin(ssidHomeWlan,passwordHomeWlan);

	unsigned long homeWlanConnectionTimeMs = 0;
	unsigned long homeWlanConnectionTimeStartMs = millis();
	unsigned long homeWlanConnectionTimePreviousMs = 0;
	while ((WiFi.status() != WL_CONNECTED) && (homeWlanConnectionTimeMs < 10000)) {
		delay(10);
		homeWlanConnectionTimeMs = millis() - homeWlanConnectionTimeStartMs;
		if(homeWlanConnectionTimeMs - homeWlanConnectionTimePreviousMs >= 500) {
			homeWlanConnectionTimePreviousMs = homeWlanConnectionTimeMs;
			Serial.print(".");
		}
		if(homeWlanConnectionTimeMs >= 10000) {
			Serial.println("\nno home-wlan found.");
		}
	}
	//check if home-wlan connected
	if(WiFi.status() == WL_CONNECTED) {
		Serial.println("\nWLAN connected.");
		//print the IP-address
		Serial.print("use this URL to connect: ");
		Serial.print("http://");
		Serial.print(WiFi.localIP());
		Serial.println("/");

		//ntp-time
		timeClient.begin();
		timeClient.setTimeOffset(3600);
	} else {
		//create own standalone wlan
		WiFi.mode(WIFI_AP);
		wlanMode = STANDALONEWLAN;
		unsigned int chipId = ESP.getChipId();
		char chipIdStr[32];
		sprintf(chipIdStr,"%d",chipId);
		strcat(ssidStandaloneWlan,chipIdStr);
		Serial.print("create standalone wlan: ");
		Serial.println(ssidStandaloneWlan);
		WiFi.softAP(ssidStandaloneWlan, passwordStandaloneWlan,8);
		WiFi.softAPConfig(local_ip, gateway, subnet);
	}

	//webserver
	server.onNotFound(handleNotFound);
	server.on("/", HTTP_GET, handleDataPage);
	server.on("/data.html", handleDataPage);
	server.on("/data.xml", handleXmlPage);
	server.on("/settings.html", handleSettingsPage);
	server.on("/new-wlan.html", handleNewWlanPage);
	server.on("/new-wlan", HTTP_GET, handleNewWlanAction);
	server.on("/wlan-scan", HTTP_GET, handleWlanScanAction);

	server.begin();
	Serial.println("server started.");

	//first measurement
	co2PpmLastMeasurement = readCo2SensorPwm();
	timestampLastMeasurement = getDatetime();
	timestampLastMeasurementStr = timestampLastMeasurement.currentDatetimeStr;
	millisSinceFirstMeasurement = millis() - millisStartUp;

	sortCo2PpmMeasurements(co2PpmLastMeasurement);
	co2PpmMean = calculateCo2Mean();

	Serial.println("setup finished.");

}


void loop() {

	currentMillis = millis();
	if(currentMillis - previousMillis >= duration) {
		previousMillis = currentMillis;
		//check modem sleep
		if(wlanMode == HOMEWLAN) {
			modemSleep();
		}
		//start measurement
		co2PpmLastMeasurement = readCo2SensorPwm();
		timestampLastMeasurement = getDatetime();
		timestampLastMeasurementStr = timestampLastMeasurement.currentDatetimeStr;
		millisSinceFirstMeasurement = millis() - millisStartUp;
		Serial.print("next measurement in ");
		Serial.print(duration/1000);
		Serial.print(" sec.\n");
		//calculate mean
		sortCo2PpmMeasurements(co2PpmLastMeasurement);
		co2PpmMean = calculateCo2Mean();
		setCo2Alarm(co2PpmMean);
	}
	if(resetFlag == true) {
		Serial.println("software reset.");
		resetFlag = false;
		delay(3000);
		ESP.restart();
	}
	if(wlanScanFlag == true) {
		Serial.println("wlan-scan...");
		delay(500);
		scanWlan();
		wlanScanFlag = false;
		delay(500);
	}

}


//control sleep-time of wlan
void modemSleep() {

	TDatetime t = getDatetime();
	if(modemSleeping == false) {
		if((t.currentHour == 23) && (t.currentMinute < 5)) {
			Serial.println("modem sleep started.");
			modemSleeping = true;
			millisModemSleepStartUp = millis();
			WiFi.forceSleepBegin();
		}
	}
	if(modemSleeping == true) {
		millisSinceModemSleep = millis() - millisModemSleepStartUp;
		if(millisSinceModemSleep > 25200000) {	//25200000 = 7h
			Serial.println("modem sleep stopped.");
			modemSleeping = false;
			millisSinceModemSleep = 0;
			WiFi.forceSleepWake();
		}
	}

}


//write string to EEPROM
void writeStringToEeprom(String str, unsigned int address, unsigned int size) {

	if(str.length() > size) {
		Serial.println("EEPROM error: string-size bigger then flash-storage.");
	} else {
		Serial.println("EEPROM: " + str);
		EEPROM.write(address,str.length());
		for(unsigned int i = 0; i < str.length(); i++) {
			char c = str[i];
			EEPROM.write(address+i+1,c);
			EEPROM.commit();
		}
		Serial.println("EEPROM: String saved successfully.");
	}

}


//read string from EEPROM
String readStringFromEeprom(unsigned int adress) {
	
	String str;
	unsigned int strLength = EEPROM.read(adress);
	for(unsigned int i = 0; i < strLength; i++) {
		str += char(EEPROM.read(adress+i+1));
	}
	return str;

}


void setCo2Alarm(int value) {
	
	TDatetime datetime;
	datetime = getDatetime();

	if(value <= 800) {
		Serial.println("air quality: green");
		airQuality = "green";
		digitalWrite(pinLedRed,LOW);
		digitalWrite(pinLedYellow,LOW);
		if((datetime.currentHour >= 10 || datetime.currentHour <= 22 ) && (co2ok == false)) {
			co2Alarm(0);
			co2ok = true;
		}
	} else if(value >= 1000 && value < 2000) {
		Serial.println("air quality: yellow");
		airQuality = "yellow";
		digitalWrite(pinLedRed,LOW);
		digitalWrite(pinLedYellow,HIGH);
		co2ok = false;
		if((datetime.currentEpochTime - alarmsSetTime[0] > 3600) && (datetime.currentHour >= 10 || datetime.currentHour <= 22 )) {
			co2Alarm(1);
			alarmsSetTime[0] = datetime.currentEpochTime;
		}
	} else if(value >= 2000) {
		Serial.println("air quality: red");
		airQuality = "red";
		digitalWrite(pinLedRed,HIGH);
		digitalWrite(pinLedYellow,LOW);
		co2ok = false;
		if((datetime.currentEpochTime - alarmsSetTime[1] > 3600) && (datetime.currentHour >= 10 || datetime.currentHour <= 22 )) {
			co2Alarm(2);
			alarmsSetTime[1] = datetime.currentEpochTime;
		}
	}

}


//tone-generator: alarm0: <=800 ppm, alarm2: 1000-2000, alarm3: >=2000, alarm4: 
void co2Alarm(int alarm) {
	switch(alarm) {
		case 0:
			for(unsigned int i=0;i<10;i++) {
				tone(D1,1);
				delay(100);
			}
			noTone(D1);
			break;
		case 1:
			tone(D1,1);
			delay(200);
			noTone(D1);
			delay(800);
			tone(D1,1);
			delay(200);
			noTone(D1);
			break;
		case 2:
			tone(D1,1);
			delay(200);
			noTone(D1);
			delay(800);
			tone(D1,1);
			delay(200);
			noTone(D1);
			delay(100);
			tone(D1,1);
			delay(200);
			noTone(D1);
			break;
		case 3:
			tone(D1,1);
			delay(200);
			noTone(D1);
			delay(800);
			tone(D1,1);
			delay(200);
			noTone(D1);
			delay(100);
			tone(D1,1);
			delay(200);
			noTone(D1);
			delay(100);
			tone(D1,1);
			delay(200);
			noTone(D1);
			break;
	}
}


//get time and date from ntp-server
TDatetime getDatetime() {

	TDatetime datetime;

	timeClient.update();
	unsigned long epochTime = timeClient.getEpochTime();
	datetime.currentEpochTime = epochTime;
	//String formattedTime = timeClient.getFormattedTime();

    //Get a time structure
	struct tm *ptm = gmtime ((time_t *)&epochTime);
	datetime.currentDay = ptm->tm_mday;
	datetime.currentMonth = ptm->tm_mon+1;
	datetime.currentYear = ptm->tm_year+1900;
	datetime.currentHour = timeClient.getHours();
	datetime.currentMinute = timeClient.getMinutes();
	datetime.currentSecond = timeClient.getSeconds();

	boolean summertime = summertime_EU(datetime.currentYear,datetime.currentMonth,datetime.currentDay,datetime.currentHour,1);
	if(summertime) {
		datetime.currentHour = datetime.currentHour + 1;
	}

	char buf[50];
	sprintf(buf, "%02d-%02d-%02d %02d:%02d:%02d", datetime.currentYear, datetime.currentMonth, datetime.currentDay, datetime.currentHour, datetime.currentMinute, datetime.currentSecond);

	datetime.currentDatetimeStr = buf;
	return datetime;

}


// European Daylight Savings Time calculation by "jurs" for German Arduino Forum
// input parameters: "normal time" for year, month, day, hour and tzHours (0=UTC, 1=MEZ)
// return value: returns true during Daylight Saving Time, false otherwise
boolean summertime_EU(int year, byte month, byte day, byte hour, byte tzHours) {

	if (month<3 || month>10) return false; // no summertime in Jan, Feb, Nov, Dez
	if (month>3 && month<10) return true; // summertime in Apr, Mai, Jun, Jul, Aug, Sep
	if (month==3 && (hour + 24 * day)>=(1 + tzHours + 24*(31 - (5 * year /4 + 4) % 7)) || month==10 && (hour + 24 * day)<(1 + tzHours + 24*(31 - (5 * year /4 + 1) % 7)))
		return true;
	else
		return false;

}


//measurement co2-value over PWM (pulse width modulation) from mh-z19b
int readCo2SensorPwm() {

	unsigned long durationPwmPulse;
	int co2ValuePpm = 0;
	float durationPwmPulsePercentage;

	Serial.println("measurement started...");

	do {
		//duration of pwm-pulse from co2-sensor in ms (timeout 1004000µs)
		durationPwmPulse = pulseIn(pinCo2Sensor, HIGH, 1004000) / 1000;
		//pwm-pulse in percentage (%)
		float durationPwmPulsePercentage = durationPwmPulse / 1004.0;
		//calculate co2-ppm-value
		co2ValuePpm = co2SensorRange * durationPwmPulsePercentage;

	} while (durationPwmPulse == 0);

	Serial.print("co2-value: ");
	Serial.print(co2ValuePpm);
	Serial.print(" ppm\n");

	return co2ValuePpm; 

}


//sort old co2-values
void sortCo2PpmMeasurements(int newCo2Ppm) {

	for(unsigned int i = 3; i > 0; i--) {
		co2Ppm4LastMeasurements[i] = co2Ppm4LastMeasurements[i-1];
	}
	co2Ppm4LastMeasurements[0] = newCo2Ppm;

}


//calculate average/mean co2-value
int calculateCo2Mean() {

	int sum = 0;
	for(unsigned int i = 0; i < 4; i++) {
		sum = sum + co2Ppm4LastMeasurements[i];
	}
	int mean = sum / 4;
	Serial.print("co2-value mean: ");
	Serial.print(mean);
	Serial.println(" ppm");
	return mean;

}


//scan after available wlan-networks
void scanWlan() {

	// WiFi.scanNetworks will return the number of networks found
	int n = WiFi.scanNetworks();
	wlanList.length = n;
	Serial.println("wlan-scan done.");
	if (n == 0) {
		Serial.println("no networks found.");
	} else {
		Serial.print(n);
		Serial.println(" networks found");
		for (int i = 0; i < n; i++) {
			wlanList.ssid[i] = WiFi.SSID(i);
			wlanList.rssi[i] = WiFi.RSSI(i);
			if(WiFi.encryptionType(i) == ENC_TYPE_NONE) {
				wlanList.open[i] = true;
			} else {
				wlanList.open[i] = false;
			}
			// Print SSID and RSSI for each network found
/* 			Serial.print(i + 1);
			Serial.print(": ");
			Serial.print(WiFi.SSID(i));
			Serial.print(" (");
			Serial.print(WiFi.RSSI(i));
			Serial.print(")");
			Serial.println((WiFi.encryptionType(i) == ENC_TYPE_NONE)?" ":"*"); */
			delay(10);
		}
	}

}


//handle WebpageNotFound-event
void handleNotFound(AsyncWebServerRequest *request) {

	Serial.println("server: handleNotFound()");
    //request->send(404, "text/plain", "Not found");
	request->send(SPIFFS, "/not-found.html");

}


//handle dataPage-event
void handleDataPage(AsyncWebServerRequest *request) {

	Serial.println("server: handleDataPage()");
	request->send(SPIFFS, "/data.html", String(), false, replacePlaceholder);

}


//handle xmlPage-event
void handleXmlPage(AsyncWebServerRequest *request) {

	Serial.println("server: handleXmlPage()");
	request->send(SPIFFS, "/data.xml", String(), false, replacePlaceholder);

}


//handle settingsPage-event
void handleSettingsPage(AsyncWebServerRequest *request) {

	Serial.println("server: handleSettingsPage()");
	request->send(SPIFFS, "/settings.html");
	wlanScanFlag = true;

}


//handle newWlanPage-event
void handleNewWlanPage(AsyncWebServerRequest *request) {

	Serial.println("server: handleNewWlanPage()");
	request->send(SPIFFS, "/new-wlan.html");
	resetFlag = true;

}


//handle new-wlan-action
void handleNewWlanAction(AsyncWebServerRequest *request) {

	Serial.println("server: handleNewWlanAction()");

	String inputSsid;
    String inputPassword;

    //GET ssid: /new-wlan?inputSsid=
    if(request->hasParam("inputSsid")) {
		inputSsid = request->getParam("inputSsid")->value();
    }
    //GET password: /new-wlan?inputPassword=
    if(request->hasParam("inputPassword")) {
		inputPassword = request->getParam("inputPassword")->value();
    }
    //Serial.println(inputSsid + " " + inputPassword);
	writeStringToEeprom(inputSsid,0,256);
	writeStringToEeprom(inputPassword,256,256);
	
	delay(2000);

	request->redirect("/new-wlan.html");

}


//handle wlan-scan-action
void handleWlanScanAction(AsyncWebServerRequest *request) {

	Serial.println("server: handleWlanScanAction()");

	/* wlanList.length = 2;
	wlanList.ssid[0] = "test0";
	wlanList.rssi[0] = "-100";
	wlanList.open[0] = false;
	wlanList.ssid[1] = "test1";
	wlanList.rssi[1] = "-50";
	wlanList.open[1] = true; */
	
	//build json
	StaticJsonBuffer<1000> jsonBuffer;
	JsonObject& doc = jsonBuffer.createObject();
	//doc["ssid"] = wlanList.ssid[0];
	//doc["rssi"] = wlanList.rssi[0];
	//doc["open"] = wlanList.open[0];
	JsonArray& ssidJson = doc.createNestedArray("ssid");
	JsonArray& rssiJson = doc.createNestedArray("rssi");
	JsonArray& openJson = doc.createNestedArray("open");
	for(unsigned int i = 0; i < wlanList.length; i++) {
		ssidJson.add(wlanList.ssid[i]);
		rssiJson.add(wlanList.rssi[i]);
		openJson.add(wlanList.open[i]);
	}
	doc.printTo(Serial);
	AsyncResponseStream *response = request->beginResponseStream("application/json");
	doc.printTo(*response);
	request->send(response);
	//request->send(200,"text/plain", String(doc));
	//request->send(200,"text/plain", String(wlanList.ssid[0]));

}


// Replaces web-placeholder with values
String replacePlaceholder(const String& var) {

	//values for both wlan modes
	if(var == "CO2PPMLASTMEASUREMENT") {
		return String(co2PpmLastMeasurement);
	} else if(var == "CO2PPMMEAN") {
		return String(co2PpmMean);
	} else if(var == "AIRQUALITY") {
		return String(airQuality);
	} else if(var == "IPADDRESS") {
		IPAddress ip = WiFi.localIP();
		String ipStr = String(ip[0]) + "." + String(ip[1]) + "." + String(ip[2]) + "." + String(ip[3]);
		return ipStr;
	}

	//values for HOMEWLAN mode
	if(wlanMode == HOMEWLAN) {
		if(var == "TIMEHEADING") {
			return "timestamp last measurement";
		} else if(var == "TIMEHEADINGXML") {
			return "TIMESTAMP";
		} else if(var == "TIMESTR") {
			return timestampLastMeasurementStr;
		}
	}	

	//values for STANDALONEWLAN mode
	if(wlanMode == STANDALONEWLAN) {
		if(var == "TIMEHEADING") {
			return "time since first measurement / sec";
		} else if(var == "TIMEHEADINGXML") {
			return "TIMESINCEFIRSTMEASUREMENT";
		} else if(var == "TIMESTR") {
			return String((double(millisSinceFirstMeasurement)/1000.0));
		}
	}

}

