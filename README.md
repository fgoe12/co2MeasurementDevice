# co2MeasurementDevice
An ESP 8266 microcontroller with a CO2 sensor measure the indoor air quality and remind to ventilate the room.
# Caution:
This device provides support for indoor ventilation. It measures co2. It cannot detect coronavirus. A corona infection cannot be prevented with this device!
# Idea:
The idea was to develop an indicator for the risk of infection with the coronavirus through aerosols. To determine the amount of aerosol in the air we decided to detect the amount of co2 in the air.
When breathing out CO2 is emitted. Over a longer period of time with several people in one room, the amount of CO2 in the room therefore increases.

If there is an infectious person in the room, the amount of aerosols containing the coronavirus will also increase.
The measured co2 value should be easily communicated to everyone and a simple reminder to ventilate the room should be provided to improve the air quality.
# Components:
<ul>
  <li>1x ESP8266 Node-MCU V3</li>
  <li>1x ESP8266 Node-MCU V3</li>
  <li>1x LED red yellow</li>
  <li>2x resistor for the LED (ex. 68 ohm)</li>
  <li>1x MH-Z19B CO2-Sensor</li>
  <li>1x Piezo Buzzer Alarm Sensor</li>
</ul>

# Schematics:
<img src="pic_trulli.jpg" alt="schematics">

# Implementation:
For the easy access we decides to build an IoT-device with an ESP8266. The measured values can be accessed by the everyone with his own smartphone. As reminder to ventilate the room, we use a piezo buzzer alarm sensor, which makes a beep sound when a certain co2-level is reached. An LED also indicates the Co2 levels.

The measured values can be accessed over a standalone WiFi from the ESP8266, called "CO2Measurement-XXXX" or over your local home-WiFi. Your home-WiFi can be typed in over the webpage of the ESP8266, when it is in standalone WiFi-mode. In standalone WiFi-mode the ESP8266 can be accessed over the ip-address: 10.10.10.1

In home-WiFi-mode the ESP8266 can be accesses over the local ip-address assigned by your home-router.

It is also possible to use the co2-sensor in your smarthome. For that the values are provided as xml-file.
