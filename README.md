# Tonuiono music and game system
This is a Tonuino inspired project with an added Game functionality
Hardware:
Board/chip: ESP32-S3 N16R8 (PSRAM)
RC522 (SPI) + SD (SPI shared)
MAX98357 (connected to 3V3) (I2S) --> Will be replaced with a UDA1334A and a PAM8406 (only one channel used) on 5V in the final setup

Game logic and all music cards are defined in settings.json

## General info
All music and the settings file, *settings.json* is placed on the SD-card

### The folder structure are as follows: ###
-
  settings.json
  audio
    songInRoot1.mp3
    ...
    songInRootn.mp3
    games
       common
         (files common for several games)
       game-x
         (files for game-x) 
       game-x1
         (files for game-x1) 
    album1
       m1.mp3
       ...
       mx.mp3

# Rules implemented by the system


## Music
- Tracks, albums or playlists are selected with RFID cards
- An album is defined as everything placed in a folder, in the audio-folder, and is defined for the RFID card as the name of that folder
- A playlist is defined in settings.json and can include individual mp3-files from everywhere in the audio-folder
- When in game-mode, either the game has to be completed, or the music button must be pressed to enter music mode

## Game
- Games are defined and SELECTED with a game card defined via a card with "role": "game_selector"
- The general structure of a game is that a question must be answered by selecting one, or two, RFID cards defined with the role "answer"
- All game related details (questions, name, etc) are defined in the games-list in settings.json. See this file for details
- If a question is not answered before the time defined in "answerTimeoutMs" the question will be repeated, but only the number of times defined in "maxRepeat" If not answered before a prompt will play that the games has been canceled and the system goes back to music mode
- For multicard questions a prompt will play something like "please select next card for question" and a specific prompt if not selected before "nextCardRepeatMs" will repeat this message. This however only maxRepeat-1 times

