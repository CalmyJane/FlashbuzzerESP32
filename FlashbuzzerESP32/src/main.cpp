#include <map>
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Arduino.h>
#include <Preferences.h>  // For non-volatile memory storage (NVS)
#include <FastLED.h>

#include <vector>
#include <FastLED.h>

#define NUM_LEDS 1000      // Define the number of LEDs in your strip
#define LED_PIN 16        // Define your LED strip pin
#define DEFAULT_BRIGHTNESS 100  // Default brightness
#define DEFAULT_COLOR CRGB::Red // Default color for the running dots
#define BUTTON_PIN 13  // Pin where the button is connected
#define DEFAULT_WIDTH 1

class RunningDot {
public:
    // Constructor: Initialize variables with default color and brightness
    RunningDot() : lastUpdateTime(0), speed(30.0f), currentColor(DEFAULT_COLOR), currentBrightness(DEFAULT_BRIGHTNESS), dotWidth(DEFAULT_WIDTH) {}

    // Initialize FastLED in setup
    void begin()
    {
        FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
        FastLED.setBrightness(currentBrightness);
        FastLED.clear();
        FastLED.show();
    }

    // Trigger a new dot at the beginning of the strip with a float position
    void trigger()
    {
        activeDots.push_back(0.0f); // Add a new dot at position 0.0 (floating point)
    }

    // Update the position of all dots and render them
    void update()
    {
        // Get the current time
        unsigned long currentMillis = millis();

        // Calculate time passed in seconds since the last update
        float deltaTime = (currentMillis - lastUpdateTime) / 1000.0f;

        // If enough time has passed, update the positions of the dots
        if (deltaTime > 0.0f) {
            // Clear the strip for new positions
            FastLED.clear();

            // Update positions of all active dots based on time passed and speed
            for (int i = 0; i < activeDots.size(); i++)
            {
                if (activeDots[i] < NUM_LEDS)
                {
                    // Render the dot at its current floating position with brightness falloff
                    renderDot(activeDots[i]);
                }
                // Update the dot's position based on the speed (pixels/second)
                activeDots[i] += speed * deltaTime;
            }

            // Remove dots that have moved beyond the strip
            activeDots.erase(
                std::remove_if(activeDots.begin(), activeDots.end(),
                               [](float pos) { return pos >= NUM_LEDS; }),
                activeDots.end());

            // Show the updated LED strip
            FastLED.show();

            // Update the lastUpdateTime to the current time
            lastUpdateTime = currentMillis;
        }
    }

    // Set the speed of the running dots (pixels per second)
    void setSpeed(float newSpeed)
    {
        speed = newSpeed; // Speed is now in pixels/second
    }

    // Set the color of the running dots
    void setColor(CRGB newColor)
    {
        currentColor = newColor;
    }

    // Set the brightness of the LED strip
    void setBrightness(uint8_t newBrightness)
    {
        currentBrightness = newBrightness;
        FastLED.setBrightness(currentBrightness); // Update FastLED brightness setting
    }

    // Set the width of the dot (affects brightness falloff)
    void setWidth(float newWidth)
    {
        dotWidth = newWidth;
    }

private:
    CRGB leds[NUM_LEDS];           // Array to store the LED colors
    std::vector<float> activeDots; // Vector to store the positions of active dots (float positions)
    unsigned long lastUpdateTime;  // To track when the dots were last updated
    float speed;                   // Speed of the dots in pixels per second
    CRGB currentColor;             // Current color of the running dots
    uint8_t currentBrightness;     // Current brightness of the LED strip
    float dotWidth;                // Width of the dot (affects how quickly the brightness falls off)

    // Function to render a dot with smooth brightness fading
    void renderDot(float position)
    {
        int centerLED = (int)position; // The nearest integer LED to the dot's center position

        // Iterate over all LEDs to calculate their brightness based on distance from the dot center
        for (int i = 0; i < NUM_LEDS; i++)
        {
            float distance = fabs(i - position); // Calculate the distance of each LED from the dot center

            // If the LED is within the dotWidth range, calculate the brightness falloff
            if (distance < dotWidth)
            {
                // Calculate brightness as a fraction (falling off linearly from the center)
                float brightness = 1.0f - (distance / dotWidth);

                // Apply the brightness to the color and add it to the existing LED state
                leds[i] += CRGB(
                    (uint8_t)(currentColor.r * brightness),
                    (uint8_t)(currentColor.g * brightness),
                    (uint8_t)(currentColor.b * brightness));
            }
        }
    }
};

class ConfigParameter {
public:
    enum Type { STRING, FLOAT };

    // Default constructor
    ConfigParameter() : type(FLOAT), floatValue(0.0f) {}

    // Constructor for String parameter
    ConfigParameter(const String& name, const String& value)
        : name(name), type(STRING) {
        stringValue = new String(value);
    }

    // Constructor for Float parameter
    ConfigParameter(const String& name, float value)
        : name(name), type(FLOAT), floatValue(value) {}

    // Copy constructor
    ConfigParameter(const ConfigParameter& other)
        : name(other.name), type(other.type) {
        if (type == STRING) {
            stringValue = new String(*other.stringValue);
        } else {
            floatValue = other.floatValue;
        }
    }

    // Move constructor
    ConfigParameter(ConfigParameter&& other) noexcept
        : name(std::move(other.name)), type(other.type) {
        if (type == STRING) {
            stringValue = other.stringValue;
            other.stringValue = nullptr;
        } else {
            floatValue = other.floatValue;
        }
    }

    // Destructor to properly handle the string memory
    ~ConfigParameter() {
        if (type == STRING && stringValue) {
            delete stringValue;
        }
    }

    // Copy assignment operator
    ConfigParameter& operator=(const ConfigParameter& other) {
        if (this == &other) return *this;
        name = other.name;
        type = other.type;
        if (type == STRING) {
            if (stringValue) {
                *stringValue = *other.stringValue;
            } else {
                stringValue = new String(*other.stringValue);
            }
        } else {
            floatValue = other.floatValue;
        }
        return *this;
    }

    // Move assignment operator
    ConfigParameter& operator=(ConfigParameter&& other) noexcept {
        if (this == &other) return *this;
        name = std::move(other.name);
        type = other.type;
        if (type == STRING) {
            delete stringValue;
            stringValue = other.stringValue;
            other.stringValue = nullptr;
        } else {
            floatValue = other.floatValue;
        }
        return *this;
    }

    // Getter for parameter name
    String getName() const { return name; }

    // Getter for type
    Type getType() const { return type; }

    // Get the String value (only call if type is STRING)
    String getStringValue() const {
        if (type == STRING && stringValue) return *stringValue;
        return "";
    }

    // Get the float value (only call if type is FLOAT)
    float getFloatValue() const {
        if (type == FLOAT) return floatValue;
        return 0.0;
    }

    // Setter for String value
    void setValue(const String& newValue) {
        if (type == STRING) {
            if (stringValue) {
                *stringValue = newValue;
            } else {
                stringValue = new String(newValue);
            }
        }
    }

    // Setter for float value
    void setValue(float newValue) {
        if (type == FLOAT) {
            floatValue = newValue;
        }
    }

private:
    String name;
    Type type;
    union {
        String* stringValue;
        float floatValue;
    };
};



class WebConfig {
public:
    WebConfig(const char* ssid, const char* password)
        : softAP_ssid(ssid), softAP_password(password), server(80), title("Configuration Page") {}

    void begin() {
        preferences.begin("webconfig", false);  // Open NVS with namespace 'webconfig'
        loadParameters();  // Load parameters from NVS on startup
        configureAccessPoint();
        setupDNS();
        setupWebServer();
    }

    void handleClient() {
        dnsServer.processNextRequest();
        server.handleClient();
    }

    void addParamString(const String& name, const String& defaultValue) {
        String storedValue = defaultValue;
        if (preferences.isKey(name.c_str())) {
            // Load the value from NVS (either default or stored)
            storedValue = preferences.getString(name.c_str(), defaultValue);
        }
        configParams[name] = ConfigParameter(name, storedValue);
        saveParameter(name);
    }

    void addParamFloat(const String& name, float defaultValue) {
        float storedValue = defaultValue;
        if (preferences.isKey(name.c_str())) {
            // Load the value from NVS (either default or stored)
            storedValue = preferences.getFloat(name.c_str(), defaultValue);
        }
        configParams[name] = ConfigParameter(name, storedValue);
        saveParameter(name);
    }

    String getParamString(const String& name) {
        return configParams[name].getStringValue();
    }

    float getParamFloat(const String& name) {
        return configParams[name].getFloatValue();
    }

    void setParam(const String& name, const String& value) {
        if (configParams[name].getType() == ConfigParameter::STRING) {
            configParams[name].setValue(value);
            saveParameter(name);  // Save modified parameter to NVS
        }
    }

    void setParam(const String& name, float value) {
        if (configParams[name].getType() == ConfigParameter::FLOAT) {
            configParams[name].setValue(value);
            saveParameter(name);  // Save modified parameter to NVS
        }
    }

    // New method to set the dynamic title
    void setTitle(const String& newTitle) {
        title = newTitle;
    }

    // Method to set custom HTML content
    void setCustomHTML(const String& htmlContent) {
        customHTML = htmlContent;
    }

    String getHtmlButton(const String& buttonName) {
        // Return HTML code for a button that sends a request to '/button' with the button's name as a parameter
        return "<button onclick=\"window.location.href='/button?name=" + buttonName + "'\">" + buttonName + "</button>";
    }

    void onWebButtonPressed(std::function<void(String)> callback) {
        webButtonCallback = callback;
    }

    void onPropertiesModified(std::function<void(void)> callback) {
        propertiesModifiedCallback = callback;
    }

    // Uncomment this method to clear all stored parameters and reset
    /*
    void resetParameters() {
        preferences.clear();  // Clear all stored preferences
    }
    */

private:
    const char* softAP_ssid;
    const char* softAP_password;
    IPAddress apIP = IPAddress(8, 8, 8, 8); // Access Point IP Address
    IPAddress netMsk = IPAddress(255, 255, 255, 0); // Netmask
    const byte DNS_PORT = 53;
    DNSServer dnsServer;
    WebServer server;
    String title;  // Dynamic title for the configuration page
    String customHTML; // Custom HTML content to include in the webpage
    std::function<void(String)> webButtonCallback;
    std::function<void(void)> propertiesModifiedCallback; // Callback for property modifications

    Preferences preferences;  // NVS Preferences for storing parameters

    std::map<String, ConfigParameter> configParams; // Configuration storage

    void configureAccessPoint() {
        WiFi.softAPConfig(apIP, apIP, netMsk);
        WiFi.softAP(softAP_ssid, softAP_password);
        delay(1000);
        Serial.print("AP IP address: ");
        Serial.println(WiFi.softAPIP());
    }

    void setupDNS() {
        dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
        dnsServer.start(DNS_PORT, "*", apIP);
    }

    void setupWebServer() {
        server.on("/", [this]() { handleRoot(); });
        server.on("/generate_204", [this]() { handleRoot(); }); // Handle Android captive portal request
        server.on("/submit", [this]() { handleSubmit(); });     // Form submission
        server.on("/button", [this]() { handleButton(); });     // Button click handler
        server.onNotFound([this]() { handleNotFound(); });
        server.begin();
        Serial.println("HTTP server started");
    }

    // Save parameter to NVS
    void saveParameter(const String& paramName) {
        ConfigParameter& param = configParams[paramName];
        if (param.getType() == ConfigParameter::STRING) {
            preferences.putString(paramName.c_str(), param.getStringValue());
        } else if (param.getType() == ConfigParameter::FLOAT) {
            preferences.putFloat(paramName.c_str(), param.getFloatValue());
        }
    }

    void loadParameters() {
        for (auto& param : configParams) {
            String paramName = param.first;
            if (param.second.getType() == ConfigParameter::STRING) {
                String value = preferences.getString(paramName.c_str(), param.second.getStringValue());
                param.second.setValue(value);
            } else if (param.second.getType() == ConfigParameter::FLOAT) {
                float value = preferences.getFloat(paramName.c_str(), param.second.getFloatValue());
                param.second.setValue(value);
            }
        }
    }

    void handleButton() {
        if (server.hasArg("name")) {
            String buttonName = server.arg("name");
            // Call the webButtonCallback if it's set
            if (webButtonCallback) {
                webButtonCallback(buttonName);
            }
            // Optionally, redirect back to the main page
            server.send(200, "text/html", "<html><body><script>window.location.href = '/';</script></body></html>");
        } else {
            server.send(400, "text/plain", "Bad Request: Missing 'name' parameter");
        }
    }

    void handleRoot() {
        if (captivePortal()) {
            return;
        }
        server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
        server.sendHeader("Pragma", "no-cache");
        server.sendHeader("Expires", "-1");

        // Create an HTML page with a dynamic title and tabs for each group
        String p = F("<html><head>"
                    "<style>"
                    "body {"
                    "  margin: 0;"
                    "  font-family: Arial, sans-serif;"
                    "  background-color: #f0f0f0;"
                    "  height: 100vh;"
                    "  overflow-x: hidden;" /* Prevent horizontal scroll */
                    "}"
                    ".header {"
                    "  width: 100%;"
                    "  background-color: #fff;"
                    "  padding: 10px 0;"
                    "  position: sticky;" /* Keep the header at the top when scrolling */ 
                    "  top: 0;"
                    "  z-index: 1000;"
                    "  box-shadow: 0 2px 4px rgba(0,0,0,0.1);"
                    "  text-align: center;"
                    "}"
                    ".header h1 {"
                    "  font-size: 5em;"
                    "  color: #333;"
                    "  margin: 10px 0;"
                    "  -webkit-text-stroke: 3px transparent;"
                    "  text-shadow: 0 0 12px rgba(0, 0, 0, 0.5);"
                    "  animation: textOutlineAnimation 3s infinite ease-in-out;"
                    "}"
                    "@keyframes textOutlineAnimation {"
                    "  0%, 100% { -webkit-text-stroke: 2px transparent; text-shadow: 0 0 6px rgba(0, 0, 0, 0.5); }"
                    "  50% { -webkit-text-stroke: 2px #4CAF50; text-shadow: none; }"
                    "}"
                    ".svg-container {"
                    "  width: 100%;"
                    "  display: flex;"
                    "  justify-content: center;"
                    "  margin-bottom: 10px;"
                    "  padding: 15;"
                    "}"
                    "svg {"
                    "  width: 60%;"
                    "  max-width: 1080px;"
                    "}"
                    ".svg-outline {"
                    "  fill: none;"
                    "  stroke: black;"
                    "  stroke-width: 2;"
                    "  stroke-dasharray: 10, 5;"
                    "  animation: dash 5s linear infinite;"
                    "}"
                    "@keyframes dash {"
                    "  to { stroke-dashoffset: -50; }"
                    "}"
                    ".tab-container {"
                    "  width: 100%;"
                    "  display: flex;"
                    "  justify-content: center;"
                    "  margin-top: 20px;"
                    "}"
                    "ul {"
                    "  list-style-type: none;"
                    "  padding: 0;"
                    "  margin: 0;"
                    "  width: 80%;" /* Full width of the tab container */
                    "  display: flex;"
                    "  justify-content: center;" /* Center the tabs */
                    "  overflow-x: auto;" /* Allow horizontal scrolling for smaller screens */ 
                    "}"
                    "li {"
                    "  flex: 1;"
                    "  text-align: center;"
                    "  margin-right: 10px;"
                    "}"
                    "a {"
                    "  font-size: 2em;"
                    "  text-decoration: none;"
                    "  color: #333;"
                    "  padding: 10px;"
                    "  background-color: #f0f0f0;"
                    "  border: 1px solid #ccc;"
                    "  border-radius: 5px;"
                    "  display: block;"
                    "  width: 100%;"
                    "  box-sizing: border-box;"
                    "}"
                    "a:hover {"
                    "  background-color: #ddd;"
                    "}"
                    ".tab-content {"
                    "  display: none;"
                    "  width: 80%;"
                    "  padding: 0px;"
                    "  margin: 20px auto;"
                    "}"
                    ".active-tab {"
                    "  display: block;"
                    "}"
                    "form {"
                    "  background: white;"
                    "  padding: 20px;"
                    "  border-radius: 10px;"
                    "  box-shadow: 0 4px 8px rgba(0,0,0,0.1);"
                    "  width: 100%;"
                    "  box-sizing: border-box;"
                    "  margin: 0 auto;"
                    "}"
                    "label, input {"
                    "  display: block;"
                    "  width: 100%;"
                    "  margin-bottom: 3px;"
                    "  font-size: 3em;"
                    "  font-weight: bold;"
                    "}"
                    "input {"
                    "  padding: 10px;"
                    "  border: 1px solid #ccc;"
                    "  border-radius: 5px;"
                    "  font-size: 3em;"
                    "  box-sizing: border-box;"
                    "}"
                    "input[type='submit'] {"
                    "  background-color: #333333;"
                    "  color: white;"
                    "  border: none;"
                    "  cursor: pointer;"
                    "  padding: 15px;"
                    "  transition: background-color 0.3s ease;"
                    "  font-size: 3em;"
                    "}"
                    "input[type='submit']:hover {"
                    "  background-color: #45a049;"
                    "}"
                    "</style>"
                    
                    // JavaScript to handle the tab switching
                    "<script>"
                    "function openTab(tabName) {"
                    "  var i, tabcontent;"
                    "  tabcontent = document.getElementsByClassName('tab-content');"
                    "  for (i = 0; i < tabcontent.length; i++) {"
                    "    tabcontent[i].style.display = 'none';"
                    "  }"
                    "  document.getElementById(tabName).style.display = 'block';"
                    "}"
                    "</script>"
                    
                    "</head><body>");

        // Insert SVG animation and title in a fixed header container
        p += "<div class='header'><div class='svg-container'>";
        p += "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 150 126\">";
        p += "<g transform=\"translate(-28.34617, -67.34671)\">";
        p += "<path class=\"svg-outline\" d=\"M46.648479 131.26477v13.51339h-0.003v17.82217H159.91391V144.77816H64.562629V131.26477ZM126.77385 99.98706h18.61959v18.63263h-18.61959zm-62.580031 0h18.61959v18.63263H64.193819ZM28.346749 67.346711c-0.002 41.819719 0.002 84.474009 0 126.000059h0.0486 149.900931V72.846631h0.0501l-0.0501 -5.49992zm5.49992 5.49992H172.79633V187.84685H33.846669Z\" />";
        p += "</g></svg></div>";
        p += "<h1>" + title + "</h1></div>";

        // Collect group names and parameters
        std::map<String, std::vector<String>> groupedParams;
        std::vector<String> noGroupParams;

        // Organize parameters by group or no group
        for (const auto& param : configParams) {
            String paramName = param.first;
            int underscoreIndex = paramName.indexOf('_');
            if (underscoreIndex != -1) {
                // Grouped parameter (group_name_parameter_name)
                String groupName = paramName.substring(0, underscoreIndex);
                String subParamName = paramName.substring(underscoreIndex + 1);
                groupedParams[groupName].push_back(subParamName);
            } else {
                // Parameter without a group
                noGroupParams.push_back(paramName);
            }
        }

        // Display the tabs for each group and the Home tab for non-grouped parameters
        p += "<div class='tab-container'><ul>";
        p += "<li><a onclick=\"openTab('home')\">Home</a></li>";
        for (const auto& group : groupedParams) {
            p += "<li><a onclick=\"openTab('" + group.first + "')\">" + group.first + "</a></li>";
        }
        p += "</ul></div>";

        // Display non-grouped parameters (Home Tab)
        p += "<div id='home' class='tab-content active-tab'><form action=\"/submit\" method=\"POST\">";
        if (!noGroupParams.empty()) {
            for (const String& paramName : noGroupParams) {
                const ConfigParameter& param = configParams[paramName];
                p += "<label for='" + paramName + "'>" + paramName + ":</label>";
                if (param.getType() == ConfigParameter::STRING) {
                    p += "<input type='text' name='" + paramName + "' value='" + param.getStringValue() + "'><br>";
                } else if (param.getType() == ConfigParameter::FLOAT) {
                    p += "<input type='number' step='any' name='" + paramName + "' value='" + String(param.getFloatValue()) + "'><br>";
                }
            }
            p += "<input type='submit' value='Submit'>";
        } else {
            p += "<p>No parameters available on this page.</p>";
        }
        p += "</form></div>";

        // Display grouped parameters (Each group in its own tab)
        for (const auto& group : groupedParams) {
            p += "<div id='" + group.first + "' class='tab-content'><form action=\"/submit\" method=\"POST\">";
            for (const String& subParamName : group.second) {
                String fullParamName = group.first + "_" + subParamName;
                const ConfigParameter& param = configParams[fullParamName];
                p += "<label for='" + fullParamName + "'>" + subParamName + ":</label>";
                if (param.getType() == ConfigParameter::STRING) {
                    p += "<input type='text' name='" + fullParamName + "' value='" + param.getStringValue() + "'><br>";
                } else if (param.getType() == ConfigParameter::FLOAT) {
                    p += "<input type='number' step='any' name='" + fullParamName + "' value='" + String(param.getFloatValue()) + "'><br>";
                }
            }
            p += "<input type='submit' value='Submit'></form></div>";
        }

        // At the end of the generated content, insert the custom HTML
        p += customHTML;

        p += "</body></html>";

        server.send(200, "text/html", p);
    }

    void handleSubmit() {
        for (const auto& param : configParams) {
            if (server.hasArg(param.first)) {
                if (param.second.getType() == ConfigParameter::STRING) {
                    setParam(param.first, server.arg(param.first));
                } else if (param.second.getType() == ConfigParameter::FLOAT) {
                    setParam(param.first, server.arg(param.first).toFloat());
                }
            }
        }

        // Call the propertiesModifiedCallback if it's set
        if (propertiesModifiedCallback) {
            propertiesModifiedCallback();
        }

        // Instead of showing a separate page, reload the current page after submission
        server.send(200, "text/html", "<html><body><script>window.location.href = '/';</script></body></html>");
    }

    void handleNotFound() {
        if (captivePortal()) {
            return;
        }
        String message = "404 Not Found\n\n";
        message += "URI: ";
        message += server.uri();
        message += "\nMethod: ";
        message += (server.method() == HTTP_GET) ? "GET" : "POST";
        message += "\nArguments: ";
        message += server.args();
        message += "\n";
        for (uint8_t i = 0; i < server.args(); i++) {
            message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
        }
        server.send(404, "text/plain", message);
    }

    boolean captivePortal() {
        if (!isIp(server.hostHeader())) {
            Serial.println("Request redirected to captive portal");
            server.sendHeader("Location", String("http://") + toStringIp(server.client().localIP()), true);
            server.send(302, "text/plain", "");
            server.client().stop();
            return true;
        }
        return false;
    }

    bool isIp(String str) {
        for (size_t i = 0; i < str.length(); i++) {
            int c = str.charAt(i);
            if ((c != '.') && (c < '0' || c > '9')) {
                return false;
            }
        }
        return true;
    }

    String toStringIp(IPAddress ip) {
        return String(ip[0]) + "." + String(ip[1]) + "." + String(ip[2]) + "." + String(ip[3]);
    }
};

class ButtonHandler {
public:
    // Method to add a button with a name and pin number
    void addButton(const String& name, uint8_t pin) {
        // Configure the pin as input with internal pull-up
        pinMode(pin, INPUT_PULLUP);
        ButtonState state;
        state.pin = pin;
        state.lastStableState = digitalRead(pin) == LOW;
        state.lastReading = state.lastStableState;
        state.lastDebounceTime = millis();
        buttons[name] = state;
    }

    // Method to set the button state changed callback
    void onButtonStateChanged(std::function<void(String, bool)> callback) {
        buttonCallback = callback;
    }

    // Method to update the button states; should be called in the loop()
    void update() {
        unsigned long currentTime = millis();
        for (auto& pair : buttons) {
            const String& name = pair.first;
            ButtonState& state = pair.second;
            bool reading = digitalRead(state.pin) == LOW; // Buttons are active LOW due to pull-up

            if (reading != state.lastReading) {
                // reset the debouncing timer
                state.lastDebounceTime = currentTime;
            }

            if ((currentTime - state.lastDebounceTime) > debounceDelay) {
                // whatever the reading is at, it's been there for longer than the debounce delay
                // so take it as the actual current state

                if (reading != state.lastStableState) {
                    state.lastStableState = reading;

                    // Button state changed, call the callback
                    if (buttonCallback) {
                        buttonCallback(name, reading); // Pass the name and the new state (pressed/released)
                    }
                }
            }

            state.lastReading = reading;
        }
    }

    // Method to get the current state of a button
    bool isButtonPressed(const String& name) {
        if (buttons.find(name) != buttons.end()) {
            return buttons[name].lastStableState;
        }
        return false; // Button not found
    }

private:
    struct ButtonState {
        uint8_t pin;
        bool lastStableState; // The last stable state
        bool lastReading;     // The last reading from the pin
        unsigned long lastDebounceTime;
    };
    std::map<String, ButtonState> buttons;
    std::function<void(String, bool)> buttonCallback;
    const unsigned long debounceDelay = 50; // Debounce delay in milliseconds
};

// Global WebConfig object
WebConfig webConfig("CJ_FB", "Flash1234");
RunningDot dot;
ButtonHandler buttonHandler;

// Called when a button on the website is pressed
void handleWebButtonPressed(String name) {
    Serial.println("Web Button pressed: " + name);
    if (name == "DoSomething") {
        Serial.println("Performing 'DoSomething' action.");
        // Add your action code here
        dot.trigger(); // Example action
    } else if (name == "AnotherAction") {
        Serial.println("Performing 'AnotherAction' action.");
        // Add your action code here
        dot.setColor(CRGB::Blue);
    }
}

void handlePropertiesModified() {
    Serial.println("Properties were modified.");
    // Update dot parameters based on the new configuration
    dot.setBrightness(webConfig.getParamFloat("Brightness"));
    dot.setSpeed(webConfig.getParamFloat("Speed"));
    dot.setWidth(webConfig.getParamFloat("Width"));
    // Update the color if color parameters have changed
    float red = webConfig.getParamFloat("Color_Red");
    float green = webConfig.getParamFloat("Color_Green");
    float blue = webConfig.getParamFloat("Color_Blue");
    dot.setColor(CRGB((uint8_t)red, (uint8_t)green, (uint8_t)blue));
}

void handleButtonPressed(String name, bool pressed) {
    Serial.print("Button ");
    Serial.print(name);
    Serial.print(" ");
    Serial.println(pressed ? "pressed" : "released");
    if (name == "Trigger") {
        if (pressed) {
            // Handle the action for the 'Trigger' button pressed
            Serial.println("Trigger button pressed.");
            dot.trigger();
        } else {
            // Handle the action for the 'Trigger' button released
            Serial.println("Trigger button released.");
            // Add any action needed on button release
        }
    }
    // Add cases for other buttons if needed
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("Configuring access point...");
    
    // Set dynamic title for the configuration page
    webConfig.setTitle("ESP32 Device Configuration");

    // Add configuration parameters
    webConfig.addParamFloat("Color_Red", 255.0);
    webConfig.addParamFloat("Color_Green", 255.0);
    webConfig.addParamFloat("Color_Blue", 255.0);
    webConfig.addParamFloat("Speed", 30);
    webConfig.addParamFloat("Brightness", 30);
    webConfig.addParamFloat("Width", 30);

    // Set the web button pressed callback
    webConfig.onWebButtonPressed(handleWebButtonPressed);

    // Set the properties modified callback
    webConfig.onPropertiesModified(handlePropertiesModified);
    
    webConfig.begin(); // Start the AP and web server

    // Initialize dot with initial parameters
    dot.setBrightness(webConfig.getParamFloat("Brightness"));
    dot.setSpeed(webConfig.getParamFloat("Speed"));
    float red = webConfig.getParamFloat("Color_Red");
    float green = webConfig.getParamFloat("Color_Green");
    float blue = webConfig.getParamFloat("Color_Blue");
    dot.setColor(CRGB((uint8_t)red, (uint8_t)green, (uint8_t)blue));
    dot.setWidth(webConfig.getParamFloat("Width"));
    dot.begin();

    // Initialize button handler and add buttons
    buttonHandler.addButton("Trigger", BUTTON_PIN);

    // Set the button state changed callback
    buttonHandler.onButtonStateChanged(handleButtonPressed);

    // Optionally, call handlePropertiesModified to initialize any dependent states
    handlePropertiesModified();
}

void loop() {
    webConfig.handleClient(); // Handle client requests
    buttonHandler.update();   // Update button states and handle presses
    dot.update();
}
