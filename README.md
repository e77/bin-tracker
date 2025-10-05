# Waveshare ESP32-S3 1.28" Bin Day Tracker

Using one of these: https://www.waveshare.com/wiki/ESP32-S3-LCD-1.28#Interfaces

It has some quirks with the library version it needs. 

1) host the JSON in your own git repo and plug the dates/waste streams/bin colours into your own file.
2) Get the Raw URL from git - repo must be public. 
3) Plug your wireless SSID and password, and URL in and flash it!


## Bin Type Colour Mapping

| Bin Type   | Colour |
|------------|--------|
| recycling  | blue   |
| rubbish    | black  |
| food       | green  |
| garden     | brown  |
