#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>

// --- Configuration ---
const char* AP_SSID = "Beetle-C6-Setup";
const char* AP_PASSWORD = "password123"; // Should be at least 8 characters
IPAddress localIP(192, 168, 4, 1);
IPAddress gateway(192, 168, 4, 1);
IPAddress subnet(255, 255, 255, 0);

// Pin for visual status (e.g., built-in LED on ESP32-C6 Mini)
const int LED_PIN = 15; // Updated: Pin 15 is the default onboard LED for the Beetle ESP32-C6 Mini

// --- Global Objects ---
WebServer server(80);
DNSServer dnsServer;
Preferences preferences;

// --- State Variables ---
bool isProvisioningMode = true;
const char* PREF_NAMESPACE = "wifi_config";
const char* PREF_KEY_SSID = "ssid";
const char* PREF_KEY_PASS = "pass";

// --- Function Declarations ---
void initProvisioning();
void initSTA(const String& ssid, const String& password);
void handleRoot();
void handleSave();
void handleScan();
void handleNotFound();
void setStatusLED(int state);
void saveCredentials(const String& ssid, const String& password);
bool loadCredentials(String& ssid, String& password);
void checkSerialCommands(); // Declaration for the new function

// =================================================================
// --- Utility Functions ---
// =================================================================

void setStatusLED(int state) {
    digitalWrite(LED_PIN, state);
}

// Store credentials securely in NVS (Non-Volatile Storage)
void saveCredentials(const String& ssid, const String& password) {
    preferences.begin(PREF_NAMESPACE, false);
    preferences.putString(PREF_KEY_SSID, ssid);
    preferences.putString(PREF_KEY_PASS, password);
    preferences.end();
    Serial.println("Credentials saved to NVS.");
}

// Load credentials from NVS
bool loadCredentials(String& ssid, String& password) {
    preferences.begin(PREF_NAMESPACE, true); // true for read-only
    ssid = preferences.getString(PREF_KEY_SSID, "");
    password = preferences.getString(PREF_KEY_PASS, "");
    preferences.end();
    return ssid.length() > 0;
}

// Function to check and process serial input commands
void checkSerialCommands() {
    if (Serial.available() > 0) {
        // Read the incoming line
        String command = Serial.readStringUntil('\n');
        command.trim(); // Remove leading/trailing whitespace

        if (command.equalsIgnoreCase("reset")) {
            Serial.println("\n--- SERIAL COMMAND RECEIVED: RESET ---");
            
            // 1. Clear stored credentials
            preferences.begin(PREF_NAMESPACE, false);
            preferences.clear();
            preferences.end();
            Serial.println("Stored Wi-Fi credentials cleared.");
            
            // 2. Disconnect any active Wi-Fi connection
            WiFi.disconnect(true);
            
            // 3. Stop running services (if currently in AP mode)
            if (isProvisioningMode) {
                server.stop(); 
                dnsServer.stop();
                WiFi.softAPdisconnect(true);
            }
            
            // 4. Force device back to provisioning mode
            Serial.println("Returning to Provisioning Mode.");
            delay(500);
            initProvisioning(); 
        } else {
            Serial.print("Unknown serial command received: ");
            Serial.println(command);
        }
    }
}


// =================================================================
// --- Mode Initialization Functions ---
// =================================================================

void initProvisioning() {
    isProvisioningMode = true;
    Serial.println("\n--- Entering Provisioning Mode (AP) ---");

    setStatusLED(HIGH); // LED ON in Provisioning Mode

    // 1. Configure Soft AP
    WiFi.mode(WIFI_AP);
    if (!WiFi.softAPConfig(localIP, gateway, subnet)) {
        Serial.println("AP Configuration failed!");
        return;
    }
    if (!WiFi.softAP(AP_SSID, AP_PASSWORD)) {
        Serial.println("SoftAP creation failed!");
        return;
    }

    Serial.print("Access Point: ");
    Serial.println(AP_SSID);
    Serial.print("IP Address: ");
    Serial.println(WiFi.softAPIP());

    // 2. Start DNS Server for Captive Portal
    dnsServer.start(53, "*", localIP);
    Serial.println("DNS Server started.");

    // 3. Register Web Server Handlers
    server.on("/", HTTP_GET, handleRoot);
    server.on("/scan", HTTP_GET, handleScan);
    server.on("/save", HTTP_POST, handleSave);
    server.onNotFound(handleNotFound);

    server.begin();
    Serial.println("HTTP Server started.");
}

void initSTA(const String& ssid, const String& password) {
    isProvisioningMode = false;
    Serial.println("\n--- Entering Operational Mode (STA) ---");

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());

    Serial.print("Connecting to Wi-Fi '");
    Serial.print(ssid);
    Serial.print("'");

    int timeout = 20; // 10 seconds timeout
    while (WiFi.status() != WL_CONNECTED && timeout-- > 0) {
        delay(500);
        Serial.print(".");
        setStatusLED(timeout % 2 == 0 ? HIGH : LOW); // Blink while connecting
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nConnected successfully!");
        Serial.print("IP Address: ");
        Serial.println(WiFi.localIP());

        // Stop provisioning services if they were running
        server.stop();
        dnsServer.stop();
    } else {
        Serial.println("\nConnection failed! Erasing config and reverting to AP mode.");
        // Connection failed, erase stored bad credentials and restart provisioning
        preferences.begin(PREF_NAMESPACE, false);
        preferences.clear();
        preferences.end();

        WiFi.disconnect(true); // Disconnect and clear config
        delay(100);
        initProvisioning(); // Re-enter provisioning mode
    }
}

// =================================================================
// --- Web Server Handlers ---
// =================================================================

// Handles all requests not handled by specific paths.
// Crucial for captive portal detection and redirects.
void handleNotFound() {
    // Captive portal detection paths (e.g., from iOS/Android)
    if (server.hostHeader().indexOf("generate_204") != -1 ||
        server.uri().indexOf("hotspot-detect.html") != -1 ||
        server.uri().indexOf("redirect") != -1) {
        
        Serial.println("Detected Captive Portal Check, redirecting...");
        // Send a 302 redirect to the root page (AP IP)
        server.sendHeader("Location", "http://" + localIP.toString() + "/");
        server.send(302, "text/plain", "");
    } else {
        // For all other unknown requests, show the provisioning page
        handleRoot();
    }
}

// Serves the main HTML/CSS/JS portal page
void handleRoot() {
    Serial.println("Serving root page.");
    String html = R"raw(
<!DOCTYPE html>
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32-C6 Wi-Fi Setup</title>
    <style>
        body { font-family: sans-serif; background-color: #f4f7f6; display: flex; justify-content: center; align-items: center; min-height: 100vh; margin: 0; }
        .container { background-color: white; padding: 25px; border-radius: 12px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); width: 90%; max-width: 400px; }
        h1 { color: #2c3e50; font-size: 1.5rem; text-align: center; margin-bottom: 20px; }
        label { display: block; margin-top: 15px; margin-bottom: 5px; font-weight: bold; color: #34495e; }
        select, input[type="password"] { width: 100%; padding: 10px; margin-bottom: 15px; border: 1px solid #ccc; border-radius: 6px; box-sizing: border-box; }
        button { background-color: #3498db; color: white; padding: 12px 20px; border: none; border-radius: 6px; cursor: pointer; width: 100%; font-size: 1rem; transition: background-color 0.3s; margin-top: 10px; }
        button:hover { background-color: #2980b9; }
        #status { text-align: center; margin-top: 20px; padding: 10px; border-radius: 6px; background-color: #ecf0f1; color: #2c3e50; }
        .scan-button { background-color: #2ecc71; }
        .scan-button:hover { background-color: #27ae60; }
        .logo { text-align: center; font-size: 2rem; color: #3498db; margin-bottom: 10px; }
    </style>
</head>
<body>
    <div class="container">
        <div class="logo">&#9974;</div> <!-- Beetle icon -->
        <h1>Wi-Fi Provisioning Portal</h1>
        <form id="wifiForm">
            <label for="ssid">Choose Network (SSID):</label>
            <select id="ssid" name="ssid" required>
                <option value="" disabled selected>Scanning...</option>
            </select>
            
            <button type="button" class="scan-button" onclick="scanNetworks()">&#x21bb; Scan Networks</button>

            <label for="password">Password:</label>
            <input type="password" id="password" name="password" placeholder="Enter network password" required>

            <button type="submit" id="saveBtn">Connect & Save</button>
        </form>
        <div id="status">Ready to configure.</div>
    </div>

    <script>
        const form = document.getElementById('wifiForm');
        const ssidSelect = document.getElementById('ssid');
        const statusDiv = document.getElementById('status');
        const saveBtn = document.getElementById('saveBtn');

        // Function to update status message
        function updateStatus(message) {
            statusDiv.innerHTML = message;
        }

        // Function to fetch and display SSIDs
        function scanNetworks() {
            updateStatus('Scanning for networks...');
            saveBtn.disabled = true;
            
            fetch('/scan')
                .then(response => response.json())
                .then(data => {
                    ssidSelect.innerHTML = '';
                    if (data.length === 0) {
                        const option = document.createElement('option');
                        option.value = '';
                        option.text = 'No networks found';
                        option.disabled = true;
                        option.selected = true;
                        ssidSelect.appendChild(option);
                        updateStatus('No Wi-Fi networks found. Try scanning again.');
                    } else {
                        const defaultOption = document.createElement('option');
                        defaultOption.value = '';
                        defaultOption.text = 'Select an SSID...';
                        defaultOption.disabled = true;
                        defaultOption.selected = true;
                        ssidSelect.appendChild(defaultOption);

                        data.forEach(network => {
                            const option = document.createElement('option');
                            option.value = network.ssid;
                            option.text = `${network.ssid} (${network.rssi} dBm) [${network.auth ? 'Secured' : 'Open'}]`;
                            ssidSelect.appendChild(option);
                        });
                        updateStatus(`Found ${data.length} networks. Select your network.`);
                    }
                    saveBtn.disabled = false;
                })
                .catch(error => {
                    console.error('Scan Error:', error);
                    // Updated message for better user guidance
                    updateStatus('Network request failed. Check device connectivity or serial console for details.'); 
                    saveBtn.disabled = false;
                });
        }

        // Handle form submission to save credentials
        form.addEventListener('submit', function(e) {
            e.preventDefault();
            const selectedSsid = ssidSelect.value;
            const password = document.getElementById('password').value;

            if (!selectedSsid || !password) {
                 updateStatus('Please select an SSID and enter the password.');
                 return;
            }

            updateStatus('Attempting to connect and save credentials...');
            saveBtn.disabled = true;

            const formData = new URLSearchParams();
            formData.append('ssid', selectedSsid);
            formData.append('password', password);

            fetch('/save', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/x-www-form-urlencoded'
                },
                body: formData
            })
            .then(response => response.json())
            .then(data => {
                if (data.status === 'success') {
                    updateStatus('Configuration successful! The device is now rebooting/switching to your network.');
                    // The device will restart/switch modes, so the page will eventually become unavailable.
                } else {
                    updateStatus(`Connection failed: ${data.message}. Please check credentials and try again.`);
                    saveBtn.disabled = false;
                }
            })
            .catch(error => {
                console.error('Save Error:', error);
                updateStatus('A network error occurred while saving. Try again.');
                saveBtn.disabled = false;
            });
        });

        // Initial scan on load
        window.onload = scanNetworks;

    </script>
</body>
</html>
)raw";
    server.send(200, "text/html", html);
}

// Handles the AJAX request to rescan networks
void handleScan() {
    Serial.println("Scanning networks...");
    
    // Scan all networks, hidden or not, blocking for up to 5000ms
    int n = WiFi.scanNetworks(false, true, false, 5000); 

    // Build JSON response
    DynamicJsonDocument doc(4096);
    JsonArray networks = doc.to<JsonArray>();

    if (n > 0) {
        for (int i = 0; i < n; ++i) {
            // Filter out empty SSIDs (e.g., hidden networks that didn't broadcast)
            if (WiFi.SSID(i).length() > 0) {
                JsonObject network = networks.add<JsonObject>();
                network["ssid"] = WiFi.SSID(i);
                network["rssi"] = WiFi.RSSI(i);
                // Use WiFi.encryptionType() for ESP32
                network["auth"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
            }
        }
    }
    
    // Clear scan results to free memory
    WiFi.scanDelete();

    String jsonResponse;
    serializeJson(doc, jsonResponse);
    
    server.send(200, "application/json", jsonResponse);
    Serial.print("Scan complete, found ");
    Serial.print(networks.size());
    Serial.println(" unique SSIDs.");
}

// Handles the POST request to save credentials
void handleSave() {
    DynamicJsonDocument responseDoc(256);

    if (server.method() == HTTP_POST && server.hasArg("ssid") && server.hasArg("password")) {
        String newSsid = server.arg("ssid");
        String newPassword = server.arg("password");
        
        Serial.print("Attempting to connect to: ");
        Serial.println(newSsid);

        // Disconnect from AP mode services
        server.stop(); 
        dnsServer.stop();
        WiFi.softAPdisconnect(true); 

        // Temporarily try to connect with the new credentials
        WiFi.mode(WIFI_STA);
        WiFi.begin(newSsid.c_str(), newPassword.c_str());

        int timeout = 30; // 15 seconds connection timeout
        while (WiFi.status() != WL_CONNECTED && timeout-- > 0) {
            delay(500);
        }

        if (WiFi.status() == WL_CONNECTED) {
            // Connection successful! Save and switch to operational mode.
            saveCredentials(newSsid, newPassword);
            responseDoc["status"] = "success";
            responseDoc["message"] = "Connected and configuration saved.";
            Serial.println("Configuration successful! Switching to STA mode.");
        } else {
            // Connection failed. Stay in provisioning mode (AP).
            responseDoc["status"] = "error";
            responseDoc["message"] = "Failed to connect to the network. Please check credentials.";
            Serial.println("Connection failed. Re-initializing AP mode.");
            
            // Re-initialize AP mode for the user to try again
            initProvisioning(); 
        }
    } else {
        responseDoc["status"] = "error";
        responseDoc["message"] = "Invalid request or missing parameters.";
    }

    String jsonResponse;
    serializeJson(responseDoc, jsonResponse);
    server.send(200, "application/json", jsonResponse);
}

// =================================================================
// --- Arduino Setup and Loop ---
// =================================================================

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\nBeetle ESP32-C6 Mini Provisioning Start");

    // Configure LED pin
    pinMode(LED_PIN, OUTPUT);
    setStatusLED(HIGH); // LED on initially

    String savedSsid, savedPassword;

    // 1. Check for saved credentials
    if (loadCredentials(savedSsid, savedPassword)) {
        Serial.println("Found saved credentials. Attempting connection.");
        // If credentials exist, try to connect in STA mode
        initSTA(savedSsid, savedPassword);
    } else {
        Serial.println("No saved credentials found. Starting provisioning AP.");
        // If no credentials, start the AP/Portal
        initProvisioning();
    }
}

void loop() {
    // Check for serial commands regardless of the current mode
    checkSerialCommands();

    if (isProvisioningMode) {
        // While in provisioning mode (AP + Portal)
        dnsServer.processNextRequest();
        server.handleClient();
        // Simple heartbeat blink (ON for 1s, OFF for 1s)
        setStatusLED(millis() % 2000 < 1000 ? HIGH : LOW);
    } else {
        // While in operational mode (STA)
        // Check if the connection dropped (e.g., if the router restarts)
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("Connection lost. Re-attempting connection...");
            String savedSsid, savedPassword;
            if (loadCredentials(savedSsid, savedPassword)) {
                // Try to reconnect
                initSTA(savedSsid, savedPassword); 
            } else {
                // Should not happen if credentials were saved, but as a safeguard
                initProvisioning(); 
            }
        }
        
        // --- LED Blinking Logic for Successful Connection (STA Mode) ---
        // The LED will blink at a rate of 500ms on/500ms off (1 second period).
        static bool led_state = false;
        led_state = !led_state;
        setStatusLED(led_state ? HIGH : LOW); 
        delay(500);
    }
}
