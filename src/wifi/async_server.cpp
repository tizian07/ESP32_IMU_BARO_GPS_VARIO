#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <Update.h>
#include "common.h"
#include "drv/wdog.h"
#include "nv/flashlog.h"
#include "async_server.h"

static const char* TAG = "async_server";

// Credits : this is a mashup and modification of code from the following repositories
// https://github.com/smford/esp32-asyncwebserver-fileupload-example
// https://randomnerdtutorials.com/esp32-web-server-spiffs-spi-flash-file-system/
// I also added OTA firmware update, chunked spiffs file and datalog downloads

// connect to an existing WiFi access point as a station
//#define STATION_WEBSERVER

typedef struct WIFI_CONFIG_ {
  String ssid;               // wifi ssid
  String wifipassword;       // wifi password
  String httpuser;           // username to access web admin
  String httppassword;       // password to access web admin
  int webserverporthttp;     // http port number for web admin
} WIFI_CONFIG;

AsyncWebServer *server = NULL;  
static File SpiffsFile;
static File SpiffsDir;

const char* host = "esp32"; // use http://esp32.local instead of 192.168.4.1

// You must specify the WiFi Access point SSID and password if STATION_WEBSERVER is defined above
const String default_ssid = "SSID";
const String default_wifipassword = "PASSWORD";

// Access to webpage http://esp32.local is protected with a username and password
// Change these to whatever you want
const String default_httpuser = "admin";
const String default_httppassword = "admin";
const int default_webserverporthttp = 80;

static WIFI_CONFIG config;    

static String server_directory(bool ishtml = false);
static void server_not_found(AsyncWebServerRequest *request);
static bool server_authenticate(AsyncWebServerRequest * request);
static void server_handle_upload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final);
static void server_handle_SPIFFS_upload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final);
static String server_string_processor(const String& var);
static void server_configure();
static void server_handle_OTA_update(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final);
static int spiffs_chunked_read(uint8_t* buffer, size_t maxLen);
static int datalog_chunked_read(uint8_t* buffer, size_t maxLen, size_t index);


void server_init() {
  config.httpuser = default_httpuser;
  config.httppassword = default_httppassword;
  config.webserverporthttp = default_webserverporthttp;

#ifdef STATION_WEBSERVER
  // connect to existing WiFi access point as a station
  // the SSID and PASSWORD must be specified
  config.ssid = default_ssid;
  config.wifipassword = default_wifipassword;

  ESP_LOGD(TAG,"Connecting as station to existing Wifi Access Point");
  WiFi.begin(config.ssid.c_str(), config.wifipassword.c_str());
  WiFi.setTxPower(WIFI_POWER_MINUS_1dBm);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    //ESP_LOGD(TAG,".");
    }
  ESP_LOGD(TAG,"Network Configuration:");
  ESP_LOGD(TAG,"         SSID: %s", WiFi.SSID());
  ESP_LOGD(TAG,"  Wifi Status: %d", WiFi.status());
  ESP_LOGD(TAG,"Wifi Strength: %d dBm", WiFi.RSSI());
  ESP_LOGD(TAG,"          MAC: %s", WiFi.macAddress().c_str());
  ESP_LOGD(TAG,"           IP: %s", WiFi.localIP().toString().c_str());
  ESP_LOGD(TAG,"       Subnet: %s", WiFi.subnetMask().toString().c_str());
  ESP_LOGD(TAG,"      Gateway: %s", WiFi.gatewayIP().toString().c_str());
  ESP_LOGD(TAG,"        DNS 1: %s", WiFi.dnsIP(0).toString().c_str());
  ESP_LOGD(TAG,"        DNS 2: %s", WiFi.dnsIP(1).toString().c_str());
  ESP_LOGD(TAG,"        DNS 3: %s", WiFi.dnsIP(2).toString().c_str());
  if (!MDNS.begin(host)) { // Web server page is at http://esp32.local
    ESP_LOGD(TAG,"Error setting up MDNS responder, hang here ...");
    while (1) {
      delay(1000);
      }
    }
  ESP_LOGD(TAG,"mDNS responder started");
#else 
  WiFi.mode(WIFI_AP);
  WiFi.setTxPower(WIFI_POWER_MINUS_1dBm);
  // set up as stand-alone WiFi Access Point with SSID = "Esp32GpsVario", 
  // and no access password is required
  // you can specify an access password if you like below
  bool result =  WiFi.softAP("Esp32GpsVario", ""); // "" => no password
  ESP_LOGD(TAG, "AP setup %s",result == true ? "OK" : "failed");
  IPAddress myIP = WiFi.softAPIP();  
  ESP_LOGD(TAG,"Access Point IP address: %s", myIP.toString().c_str());
  
  if (!MDNS.begin(host)) { // Use http://esp32.local for web server page
    ESP_LOGD(TAG,"Error setting up MDNS responder!");
    while (1) {
      delay(1000);
      }
    }
  ESP_LOGD(TAG,"mDNS responder started");
#endif

  ESP_LOGD(TAG,"Configuring Webserver ...");
  server = new AsyncWebServer(config.webserverporthttp);
  server_configure();

  ESP_LOGD(TAG,"Starting Webserver ...");
  server->begin();
}


// Make size of files human readable
// source: https://github.com/CelliesProjects/minimalUploadAuthESP32
String server_ui_size(const size_t bytes) {
  if (bytes < 1024) return String(bytes) + " B";
  else if (bytes < (1024 * 1024)) return String(bytes / 1024.0) + " KB";
  else if (bytes < (1024 * 1024 * 1024)) return String(bytes / 1024.0 / 1024.0) + " MB";
  else return String(bytes / 1024.0 / 1024.0 / 1024.0) + " GB";
  }


// list all spiffs partition files. If isHtml == true, return html formatted table rather than simple text
static String server_directory(bool isHtml) {
  String returnText = "";
  ESP_LOGD(TAG,"Listing files stored on SPIFFS");
  File root = SPIFFS.open("/");
  File foundfile = root.openNextFile();
  String fileName = foundfile.name();
  if (isHtml) {
    returnText += "<p align='center'>SPIFFS file storage : <span id='totalspiffs'>" + server_ui_size(SPIFFS.totalBytes()) + "</span> | Used : <span id='usedspiffs'>" + server_ui_size(SPIFFS.usedBytes()) + "</span></p>";
    returnText += "<table align='center'><tr><th align='left'>Name</th><th align='left'>Size</th><th></th><th></th></tr>";
    }
  while (foundfile) {
    fileName = foundfile.name();
    if (isHtml) {
      ESP_LOGD(TAG, "%s", fileName.c_str());
      if (fileName.endsWith(".html") || fileName.endsWith(".css")) {
        // do not list .html and .css files 
        } 
      else {
        // format as html table entry with file name, size, download and delete buttons
        returnText += "<tr align='left'><td>" + fileName + "</td><td>" + server_ui_size(foundfile.size()) + "</td>";
        returnText += "<td><button class='directory_buttons' onclick=\"directory_button_handler(\'" + fileName + "\', \'download\')\">Download</button>";
        returnText += "<td><button class='directory_buttons' onclick=\"directory_button_handler(\'" + fileName + "\', \'delete\')\">Delete</button></tr>";
        }
      } 
    else {
      // format as plain text
      returnText += "File: " + fileName + " Size: " + server_ui_size(foundfile.size()) + "\n";
      }
    foundfile.close();
    foundfile = root.openNextFile();
    }
  root.close();
  if (isHtml) {
    returnText += "</table>";
    }
  return returnText;
  }


// replace %var%  in webpage with dynamically generated string
static String server_string_processor(const String& var) {
    if (var == "BUILD_TIMESTAMP") {
        return String(__DATE__) + " " + String(__TIME__); 
        }
    else
    if (var == "TOTALDATALOG") {
        return server_ui_size(FLASH_SIZE_BYTES);
        }
    if (var == "USEDDATALOG") {
        return server_ui_size(FlashLogFreeAddress);
        }
    else
        return "?";
    }


// chunked download for SPIFFS files, this is needed for files larger than a few hundred bytes.
// returns buffer with up to maxLen bytes read
static int spiffs_chunked_read(uint8_t* buffer, size_t maxLen) {              
  // ESP_LOGD(TAG, "MaxLen = %d", maxLen);
  if (!SpiffsFile.available()) {
    SpiffsFile.close();
    return 0;
    }
  else {
    int count = 0;
    while (SpiffsFile.available() && (count < maxLen)) {
      buffer[count] = SpiffsFile.read();
      count++;
      }
    return count;
    }
}


// chunked download for the IMU/GPS data log stored in external spi flash. 
// returns buffer with up to maxLen bytes read
static int datalog_chunked_read(uint8_t* buffer, size_t maxLen, size_t index) {
  int bytesRemaining = (int)(FlashLogFreeAddress - (uint32_t)index);
  if (bytesRemaining) {
    int numBytes =  bytesRemaining > maxLen ? maxLen : bytesRemaining;  
    spiflash_readBuffer(index, buffer, numBytes);
    return numBytes;
    }
  else {
    return 0;
    }
  }


// define the handlers
void server_configure() {
  server->onNotFound(server_not_found);
  server->onFileUpload(server_handle_upload);
  server->on("/", HTTP_GET, [](AsyncWebServerRequest * request) {
    ESP_LOGD(TAG,"Client: %s %s",request->client()->remoteIP().toString().c_str(), request->url().c_str());
    if (server_authenticate(request)) {
      ESP_LOGD(TAG," Auth: Success");
      request->send(SPIFFS, "/index.html", String(), false, server_string_processor);
      }  
    else {
      ESP_LOGD(TAG," Auth: Failed");
      return request->requestAuthentication();
      }
  });

  // Route to load style.css file
  server->on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/style.css", "text/css");
  });

  server->on("/datalog", HTTP_GET, [](AsyncWebServerRequest * request) {
    ESP_LOGD(TAG,"Client: %s %s",request->client()->remoteIP().toString().c_str(), request->url().c_str());
    if (FlashLogFreeAddress) {
      AsyncWebServerResponse *response = request->beginResponse("application/octet-stream", FlashLogFreeAddress, [](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
        feed_watchdog();
        delayMs(5);
        return datalog_chunked_read(buffer, maxLen, index);
      });
      response->addHeader("Content-Disposition", "attachment; filename=datalog");
      response->addHeader("Connection", "close");
      request->send(response);
      }
    else {
      request->send(400, "text/plain", "ERROR : no data log found");
      }
    ESP_LOGD(TAG," Datalog Download : Success");
    });

  server->on("/directory", HTTP_GET, [](AsyncWebServerRequest * request)  {
    ESP_LOGD(TAG,"Client: %s %s",request->client()->remoteIP().toString(), request->url().c_str());
    if (server_authenticate(request)) {
      ESP_LOGD(TAG," Auth: Success");
      request->send(200, "text/plain", server_directory(true)); // send html formatted table
      } 
    else {
      ESP_LOGD(TAG, " Auth: Failed");
      return request->requestAuthentication();
      }
    });

  server->on("/file", HTTP_GET, [](AsyncWebServerRequest * request) {
    ESP_LOGD(TAG,"Client: %s %s",request->client()->remoteIP().toString().c_str(), request->url().c_str());
    if (server_authenticate(request)) {
      ESP_LOGD(TAG," Auth: Success");
      if (request->hasParam("name") && request->hasParam("action")) {
        String fname = "/" + request->getParam("name")->value();
        const char* fileName = fname.c_str();
        const char *fileAction = request->getParam("action")->value().c_str();
        ESP_LOGD(TAG, "Client : %s %s ?name = %s &action = %s",request->client()->remoteIP().toString().c_str(), request->url().c_str(), fileName, fileAction);
        if (!SPIFFS.exists(fileName)) {
          ESP_LOGE(TAG," ERROR: file does not exist");
          request->send(400, "text/plain", "ERROR: file does not exist");
          } 
        else {
          ESP_LOGD(TAG," file exists");
          if (strcmp(fileAction, "download") == 0) {
            ESP_LOGD(TAG, " downloaded");
            SpiffsFile = SPIFFS.open(fileName, "r");
            int sizeBytes = SpiffsFile.size();
            AsyncWebServerResponse *response = request->beginResponse("application/octet-stream", sizeBytes, [](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
              feed_watchdog();
              delayMs(5);
              return spiffs_chunked_read(buffer, maxLen);
              });
            char szBuf[80];
            sprintf(szBuf, "attachment; filename=%s", &fileName[1]);// get past the leading '/'
            response->addHeader("Content-Disposition", szBuf);
            response->addHeader("Connection", "close");
            request->send(response);
            } 
          else if (strcmp(fileAction, "delete") == 0) {
            ESP_LOGD(TAG, " deleted");
            SPIFFS.remove(fileName);
            request->send(200, "text/plain", "Deleted File: " + String(fileName));
            } 
          else {
            ESP_LOGE(TAG," ERROR: invalid action param supplied");
            request->send(400, "text/plain", "ERROR: invalid action param supplied");
            }
          }
        } 
      else {
        ESP_LOGE(TAG," ERROR: name and action param required");
        request->send(400, "text/plain", "ERROR: name and action params required");
        }
      } 
    else {
      ESP_LOGD(TAG," Auth: Failed");
      return request->requestAuthentication();
      }
    });
  }


static void server_not_found(AsyncWebServerRequest *request) {
  ESP_LOGD(TAG,"Client: %s %s",request->client()->remoteIP().toString().c_str(), request->url().c_str());
  request->send(404, "text/plain", "Not found");
  }
  

// used by server.on functions to discern whether a user has the correct httpapitoken OR is authenticated by username and password
bool server_authenticate(AsyncWebServerRequest * request) {
  bool isAuthenticated = false;
  if (request->authenticate(config.httpuser.c_str(), config.httppassword.c_str())) {
    ESP_LOGD(TAG,"is authenticated via username and password");
    isAuthenticated = true;
    }
  return isAuthenticated;
  }


static void server_handle_upload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
  if (filename.endsWith(".bin") ) {
    // .bin files uploaded are processed as application firmware updates
    server_handle_OTA_update(request, filename, index, data, len, final);
    }
  else {
    // non .bin files are uploaded to the SPIFFS partition
    size_t freeBytes = SPIFFS.totalBytes() - SPIFFS.usedBytes();
    if (len < freeBytes) {
      server_handle_SPIFFS_upload(request, filename, index, data, len, final);
      }
    else {
      ESP_LOGD(TAG, "Cannot upload file size %d bytes, as SPIFFS free space = %d bytes", len, freeBytes);
      request->send(404, "text/plain", "Not enough free space in SPIFFS partition");
      }
    }
  }


// handles non .bin file uploads to the SPIFFS directory
static void server_handle_SPIFFS_upload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
  // make sure authenticated before allowing upload
  if (server_authenticate(request)) {
    ESP_LOGD(TAG,"Client: %s %s",request->client()->remoteIP().toString().c_str(), request->url().c_str());
    // open the file on first call
    if (index == 0) {
      ESP_LOGD(TAG,"Upload Start : %s", filename.c_str());
      filename = "/" + filename;
      SpiffsDir = SPIFFS.open("/");
      if (SPIFFS.exists(filename)) {
        bool res = SPIFFS.remove(filename);
        ESP_LOGD(TAG, "Delete file %s : %s", filename, res == false ? "Error" : "OK");
        }
      SpiffsFile = SPIFFS.open(filename, FILE_WRITE);
      }

    if (len) {
      // stream the incoming chunk to the opened file
      SpiffsFile.write(data, len);
      delayMs(5);
      feed_watchdog();
      ESP_LOGD(TAG,"Writing file : %s, index = %d, len = %d", filename.c_str(), index, len);
      }

    if (final) {
      ESP_LOGD(TAG,"Upload Complete : %s, size = %d", filename.c_str(), index+len);
      // close the file handle after upload
      SpiffsFile.close();
      SpiffsDir.close();
      }
    } 
  else {
    ESP_LOGD(TAG,"Auth: Failed");
    return request->requestAuthentication();
    }
  }


// handles OTA firmware update
static void server_handle_OTA_update(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
  // make sure authenticated before allowing upload
  if (server_authenticate(request)) {
    ESP_LOGD(TAG,"Client: %s %s",request->client()->remoteIP().toString().c_str(), request->url().c_str());
    if (!index) {
      ESP_LOGD(TAG,"OTA Update Start : %s", filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { //start with max available size
        Update.printError(Serial);
        }
      }

    if (len) {
     // flashing firmware to ESP
     if (Update.write(data, len) != len) {
        delayMs(5);
        feed_watchdog();
        Update.printError(Serial);
        }      
      ESP_LOGD(TAG,"Writing : %s index = %d len = %d", filename.c_str(), index, len);
      }

    if (final) {
      if (Update.end(true)) { //true to set the size to the current progress
        ESP_LOGD(TAG,"OTA Complete : %s, size = %d", filename.c_str(), index + len);
        } 
      else {
        Update.printError(Serial);
        }
      delayMs(1000);
      ESP.restart(); // reboot after firmware update
      }
    } 
  else {
    ESP_LOGD(TAG,"Auth: Failed");
    return request->requestAuthentication();
    }
  }