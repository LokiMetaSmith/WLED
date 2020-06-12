#pragma once

#include "wled.h"

/*
Heavily modified copy of WebRadio Example
  Very simple HTML app to control web streaming
  
  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
 * Usermods allow you to add own functionality to WLED more easily
 * See: https://github.com/Aircoookie/WLED/wiki/Add-own-functionality
 * 
 * This is an example for a v2 usermod.
 * v2 usermods are class inheritance based and can (but don't have to) implement more functions, each of them is shown in this example.
 * Multiple v2 usermods can be added to one compilation easily.
 * 
 * Creating a usermod:
 * This file serves as an example. If you want to create a usermod, it is recommended to use usermod_v2_empty.h from the usermods folder as a template.
 * Please remember to rename the class and file to a descriptive name.
 * You may also use multiple .h and .cpp files.
 * 
 * Using a usermod:
 * 1. Copy the usermod into the sketch folder (same folder as wled00.ino)
 * 2. Register the usermod by adding #include "usermod_filename.h" in the top and registerUsermod(new MyUsermodClass()) in the bottom of usermods_list.cpp
 */

#include <Arduino.h>
#ifdef ESP32
    #include <WiFi.h>
#else
    #include <ESP8266WiFi.h>
#endif
#include "AudioFileSourceICYStream.h"
#include "AudioFileSourceBuffer.h"
#include "AudioGeneratorMP3.h"
#include "AudioGeneratorAAC.h"
#include "AudioOutputI2S.h"
#include <EEPROM.h>



//Pin defaults for QuinLed Dig-Uno
#ifdef ARDUINO_ARCH_ESP32
#define I2S_PIN 18
#else //ESP8266 boards
#define I2S_PIN 14
#endif





class UsermodWebSpeaker : public Usermod {
  private:
    //set last reading as "40 sec before boot", so first reading is taken after 20 sec
	#ifdef ESP8266
	const int preallocateBufferSize = 5*1024;
	const int preallocateCodecSize = 29192; // MP3 codec max mem needed
	#else
	const int preallocateBufferSize = 16*1024;
	const int preallocateCodecSize = 85332; // AAC+SBR codec max mem needed
	#endif
	void *preallocateBuffer = NULL;
	void *preallocateCodec = NULL;
	
	// To run, set your ESP8266 build to 160MHz, update the SSID info, and upload.
	AudioGenerator *decoder = NULL;
	AudioFileSourceICYStream *file = NULL;
	AudioFileSourceBuffer *buff = NULL;
	AudioOutputI2S *out = NULL;
	int volume = 100;
	char title[64];
	char url[96];
	char status[64];
	bool newUrl = false;
	bool isAAC = false;
	int retryms = 0;

	typedef struct {
	  char url[96];
	  bool isAAC;
	  int16_t volume;
	  int16_t checksum;
	} Settings;
	
    unsigned long lastTime = 0;

	public:
    
//Functions called by WLED
void StopPlaying()
{
  if (decoder) {
    decoder->stop();
    delete decoder;
    decoder = NULL;
  }
  if (buff) {
    buff->close();
    delete buff;
    buff = NULL;
  }
  if (file) {
    file->close();
    delete file;
    file = NULL;
  }
  strcpy_P(status, PSTR("Stopped"));
  strcpy_P(title, PSTR("Stopped"));
}
void LoadSettings()
{
  // Restore from EEPROM, check the checksum matches
  Settings s;
  uint8_t *ptr = reinterpret_cast<uint8_t *>(&s);
  EEPROM.begin(sizeof(s));
  for (size_t i=0; i<sizeof(s); i++) {
    ptr[i] = EEPROM.read(i+2750);
  }
  EEPROM.end();
  int16_t sum = 0x1234;
  for (size_t i=0; i<sizeof(url); i++) sum += s.url[i];
  sum += s.isAAC;
  sum += s.volume;
  if (s.checksum == sum) {
    strcpy(url, s.url);
    isAAC = s.isAAC;
    volume = s.volume;
    Serial.printf_P(PSTR("Resuming stream from EEPROM: %s, type=%s, vol=%d\n"), url, isAAC?"AAC":"MP3", volume);
    newUrl = true;
  }
}

void SaveSettings()
{
  // Store in "EEPROM" to restart automatically
  Settings s;
  memset(&s, 0, sizeof(s));
  strcpy(s.url, url);
  s.isAAC = isAAC;
  s.volume = volume;
  s.checksum = 0x1234;
  for (size_t i=0; i<sizeof(url); i++) s.checksum += s.url[i];
  s.checksum += s.isAAC;
  s.checksum += s.volume;
  uint8_t *ptr = reinterpret_cast<uint8_t *>(&s);
  EEPROM.begin(sizeof(s));
  for (size_t i=0; i<sizeof(s); i++) {
    EEPROM.write(i+2750, ptr[i]);
  }
 // EEPROM.put(2750,s);
  EEPROM.commit();
  EEPROM.end();
}
void PumpDecoder()
{
  if (decoder && decoder->isRunning()) {
    strcpy_P(status, PSTR("Playing")); // By default we're OK unless the decoder says otherwise
    if (!decoder->loop()) {
      Serial.printf_P(PSTR("Stopping decoder\n"));
      StopPlaying();
      retryms = millis() + 2000;
    }
  }
}
    /*
     * setup() is called once at boot. WiFi is not yet connected at this point.
     * You can use it to initialize variables, sensors or similar.
     */
    void setup() {
		// First, preallocate all the memory needed for the buffering and codecs, never to be freed
		preallocateBuffer = malloc(preallocateBufferSize);
		preallocateCodec = malloc(preallocateCodecSize);
		if (!preallocateBuffer || !preallocateCodec) {
			//Serial.println("Hello from my usermod!");
			Serial.printf_P(PSTR("FATAL ERROR:  Unable to preallocate %d bytes for app\n"), preallocateBufferSize+preallocateCodecSize);
		}
		server.begin();
		audioLogger = &Serial;
		file = NULL;
		buff = NULL;
		out = new AudioOutputI2S();
		decoder = NULL;
		LoadSettings();
    }
	/*
     * connected() is called every time the WiFi is (re)connected
     * Use it to initialize network interfaces
     */
	void connected() {
		//Serial.println("Connected to WiFi!");
		
  server.begin();
  
  strcpy_P(url, PSTR("none"));
  strcpy_P(status, PSTR("OK"));
  strcpy_P(title, PSTR("Idle"));

  audioLogger = &Serial;
  file = NULL;
  buff = NULL;
  out = new AudioOutputI2S();
  decoder = NULL;

  LoadSettings();
    }
	
    /*
     * loop() is called continuously. Here you can check for events, read sensors, etc.
     * 
     * Tips:
     * 1. You can use "if (WLED_CONNECTED)" to check for a successful network connection.
     *    Additionally, "if (WLED_MQTT_CONNECTED)" is available to check for a connection to an MQTT broker.
     * 
     * 2. Try to avoid using the delay() function. NEVER use delays longer than 10 milliseconds.
     *    Instead, use a timer check as shown here.
     */
    void loop() {
		PumpDecoder();
      if (millis() - lastTime > 1000)
      {
        //getReading();

        if (WLED_MQTT_CONNECTED) {
          char subuf[38];
          strcpy(subuf, mqttDeviceTopic);
          strcat(subuf, "/volume");
          mqtt->publish(subuf, 0, true, String(volume).c_str());
        }
        lastTime = millis();
      }
    }
	/*
     * addToJsonInfo() can be used to add custom entries to the /json/info part of the JSON API.
     * Creating an "u" object allows you to add custom key/value pairs to the Info section of the WLED web UI.
     * Below it is shown how this could be used for e.g. a light sensor
     */
    
    void addToJsonInfo(JsonObject& root) {
		
		JsonObject user = root["u"];
		if (user.isNull()) user = root.createNestedObject("u");

     // JsonArray temp = user.createNestedArray("Temperature");
		JsonArray volumeSpeaker = user.createNestedArray("u");
		volumeSpeaker.add(volume);
		JsonArray urlSpeaker = user.createNestedArray("u");
		urlSpeaker.add(url);
		JsonArray isAACSpeaker = user.createNestedArray("u");
		isAACSpeaker.add(isAAC);
     // if (volume == DEVICE_DISCONNECTED_C) {
     //   temp.add(0);
     //   temp.add(" Sensor Error!");
     //   return;
     // }

      
    }
	 /*
     * addToJsonState() can be used to add custom entries to the /json/state part of the JSON API (state object).
     * Values in the state object may be modified by connected clients
     */
    void addToJsonState(JsonObject& root)
    {
      //root["user0"] = userVar0;
    }
	/*
     * readFromJsonState() can be used to receive data clients send to the /json/state part of the JSON API (state object).
     * Values in the state object may be modified by connected clients
     */
    void readFromJsonState(JsonObject& root)
    {
      userVar0 = root["user0"] | userVar0; //if "user0" key exists in JSON, update, else keep old value
      //if (root["bri"] == 255) Serial.println(F("Don't burn down your garage!"));
    }
	
    /*
     * getId() allows you to optionally give your V2 usermod an unique ID (please define it in const.h!).
     * This could be used in the future for the system to determine whether your usermod is installed.
     */
    uint16_t getId()
    {
      return USERMOD_ID_WEBSPEAKER;
    }
};