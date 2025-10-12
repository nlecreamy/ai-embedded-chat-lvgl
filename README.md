# AI-Embedded-Chat-LVGL

![Project Banner](https://via.placeholder.com/1200x300?text=AI+Embedded+Chat+with+LVGL) <!-- Replace with actual banner image if available -->

## Overview

AI-Embedded-Chat-LVGL is a lightweight, open-source chat application designed for embedded systems, specifically targeting ESP32 microcontrollers. It provides a user-friendly touchscreen interface powered by LVGL (Light and Versatile Graphics Library) and integrates with AI APIs such as Google Gemini, Grok, or OpenAI for real-time conversational capabilities. This project is ideal for building IoT smart assistants, interactive displays, or edge AI devices.

The application leverages PlatformIO for build management and ESP-IDF (Espressif IoT Development Framework) as the underlying SDK, ensuring efficient performance on resource-constrained hardware. Key highlights include support for low-power optimizations.

**Note:** This project is currently under active development. Contributions are welcome!

## Features

- **Touchscreen UI with LVGL**: Intuitive graphical interface for chatting, including text input, message display, and customizable themes.
- **AI API Integration**: Real-time chatting with AI models via API keys. Primarily focused on Google Gemini, with support for Grok and OpenAI.
- **Low-Power Operation**: Optimized for battery-powered devices with sleep modes and efficient networking.
- **ESP32 Compatibility**: Tested on popular ESP32 boards with TFT displays (e.g., ESP32-S3 with ILI9341 or similar).
- **PlatformIO Build System**: Simplified development, flashing, and debugging workflow.

## Requirements

### Hardware
- ESP32 microcontroller (e.g., ESP32-WROOM, ESP32-S3) with at least 4MB Flash and PSRAM recommended.
- Touchscreen display compatible with LVGL (e.g., 2.8" ILI9341 TFT with XPT2046 touch controller).
- Wi-Fi connectivity for API calls.

### Software
- [PlatformIO](https://platformio.org/) IDE (recommended for VS Code).
- ESP-IDF v5.x or later.
- API key from Google Gemini (or alternative providers like Grok/OpenAI).
- Dependencies (automatically handled by PlatformIO):
  - LVGL library (v9.x or later).
  - ESP-IDF components for Wi-Fi, HTTP client, and JSON parsing.
  - Additional libraries: cJSON for JSON handling, if not included in ESP-IDF.

## Installation

1. **Clone the Repository**:
   ```
   git clone https://github.com/Itsmeakash248/ai-embedded-chat-lvgl.git
   cd ai-embedded-chat-lvgl
   ```

2. **Set Up PlatformIO**:
   - Install PlatformIO extension in VS Code if not already done.
   - Open the project folder in VS Code.
   - PlatformIO will automatically detect the `platformio.ini` file and resolve dependencies.

3. **Configure API Key**:
   - Create a `credentials.h` file in the `src/` directory (or edit if it exists).
   - Add your API key and endpoint:
     ```cpp
     #define AI_API_KEY "your_gemini_api_key_here"
     #define AI_API_ENDPOINT "https://generativelanguage.googleapis.com/v1beta/models/gemini-flash-latest:streamGenerateContent?alt=sse&key=%s" // For Gemini
     ```
     - For other providers, adjust the endpoint accordingly (e.g., for OpenAI: `"https://api.openai.com/v1/chat/completions"`).

4. **Build and Upload**:
   - Connect your ESP32 board via USB.
   - In PlatformIO, run:
     - Build: `pio run`
     - Upload: `pio run -t upload`
     - Monitor serial output: `pio device monitor`

## Usage

1. **Power On the Device**:
   - Flash the firmware to your ESP32.
   - The LVGL UI should boot up on the touchscreen, displaying a chat interface.

2. **Connect to Wi-Fi**:
   - The app includes a Wi-Fi manager.

3. **Start Chatting**:
   - Type messages using the on-screen keyboard.
   - Send queries to the AI API; responses will be displayed in real-time.

4. **Customization**:
   - Modify LVGL styles in `src/ui.c` for UI tweaks.
   - Extend AI features by editing `src/ai_chat.c`.

### Example Interaction
- User: "What's the weather like today?"
- AI (Gemini): "Based on your location, it's sunny with a high of 75°F."

## Configuration

- **Switching AI Providers**: Update `AI_API_ENDPOINT` and adjust the request payload in the source code to match the provider's API format.
- **Debugging**: Enable verbose logging in ESP-IDF config for troubleshooting.
- **Power Saving**: Configure sleep intervals in `src/main.cpp`.

## Contributing

We welcome contributions! To get started:
1. Fork the repository.
2. Create a feature branch: `git checkout -b feature/new-feature`.
3. Commit your changes: `git commit -m 'Add new feature'`.
4. Push to the branch: `git push origin feature/new-feature`.
5. Open a Pull Request.

Please follow the [Code of Conduct](CODE_OF_CONDUCT.md) and ensure code adheres to ESP-IDF and LVGL best practices.

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Acknowledgments

- [LVGL](https://lvgl.io/) for the graphics library.
- [Espressif ESP-IDF](https://github.com/espressif/esp-idf) for the framework.
- Google Gemini API for AI capabilities.

For questions or issues, open a GitHub issue or contact the maintainer at [itsmeakash248@gmail.com](mailto:itsmeakash248@gmail.com).