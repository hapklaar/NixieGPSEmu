Solution to make an ESP32 (TTGO T-Display in this case) emit GPS NMEA serial messages to a GPIO of your choosing. I created this to be able to keep the time correct on my PV Electronics Nixie clocks that have a connection for a GPS module with stereo jack plug.

The module is powered by the Nixie clock through the cable with jack plug

1. Configure GPIO port in sketch
2. Flash sketch with PlatformIO
3. Solder a cable with stereo jack plug to the module. Normally white will be +5V, red will be GPS Serial. Jack plug is wired like GND -- GPS Serial -- +5V (  ===|==|==>  )
4. Connect jack plug to Nixie clock
