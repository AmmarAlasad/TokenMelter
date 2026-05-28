#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>

// Include custom compiled image assets and secure credentials
// #include "claude_logo.h" // Commented out to keep compiled binary light
#include "codex_logo.h"
#include "auth_data.h"

// Declare display object
TFT_eSPI tft = TFT_eSPI();

// Theme Colors
#define COLOR_BACKGROUND 0x0000        // Black
#define COLOR_CLAUDE_ORANGE 0xDC68     // RGB(220, 108, 68)
#define COLOR_CODEX_TEAL 0x0C91        // RGB(13, 148, 136)
#define COLOR_TEXT_GREY 0x94B2         // RGB(148, 163, 184)
#define COLOR_WHITE 0xFFFF
#define COLOR_RED 0xF800

// State Management
enum AppState {
  STATE_CONNECTING_WIFI,
  STATE_WIFI_FAILED,
  STATE_FETCHING_API,
  STATE_API_FAILED,
  STATE_RUNNING
};

volatile AppState appState = STATE_CONNECTING_WIFI;
volatile bool screen_changed = true;
String statusErrorMsg = "";

// Live Fetched Codex Statistics (from ChatGPT wham API)
volatile float live_ratio_5h = 0.0;
volatile float live_ratio_wk = 0.0;
volatile double live_credits = 0.0;
String live_plan = "LOADING...";
String live_email = "LOADING...";
volatile bool live_allowed = true;
volatile bool live_limit_reached = false;
volatile uint32_t live_reset_after_seconds = 0;
volatile uint32_t live_fetch_timestamp = 0;

// Multi-Page Screen State
volatile uint8_t currentPage = 0;
volatile uint32_t lastPageShift = 0;
volatile uint32_t lastCountdownUpdate = 0;

// Static timing for blinking indicator
bool heart_state = false;
uint32_t last_tick = 0;

// Mutex to protect global shared variables from multi-threaded access
SemaphoreHandle_t dataMutex;

// Helper to get Wi-Fi status name as string
const char* getWiFiStatusName(wl_status_t status) {
  switch (status) {
    case WL_NO_SHIELD: return "NO_SHIELD_OR_OFF";
    case WL_IDLE_STATUS: return "IDLE";
    case WL_NO_SSID_AVAIL: return "NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED: return "SCAN_COMPLETED";
    case WL_CONNECTED: return "CONNECTED";
    case WL_CONNECT_FAILED: return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED: return "DISCONNECTED";
    default: return "UNKNOWN";
  }
}

// Helper to draw clean status overlays on connection/error
void drawStatusScreen(const char* title, const char* line2, const char* line3, uint16_t themeColor) {
  tft.fillScreen(COLOR_BACKGROUND);
  tft.drawRect(0, 0, 240, 240, themeColor);
  tft.drawRect(1, 1, 238, 238, themeColor);
  
  // Show Codex Logo centered
  tft.pushImage(88, 20, 64, 64, codex_logo);
  
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(themeColor, COLOR_BACKGROUND);
  tft.drawString(title, 120, 105, 4);
  
  tft.setTextColor(COLOR_WHITE, COLOR_BACKGROUND);
  tft.drawString(line2, 120, 140, 2);
  
  tft.setTextColor(COLOR_TEXT_GREY, COLOR_BACKGROUND);
  tft.drawString(line3, 120, 175, 2);
}

// Humorous silicon system commentaries removed to keep compiled binary light and avoid text overlap

// Helper function to format numbers with commas (e.g. 1,000,000)
String formatNumber(uint32_t num) {
  if (num < 1000) return String(num);
  
  uint32_t thousands = num / 1000;
  uint32_t remainder = num % 1000;
  
  char buf[16];
  if (thousands >= 1000) {
    uint32_t millions = thousands / 1000;
    uint32_t mil_rem = thousands % 1000;
    snprintf(buf, sizeof(buf), "%d,%03d,%03d", millions, mil_rem, remainder);
  } else {
    snprintf(buf, sizeof(buf), "%d,%03d", thousands, remainder);
  }
  return String(buf);
}

bool fetchCodexUsage() {
  Serial.println("[HTTP] Connecting to chatgpt.com...");
  WiFiClientSecure client;
  client.setInsecure(); // Bypass local SSL certificate check for stability
  
  HTTPClient http;
  http.begin(client, "https://chatgpt.com/backend-api/wham/usage");
  
  // Pass active tokens and headers retrieved dynamically via update_auth.py
  http.addHeader("Authorization", "Bearer " + String(CODEX_ACCESS_TOKEN));
  http.addHeader("ChatGPT-Account-Id", String(CODEX_ACCOUNT_ID));
  http.addHeader("User-Agent", "codex-cli");
  http.addHeader("Accept", "application/json");
  
  int httpCode = http.GET();
  Serial.printf("[HTTP] GET Code: %d\n", httpCode);
  
  if (httpCode > 0) {
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      Serial.println("[HTTP] Successful response received");
      
      // Parse ChatGPT JSON Response
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, payload);
      if (!error) {
        // Safe multithreaded copy using Mutex
        if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
          live_plan = doc["plan_type"].as<String>();
          live_plan.toUpperCase();
          
          if (!doc["email"].isNull()) {
            live_email = doc["email"].as<String>();
          } else {
            live_email = "no-email@chatgpt.com";
          }
          
          live_ratio_5h = doc["rate_limit"]["primary_window"]["used_percent"].as<float>() / 100.0;
          
          if (!doc["rate_limit"]["secondary_window"].isNull()) {
            live_ratio_wk = doc["rate_limit"]["secondary_window"]["used_percent"].as<float>() / 100.0;
          } else {
            live_ratio_wk = 0.0;
          }
          
          live_allowed = doc["rate_limit"]["allowed"].as<bool>();
          live_limit_reached = doc["rate_limit"]["limit_reached"].as<bool>();
          live_reset_after_seconds = doc["rate_limit"]["primary_window"]["reset_after_seconds"].as<uint32_t>();
          live_fetch_timestamp = millis();
          
          if (!doc["credits"].isNull() && !doc["credits"]["balance"].isNull()) {
            live_credits = doc["credits"]["balance"].as<double>();
          } else {
            live_credits = 0.0;
          }
          xSemaphoreGive(dataMutex);
        }
        
        http.end();
        return true;
      } else {
        Serial.printf("[HTTP] JSON Parse Error: %s\n", error.c_str());
        statusErrorMsg = "JSON Parse Err";
      }
    } else {
      Serial.printf("[HTTP] Failed with HTTP code: %d\n", httpCode);
      statusErrorMsg = "HTTP Err: " + String(httpCode);
    }
  } else {
    String errStr = http.errorToString(httpCode).c_str();
    Serial.printf("[HTTP] GET failed, error: %s\n", errStr.c_str());
    statusErrorMsg = errStr;
  }
  
  http.end();
  return false;
}

// Background FreeRTOS task running network connections and wham API fetches on Core 0
// This completely bypasses stack overflow limitations and leaves screen rendering 100% fluent
void networkTask(void * pvParameters) {
  // Clean initial Wi-Fi setup via a full driver reset cycle (WIFI_OFF -> WIFI_STA)
  Serial.println("[WIFI] Powering off Wi-Fi radio...");
  WiFi.mode(WIFI_OFF);
  vTaskDelay(pdMS_TO_TICKS(500));
  
  Serial.println("[WIFI] Initializing Station Mode...");
  WiFi.mode(WIFI_STA);
  vTaskDelay(pdMS_TO_TICKS(100));
  
  Serial.printf("[WIFI] Connecting to SSID: %s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  wl_status_t lastStatus = WL_IDLE_STATUS;

  while (true) {
    // 1. Monitor Wi-Fi Connection
    if (WiFi.status() != WL_CONNECTED) {
      appState = STATE_CONNECTING_WIFI;
      uint32_t start_connect = millis();
      
      while (WiFi.status() != WL_CONNECTED) {
        wl_status_t currentStatus = WiFi.status();
        if (currentStatus != lastStatus) {
          lastStatus = currentStatus;
          Serial.printf("[WIFI] Status Changed: %s (%d)\n", getWiFiStatusName(currentStatus), currentStatus);
        }
        
        vTaskDelay(pdMS_TO_TICKS(100)); // Non-blocking delay feeding FreeRTOS WDT
        
        // Timeout connection after 25 seconds and retry
        if (millis() - start_connect > 25000) {
          appState = STATE_WIFI_FAILED;
          Serial.println("[WIFI] Connection timeout! Powering off radio and retrying...");
          vTaskDelay(pdMS_TO_TICKS(5000));
          
          WiFi.mode(WIFI_OFF);
          vTaskDelay(pdMS_TO_TICKS(500));
          WiFi.mode(WIFI_STA);
          vTaskDelay(pdMS_TO_TICKS(100));
          WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
          start_connect = millis();
        }
      }
      
      Serial.print("[WIFI] Connected! IP Address: ");
      Serial.println(WiFi.localIP());
    }

    // 2. Fetch Live API Data
    appState = STATE_FETCHING_API;
    if (fetchCodexUsage()) {
      appState = STATE_RUNNING;
      screen_changed = true;
      
      // Successfully fetched. Wait 60 seconds before next fetch
      vTaskDelay(pdMS_TO_TICKS(60000));
    } else {
      appState = STATE_API_FAILED;
      // On API failure, retry after 10 seconds
      vTaskDelay(pdMS_TO_TICKS(10000));
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(500); // Give serial monitor time to connect
  Serial.println("=========================================");
  Serial.println("         ESP32 TOKENMELTER SYSTEM        ");
  Serial.println("=========================================");

  // Turn on the backlight layer connected to pin 15
  pinMode(15, OUTPUT);
  digitalWrite(15, HIGH);
  delay(200); // Give the display controller time to stabilize after power-on

  // Initialize display
  tft.init();
  tft.setRotation(0);
  tft.setSwapBytes(true); // Swap bytes for correct pixel color ordering
  
  tft.fillScreen(COLOR_BACKGROUND);
  
  // Create Semaphore Mutex to prevent data race conditions
  dataMutex = xSemaphoreCreateMutex();
  
  // Create the Background Network Task on Core 0 with a spacious 8KB stack
  xTaskCreatePinnedToCore(
    networkTask,      /* Task function */
    "NetworkTask",    /* String name of task */
    8192,             /* Stack size in words (8192 * 4 = 32 KB, perfectly safe for SSL) */
    NULL,             /* Parameter to pass to task */
    1,                /* Priority of task */
    NULL,             /* Task handle */
    0                 /* Pin to Core 0 (Main Loop is on Core 1) */
  );
  
  last_tick = millis();
}

void loop() {
  uint32_t current_time = millis();

  // --- INTERACTIVE SCREEN STATE RENDERING ---

  if (appState == STATE_CONNECTING_WIFI) {
    char statusBuf[32];
    snprintf(statusBuf, sizeof(statusBuf), "Status: %s", getWiFiStatusName(WiFi.status()));
    drawStatusScreen("CONNECTING...", WIFI_SSID, statusBuf, COLOR_CODEX_TEAL);
    delay(400);
  }
  
  else if (appState == STATE_WIFI_FAILED) {
    char statusBuf[32];
    snprintf(statusBuf, sizeof(statusBuf), "Status: %s", getWiFiStatusName(WiFi.status()));
    drawStatusScreen("WI-FI FAILED", "Check SSID/Password", statusBuf, COLOR_RED);
    delay(1000);
  }
  
  else if (appState == STATE_FETCHING_API) {
    drawStatusScreen("FETCHING DATA", "Querying chatgpt.com", "Please wait...", COLOR_CODEX_TEAL);
    delay(200);
  }
  
  else if (appState == STATE_API_FAILED) {
    drawStatusScreen("API FETCH ERROR", statusErrorMsg.c_str(), "Retrying shortly...", COLOR_RED);
    delay(1000);
  }
  
  else if (appState == STATE_RUNNING) {
    // Read thread-safe values using Mutex
    float local_ratio_5h = 0.0;
    float local_ratio_wk = 0.0;
    double local_credits = 0.0;
    String local_plan = "FREE";
    String local_email = "";
    bool local_allowed = true;
    bool local_limit_reached = false;
    uint32_t local_reset_after_seconds = 0;
    uint32_t local_fetch_timestamp = 0;
    
    if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
      local_ratio_5h = live_ratio_5h;
      local_ratio_wk = live_ratio_wk;
      local_credits = live_credits;
      local_plan = live_plan;
      local_email = live_email;
      local_allowed = live_allowed;
      local_limit_reached = live_limit_reached;
      local_reset_after_seconds = live_reset_after_seconds;
      local_fetch_timestamp = live_fetch_timestamp;
      xSemaphoreGive(dataMutex);
    }

    // A. Page cycling timer (shift page every 5 seconds)
    if (current_time - lastPageShift > 5000) {
      lastPageShift = current_time;
      currentPage = (currentPage + 1) % 3; // Cycle 0 -> 1 -> 2
      screen_changed = true;
      lastCountdownUpdate = 0; // Force immediate countdown update on Page 2 load
    }

    // B. Static Screen Refresh (Draw once when page transitions or new data is fetched)
    if (screen_changed) {
      screen_changed = false;
      tft.fillScreen(COLOR_BACKGROUND);
      
      // Draw standard glowing brand border
      tft.drawRect(0, 0, 240, 240, COLOR_CODEX_TEAL);
      tft.drawRect(1, 1, 238, 238, COLOR_CODEX_TEAL);
      
      // Render layout based on current active page
      if (currentPage == 0) {
        // --- PAGE 0: CORE USAGE ---
        tft.pushImage(15, 15, 64, 64, codex_logo);
        
        tft.setTextColor(COLOR_CODEX_TEAL, COLOR_BACKGROUND);
        tft.setTextDatum(ML_DATUM);
        tft.drawString("MELTER", 95, 36, 4);
        tft.setTextColor(COLOR_TEXT_GREY, COLOR_BACKGROUND);
        tft.drawString("PLAN: " + local_plan, 95, 60, 2);
        
        // Progress Bar borders
        tft.drawRoundRect(15, 90, 210, 18, 5, COLOR_CODEX_TEAL);
        tft.drawRoundRect(15, 145, 210, 18, 5, COLOR_CODEX_TEAL);
        
        // Bar Fills and Text
        tft.setTextDatum(TL_DATUM);
        uint16_t bar_5h = (uint16_t)(202 * local_ratio_5h);
        if (bar_5h > 202) bar_5h = 202;
        tft.fillRect(19, 94, bar_5h, 10, COLOR_CODEX_TEAL);
        tft.fillRect(19 + bar_5h, 94, 202 - bar_5h, 10, COLOR_BACKGROUND);
        
        tft.setTextColor(COLOR_WHITE, COLOR_BACKGROUND);
        String label_5h = "5H: Used " + String((int)(local_ratio_5h * 100)) + "%      ";
        tft.drawString(label_5h, 15, 114, 2);

        uint16_t bar_wk = (uint16_t)(202 * local_ratio_wk);
        if (bar_wk > 202) bar_wk = 202;
        tft.fillRect(19, 149, bar_wk, 10, COLOR_CODEX_TEAL);
        tft.fillRect(19 + bar_wk, 149, 202 - bar_wk, 10, COLOR_BACKGROUND);
        
        tft.setTextColor(COLOR_TEXT_GREY, COLOR_BACKGROUND);
        String label_wk = "WK: Used " + String((int)(local_ratio_wk * 100)) + "%      ";
        tft.drawString(label_wk, 15, 169, 2);

        // Credits footer
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(COLOR_WHITE, COLOR_BACKGROUND);
        tft.drawString("CREDITS: $" + String(local_credits, 2), 120, 202, 2);
      }
      else if (currentPage == 1) {
        // --- PAGE 1: ACCOUNT PROFILE ---
        tft.pushImage(15, 15, 64, 64, codex_logo);
        
        tft.setTextColor(COLOR_CODEX_TEAL, COLOR_BACKGROUND);
        tft.setTextDatum(ML_DATUM);
        tft.drawString("PROFILE", 95, 36, 4);
        tft.setTextColor(COLOR_TEXT_GREY, COLOR_BACKGROUND);
        tft.drawString("USER ACCT", 95, 60, 2);
        
        // Email Block
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(COLOR_CODEX_TEAL, COLOR_BACKGROUND);
        tft.drawString("ACCOUNT EMAIL", 120, 100, 2);
        tft.setTextColor(COLOR_WHITE, COLOR_BACKGROUND);
        tft.drawString(local_email, 120, 120, 2);
        
        // Plan Block
        tft.setTextColor(COLOR_CODEX_TEAL, COLOR_BACKGROUND);
        tft.drawString("MEMBERSHIP PLAN", 120, 150, 2);
        tft.setTextColor(COLOR_WHITE, COLOR_BACKGROUND);
        tft.drawString(local_plan, 120, 172, 4);
        
        // Status Block
        tft.setTextDatum(ML_DATUM);
        tft.setTextColor(COLOR_TEXT_GREY, COLOR_BACKGROUND);
        tft.drawString("ACCESS: ", 40, 202, 2);
        if (local_allowed) {
          tft.setTextColor(0x07E0, COLOR_BACKGROUND); // Green
          tft.drawString("ALLOWED", 120, 202, 2);
        } else {
          tft.setTextColor(COLOR_RED, COLOR_BACKGROUND);
          tft.drawString("BLOCKED", 120, 202, 2);
        }
      }
      else if (currentPage == 2) {
        // --- PAGE 2: RESET SCHEDULE ---
        tft.pushImage(15, 15, 64, 64, codex_logo);
        
        tft.setTextColor(COLOR_CODEX_TEAL, COLOR_BACKGROUND);
        tft.setTextDatum(ML_DATUM);
        tft.drawString("LIMITS", 95, 36, 4);
        tft.setTextColor(COLOR_TEXT_GREY, COLOR_BACKGROUND);
        tft.drawString("SCHEDULE", 95, 60, 2);
        
        // Reset header
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(COLOR_CODEX_TEAL, COLOR_BACKGROUND);
        tft.drawString("NEXT LIMIT RESET IN", 120, 100, 2);
        
        // Limit Reached Block
        tft.setTextColor(COLOR_CODEX_TEAL, COLOR_BACKGROUND);
        tft.drawString("RATE LIMIT MET?", 120, 160, 2);
        if (local_limit_reached) {
          tft.setTextColor(COLOR_RED, COLOR_BACKGROUND);
          tft.drawString("YES (BLOCKED)", 120, 182, 2);
        } else {
          tft.setTextColor(0x07E0, COLOR_BACKGROUND); // Green
          tft.drawString("NO (OK)", 120, 182, 2);
        }
      }

      // Draw bottom active status dot frame labels
      tft.setTextDatum(ML_DATUM);
      tft.setTextColor(COLOR_TEXT_GREY, COLOR_BACKGROUND);
      tft.drawString("SYS ACTIVE", 32, 226, 2);
      
      // Page number indicator at bottom right
      tft.setTextDatum(MR_DATUM);
      tft.setTextColor(COLOR_TEXT_GREY, COLOR_BACKGROUND);
      tft.drawString("PG " + String(currentPage + 1) + "/3", 218, 226, 2);
    }

    // C. Dynamic countdown timer on Page 2 (Ticking every second)
    if (currentPage == 2) {
      if (current_time - lastCountdownUpdate > 1000 || lastCountdownUpdate == 0) {
        lastCountdownUpdate = current_time;
        
        // Calculate remaining seconds
        int32_t remaining = 0;
        if (local_reset_after_seconds > 0) {
          int32_t elapsed = (current_time - local_fetch_timestamp) / 1000;
          remaining = (int32_t)local_reset_after_seconds - elapsed;
          if (remaining < 0) remaining = 0;
        }
        
        // Convert to hh:mm:ss
        int32_t hours = remaining / 3600;
        int32_t minutes = (remaining % 3600) / 60;
        int32_t seconds = remaining % 60;
        
        char timeStr[16];
        snprintf(timeStr, sizeof(timeStr), "%02dh %02dm %02ds", hours, minutes, seconds);
        
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(COLOR_WHITE, COLOR_BACKGROUND);
        tft.drawString(timeStr, 120, 126, 4); // Render in massive Font 4
      }
    }

    // D. Blinking Heartbeat active status dot
    if (current_time - last_tick > 400) {
      last_tick = current_time;
      heart_state = !heart_state;
      tft.fillCircle(18, 226, 4, heart_state ? 0x07E0 : 0x0200);
    }
    
    delay(33);
  }
}