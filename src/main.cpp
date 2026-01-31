#include <Preferences.h>
#include <SD.h>
#include <SPI.h>

#include <ArduinoJson.h>
#include <MFRC522.h>
// Display
#include <U8g2lib.h>
#include <Wire.h>

// ESP8266Audio
#include "AudioFileSourceSD.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

// Display
static constexpr uint8_t PIN_OLED_SDA = 42;
static constexpr uint8_t PIN_OLED_SCL = 48;

// SD (Shared SPI)
static constexpr uint8_t PIN_SCK = 14;
static constexpr uint8_t PIN_MOSI = 15;
static constexpr uint8_t PIN_MISO = 16;
static constexpr uint8_t PIN_SD_CS = 10;

// RC522 (Shared SPI, own CS)
static constexpr uint8_t PIN_RC522_CS = 21; // RC522 SDA/SS
static constexpr uint8_t PIN_RC522_RST = 4;

// UDA1334 (I2S)
static constexpr int PIN_I2S_BCLK = 6;
static constexpr int PIN_I2S_WSEL = 7;
static constexpr int PIN_I2S_DIN = 5;

// Buttons
static constexpr uint8_t PIN_BTN_FORWARD = 47;
static constexpr uint8_t PIN_BTN_BACK = 17;
static constexpr uint8_t PIN_BTN_VOL_UP = 18;
static constexpr uint8_t PIN_BTN_VOL_DOWN = 39;
static constexpr uint8_t PIN_BTN_MODE_MUS = 40;
static constexpr uint8_t PIN_BTN_PLAY = 41;

// LED for RFID Card Detection
static constexpr uint8_t PIN_LED_CARD = 2;

// ---------------- Volume control ----------------
static Preferences prefs;
static constexpr const char *PREF_NS = "player";
static constexpr const char *PREF_KEY_VOL = "vol_x100";

static bool volDirty = false;
static uint32_t volLastChangedAt = 0;
static constexpr uint32_t VOL_SAVE_DELAY_MS = 800;

static float currentVolume = 0.4f; // start-volumen
static constexpr float VOL_STEP = 0.05f;
static constexpr float VOL_MIN = 0.05f;
static constexpr float VOL_MAX = 0.9f;

// Save meta data for
#include <string>
#include <unordered_map>
struct TrackMeta {
  String title;
  String artist;
};
static std::unordered_map<std::string, TrackMeta> trackMetaByPath;

static std::unordered_map<std::string, String> albumTitleByFolder;

static inline std::string keyOfPath(const String &p) {
  return std::string(p.c_str());
}

static String normalizeFolder(String f) {
  f.trim();
  if (!f.startsWith("/"))
    f = "/" + f;
  while (f.indexOf("//") >= 0)
    f.replace("//", "/");
  if (f.length() > 1 && f.endsWith("/"))
    f.remove(f.length() - 1);
  return f;
}

static String dirnameOf(const String &fullPath) {
  int slash = fullPath.lastIndexOf('/');
  if (slash <= 0)
    return "/";
  return fullPath.substring(0, slash);
}

static String lookupAlbumTitleForTrackPath(const String &trackPath) {
  String folder = normalizeFolder(dirnameOf(trackPath));
  auto it = albumTitleByFolder.find(keyOfPath(folder));
  if (it != albumTitleByFolder.end())
    return it->second;
  return "";
}

// Handle last track for play/pause
static constexpr const char *PREF_KEY_LASTPATH = "last_path";
static char lastPath[128] = {0}; // RAM copy
static bool hasLastPath = false;

static constexpr size_t MAX_ACTIVE = 300;
static String activeTracks[MAX_ACTIVE];
static size_t activeCount = 0;
static int activeIndex = -1;

// Variables for OLED display
static String oledLine1; // Mode
static String oledLine2; // song titel or game titel
static String oledLine3; // Only music

static String activeTitle[MAX_ACTIVE];
static String activeArtist[MAX_ACTIVE];
static String activePlaylistTitle; // til linje 2

enum Action {
  ACT_PLAY_PAUSE,
  ACT_NEXT,
  ACT_PREV,
  ACT_VOL_UP,
  ACT_VOL_DOWN,
  ACT_MODE_MUSIC,
  ACT_MODE_GAME_A,
  ACT_MODE_GAME_B
};

struct Button {
  uint8_t pin;
  Action action;
  bool lastState;
  uint32_t lastChange;
  // For long-press repeat
  uint32_t pressedAt;
  uint32_t lastRepeat;
};

static constexpr uint32_t DEBOUNCE_MS = 30;

Button buttons[] = {
    {PIN_BTN_FORWARD, ACT_NEXT, HIGH, 0, 0, 0},
    {PIN_BTN_PLAY, ACT_PLAY_PAUSE, HIGH, 0, 0, 0},
    {PIN_BTN_BACK, ACT_PREV, HIGH, 0, 0, 0},
    {PIN_BTN_VOL_UP, ACT_VOL_UP, HIGH, 0, 0, 0},
    {PIN_BTN_VOL_DOWN, ACT_VOL_DOWN, HIGH, 0, 0, 0},
    {PIN_BTN_MODE_MUS, ACT_MODE_MUSIC, HIGH, 0, 0, 0}
    // {PIN_BTN_MODE_GA, ACT_MODE_GAME_A, HIGH, 0, 0, 0},
    // {PIN_BTN_MODE_GB, ACT_MODE_GAME_B, HIGH, 0, 0, 0},
};

static constexpr size_t BUTTON_COUNT = sizeof(buttons) / sizeof(buttons[0]);

static constexpr uint32_t LONGPRESS_START_MS = 350; // When repeat starts
static constexpr uint32_t LONGPRESS_REPEAT_MS = 80; // repeat-interval

// LED BLINKING START

static bool ledBlinkActive = false;
static uint32_t ledBlinkUntil = 0;
static uint32_t ledBlinkNextToggle = 0;
static bool ledBlinkLevel = false;

// start blink i durationMs
static void ledStartBlink(uint32_t now, uint32_t durationMs,
                          uint32_t periodMs = 150) {
  ledBlinkActive = true;
  ledBlinkUntil = now + durationMs;
  ledBlinkNextToggle = now; // toggle med det samme
  ledBlinkLevel = true;     // start ON
  digitalWrite(PIN_LED_CARD, HIGH);
}

// stop blink og sluk
static void ledStopBlink() {
  ledBlinkActive = false;
  digitalWrite(PIN_LED_CARD, LOW);
}

// skal kaldes hver loop (eller ofte)
static void ledTick(uint32_t now, uint32_t periodMs = 150) {
  if (!ledBlinkActive)
    return;

  if ((int32_t)(now - ledBlinkUntil) >= 0) {
    ledStopBlink();
    return;
  }

  if ((int32_t)(now - ledBlinkNextToggle) >= 0) {
    ledBlinkNextToggle = now + (periodMs / 2);
    ledBlinkLevel = !ledBlinkLevel;
    digitalWrite(PIN_LED_CARD, ledBlinkLevel ? HIGH : LOW);
  }
}

static void ledSetNormal(bool on) {
  if (ledBlinkActive)
    return;
  digitalWrite(PIN_LED_CARD, on ? HIGH : LOW);
}

// LED BLINKING END

// ---------------- RC522 ----------------
MFRC522 mfrc522(PIN_RC522_CS, PIN_RC522_RST);

// ---------------- Audio ----------------
AudioGeneratorMP3 *mp3 = nullptr;
AudioFileSourceSD *file = nullptr;
AudioOutputI2S *out = nullptr;

// Parent Control

struct UiMessages {
  String antiRepeatWarning;
  String antiRepeatEnabled;
  String antiRepeatDisabled;

  String volumeLockOn;
  String volumeLockOff;

  String mastercard_used;

  String musicModeInfo;
};

static UiMessages uiMessages;

static bool parentalAntiRepeatEnabled = false;
static String lastStartedPath = "";
static uint8_t sameTrackStreak =1; // 0,1,2,... (konsekutive gange samme track startes)

static bool volumeLocked = false;
static float lockedVolume = 0.0f;

static bool volumeDirty = false;
static uint8_t lastVolumeChangeAt = 0;


static float getEffectiveVolume() {
  return volumeLocked ? lockedVolume : currentVolume;
}

static float clampf(float v, float lo, float hi) {
  if (v < lo)
    return lo;
  if (v > hi)
    return hi;
  return v;
}

static void setVolume(float v) {
  v = clampf(v, VOL_MIN, VOL_MAX);

  if (volumeLocked) {
    lockedVolume = v;
  } else {
    currentVolume = v;
  }

  // hvis du har noget "volumeDirty" / lastChanged timestamp, sæt det her
  volumeDirty = true;
  lastVolumeChangeAt = millis();
}

static bool antiRepeatBlocksThisStart(const String &path) {
  if (!parentalAntiRepeatEnabled)
    return false;

  // Hvis den samme track startes igen
  if (path == lastStartedPath) {
    // sameTrackStreak: 1 = anden gang, 2 = tredje gang, ...
    if (sameTrackStreak >= 2) {
      return true; // blokér 3. gang (eller mere)
    }
  }
  return false;
}

static void antiRepeatOnTrackStart(const String &path) {
  if (path == lastStartedPath) {
    if (sameTrackStreak < 255)
      sameTrackStreak++;
  } else {
    lastStartedPath = path;
    sameTrackStreak = 1;
  }
}



// End of Parent Control

// Card tracking
enum PlayKind : uint8_t {
  PK_NONE = 0,
  PK_SINGLE,
  PK_ALBUM_FOLDER,
  PK_ALBUM_TRACKS
};

struct TrackItem {
  String title;  // optional
  String artist; // optional
  String file;   // absolute path
};

// A “pool” with all track-items from all album(track-lists)
static constexpr size_t MAX_TRACKPOOL = 600; // Adjust if needed
static TrackItem trackPool[MAX_TRACKPOOL];
static size_t trackPoolCount = 0;

// Tune these to your needs / memory budget
static constexpr uint8_t MAX_CARD_TAGS = 8;

struct CardEntry {
  // Common
  String uid;  // uppercase hex
  String role; // "music", "answer", "game_selector"
  String title;
  String artist;

  // ---------------- MUSIC ----------------
  PlayKind kind = PK_NONE;

  // single
  String file;

  // album-folder
  String folder;

  // album/playlist tracks refer into trackPool
  uint16_t trackStart = 0;
  uint16_t trackCount = 0;

  // ---------------- GAME SELECTOR ----------------
  // Used when role == "game_selector"
  String gameId;

  // ---------------- ANSWER CARD ----------------
  // Used when role == "answer"
  String tags[MAX_CARD_TAGS];
  uint8_t tagCount = 0;

  // Optional numeric value for sum games (role=="answer" with tag "tal" etc.)
  // Use -1 when not present.
  int value = -1;

  // ---------------- PARENT / ACTION ----------------
  String action;
};

static constexpr size_t MAX_CARDS = 200;
CardEntry cards[MAX_CARDS];
size_t cardCount = 0;

// End of Card tracking

static void clearActivePlaylist() {
  activeCount = 0;
  activeIndex = -1;
}

// Read Song title and artist from file name
static void parseMetaFromFilename(const String &path, String &titleOut,
                                  String &artistOut) {
  titleOut = "";
  artistOut = "";

  // basename
  int slash = path.lastIndexOf('/');
  String base = (slash >= 0) ? path.substring(slash + 1) : path;

  // strip extension
  int dot = base.lastIndexOf('.');
  if (dot > 0)
    base = base.substring(0, dot);

  // split by ##
  int sep = base.indexOf("##");
  String t = (sep >= 0) ? base.substring(0, sep) : base;
  String a = (sep >= 0) ? base.substring(sep + 2) : "";

  t.replace('_', ' ');
  a.replace('_', ' ');
  t.trim();
  a.trim();

  titleOut = t;
  artistOut = a;
}

static void uiSetNowPlayingFromPath(const String &path) {
  // DEBUG - Remove Me
  Serial.print("UI path:   ");
  Serial.println(path);
  // 1) JSON meta først
  auto it = trackMetaByPath.find(keyOfPath(path));
  String t, a;
  if (it != trackMetaByPath.end()) {
    t = it->second.title;
    a = it->second.artist;
  } else {
    // 2) fallback: filnavn-konvention for album-folder
    parseMetaFromFilename(path, t, a);
  }

  if (t.length() > 0 && a.length() > 0)
    oledLine3 = t + " - " + a;
  else if (t.length() > 0)
    oledLine3 = t;
  else if (a.length() > 0)
    oledLine3 = a;
  else
    oledLine3 = "";
}

// End of Read Song title and artist from file name

// Sort alphabetically (used for folder scanning only)
static void sortActivePlaylist() {
  for (size_t i = 0; i + 1 < activeCount; i++) {
    for (size_t j = i + 1; j < activeCount; j++) {
      if (activeTracks[j] < activeTracks[i]) {
        String t = activeTracks[i];
        activeTracks[i] = activeTracks[j];
        activeTracks[j] = t;
      }
    }
  }
}

static bool isMp3File(const String &name) {
  String n = name;
  n.toLowerCase();
  return n.endsWith(".mp3");
}

static void setActiveFromFolder(const String &folder) {
  clearActivePlaylist();

  File dir = SD.open(folder.c_str());
  if (!dir || !dir.isDirectory()) {
    Serial.print("Folder missing/not dir: ");
    Serial.println(folder);
    return;
  }

  for (;;) {
    File f = dir.openNextFile();
    if (!f)
      break;

    if (!f.isDirectory()) {
      String name = String(f.name());
      if (isMp3File(name)) {
        String full = folder;
        if (!full.endsWith("/"))
          full += "/";
        full += name;
        if (!full.startsWith("/"))
          full = "/" + full;

        if (activeCount < MAX_ACTIVE) {
          activeTracks[activeCount++] = full;
        }
      }
    }
    f.close();
  }
  dir.close();

  sortActivePlaylist();

  Serial.print("Active playlist from folder: ");
  Serial.print(folder);
  Serial.print(" count=");
  Serial.println(activeCount);
}

static void setActiveFromTrackPool(uint16_t start, uint16_t count) {
  clearActivePlaylist();
  for (uint16_t i = 0; i < count && activeCount < MAX_ACTIVE; i++) {
    activeTracks[activeCount++] = trackPool[start + i].file;
  }
  Serial.print("Active playlist from tracks list count=");
  Serial.println(activeCount);
}

enum CmdType : uint8_t { CMD_PLAY_FILE, CMD_TOGGLE_PAUSE };

struct AudioCmd {
  CmdType type;
  char path[128]; // Only used when CMD_PLAY_FILE
};

static QueueHandle_t audioQ = nullptr;

// ---------- Helpers ----------

// Track info helpers
static void playPath(const String &path) {

  if (!audioQ) {
    Serial.println("audioQ not ready");
    return;
  }

  AudioCmd c{};
  c.type = CMD_PLAY_FILE;
  strncpy(c.path, path.c_str(), sizeof(c.path) - 1);
  c.path[sizeof(c.path) - 1] = '\0';
  xQueueSend(audioQ, &c, 0);

  // Persist last track
  strncpy(lastPath, c.path, sizeof(lastPath) - 1);
  lastPath[sizeof(lastPath) - 1] = '\0';
  hasLastPath = true;
  prefs.putString(PREF_KEY_LASTPATH, lastPath);

  Serial.print("Play: ");
  Serial.println(path);
}

static void playActiveIndex(int idx) {
  if (activeCount == 0)
    return;

  if (idx < 0)
    idx = 0;
  if (idx >= (int)activeCount)
    idx = (int)activeCount - 1;

  String path = activeTracks[idx];

  // Anti-repeat gate (valgfrit: kun i music mode)
  if (antiRepeatBlocksThisStart(path)) {
    if (uiMessages.antiRepeatWarning.length() > 0) {
      playPath(uiMessages.antiRepeatWarning);
    }
    return;
  }

  // Commit chosen index
  activeIndex = idx;

  // Update streak + UI + play
  antiRepeatOnTrackStart(path);
  uiSetNowPlayingFromPath(path);
  playPath(path);
}

// End of Track info helpers
static String uidToHexUpper(const MFRC522::Uid &u) {
  String s;
  s.reserve(u.size * 2);
  for (byte i = 0; i < u.size; i++) {
    if (u.uidByte[i] < 0x10)
      s += '0';
    s += String(u.uidByte[i], HEX);
  }
  s.toUpperCase();
  return s;
}

static void playTrackDirect(const String& path) {
  if (antiRepeatBlocksThisStart(path)) {
    if (uiMessages.antiRepeatWarning.length() > 0) playPath(uiMessages.antiRepeatWarning);
    return;
  }
  antiRepeatOnTrackStart(path);
  uiSetNowPlayingFromPath(path);
  playPath(path);
}

static const CardEntry *findCardByUid(const String &uid) {
  for (size_t i = 0; i < cardCount; i++) {
    if (cards[i].uid == uid)
      return &cards[i];
  }
  return nullptr;
}

constexpr size_t DOC_SIZE = 120 * 1024;
static bool loadCardsJson(const char *jsonPath) {
  trackPoolCount = 0;
  cardCount = 0;

  Serial.println("****** loadCardsJson **********");

  File f = SD.open(jsonPath, FILE_READ);
  if (!f) {
    Serial.print("Could not open JSON: ");
    Serial.println(jsonPath);
    return false;
  }

  // Increase if JSON grows
  //DynamicJsonDocument doc(16384);
  DynamicJsonDocument doc(DOC_SIZE);

  DeserializationError err = deserializeJson(doc, f);
  f.close();
   Serial.println("loadCardsJson");
  Serial.print("JSON capacity: ");
  Serial.println(doc.capacity());

  Serial.print("JSON memoryUsage: ");
  Serial.println(doc.memoryUsage());

  if (err) {
    Serial.print("JSON parse error: ");
    Serial.println(err.c_str());
    return false;
  }

  JsonObject msgs = doc["messages"].as<JsonObject>();
  if (!msgs.isNull()) {
    uiMessages.antiRepeatWarning = String((const char *)(msgs["anti_repeat_warning"] | ""));
    uiMessages.antiRepeatEnabled = String((const char *)(msgs["anti_repeat_enabled"] | ""));
    uiMessages.antiRepeatDisabled = String((const char *)(msgs["anti_repeat_disabled"] | ""));

    uiMessages.volumeLockOn  = String((const char*)(msgs["volume_lock_on"]  | ""));
    uiMessages.volumeLockOff = String((const char*)(msgs["volume_lock_off"] | ""));

    uiMessages.mastercard_used = String((const char*)(msgs["mastercard_used"] | ""));

    uiMessages.musicModeInfo =   String((const char*)(msgs["music_mode_info"] | ""));

  }

  JsonArray arr = doc["cards"].as<JsonArray>();
  if (arr.isNull()) {
    Serial.println("JSON missing 'cards' array");
    return false;
  }

  for (JsonObject c : arr) {
    if (cardCount >= MAX_CARDS){
      Serial.println("WARNING: MAX_CARDS reached – some cards ignored");
      break;
    }

    const char *uid = c["uid"] | "";
    const char *role = c["role"] | "";
    const char *title = c["title"] | "";
    const char *artist = c["artist"] | "---";
    const char* action = c["action"] | "";

    String suid(uid);
    suid.toUpperCase();
    if (suid.length() == 0)
      continue;

    // -------- common fields --------
    CardEntry &ce = cards[cardCount];
    ce.uid = suid;
    ce.role = String(role);
    ce.title = String(title);
    ce.artist = String(artist);
    ce.action = String(action);


    // -------- defaults (important!) --------
    ce.gameId = "";
    ce.tagCount = 0;
    ce.value = -1;

    ce.kind = PK_NONE;
    ce.file = "";
    ce.folder = "";
    ce.trackStart = 0;
    ce.trackCount = 0;

    // -------- role-specific parsing --------
    if (ce.role == "game_selector") {
      Serial.println("***** game_selector *****");
      const char *gid = c["gameId"] | "";
      ce.gameId = String(gid);


    } else if (ce.role == "answer") {
      // tags[]
      JsonArray tags = c["tags"].as<JsonArray>();
      if (!tags.isNull()) {
        for (JsonVariant tv : tags) {
          if (ce.tagCount >= MAX_CARD_TAGS)
            break;
          if (tv.is<const char *>()) {
            ce.tags[ce.tagCount++] = String(tv.as<const char *>());
          }
        }
      }

      // optional value (for sum games)
      if (c.containsKey("value")) {
        ce.value = (int)(c["value"] | -1);
      }
    }
    // music (and any other roles that have play object)
    // we only parse play for music cards to avoid accidental parsing on other
    // roles
    if (ce.role == "music") {
      JsonObject play = c["play"].as<JsonObject>();
      if (!play.isNull()) {
        const char *kind = play["kind"] | "";

        if (strcmp(kind, "single") == 0) {
          const char *file = play["file"] | "";
          if (strlen(file) > 0) {
            ce.kind = PK_SINGLE;

            // --- normaliser path (samme format overalt) ---
            String path(file);
            if (!path.startsWith("/"))
              path = "/" + path;

            ce.file = path;

            // --- metadata-opslag: path -> {title, artist} ---
            trackMetaByPath[keyOfPath(path)] = TrackMeta{ce.title, ce.artist};
          }
        } else if (strcmp(kind, "album") == 0 ||
                   strcmp(kind, "playlist") == 0) {
          const char *folder = play["folder"] | "";
          JsonArray tracks = play["tracks"].as<JsonArray>();

          if (strlen(folder) > 0) {
            ce.kind = PK_ALBUM_FOLDER;

            // normaliser og gem folder
            String f = normalizeFolder(String(folder));
            ce.folder = f;

            // album lookup: folder -> album title (fra card)
            // (kun for "album", ikke "playlist")
            if (strcmp(kind, "album") == 0 || strcmp(kind, "playlist") == 0) {
              albumTitleByFolder[keyOfPath(f)] = ce.title;
            }
          } else if (!tracks.isNull()) {
            // tracks[] playlist/album
            uint16_t start = (uint16_t)trackPoolCount;
            uint16_t cnt = 0;

            for (JsonVariant tv : tracks) {
              if (trackPoolCount >= MAX_TRACKPOOL)
                break;

              String ttitle = "";
              String tartist = "";
              String tfile = "";

              if (tv.is<const char *>()) {
                tfile = String(tv.as<const char *>());
              } else if (tv.is<JsonObject>()) {
                JsonObject to = tv.as<JsonObject>();
                ttitle = String((const char *)(to["title"] | ""));
                tartist = String((const char *)(to["artist"] | "---"));
                tfile = String((const char *)(to["file"] | ""));
              }

              if (tfile.length() == 0)
                continue;
              if (!tfile.startsWith("/"))
                tfile = "/" + tfile;
              // Map folder -> title for playlists too (even when play.folder is
              // missing)
              if (strcmp(kind, "playlist") == 0) {
                String fldr = normalizeFolder(dirnameOf(tfile));
                albumTitleByFolder[keyOfPath(fldr)] =
                    ce.title; // "/audio/mix" -> "mix"
              }

              if (ttitle.length() > 0 || tartist.length() > 0) {
                trackMetaByPath[keyOfPath(tfile)] = TrackMeta{ttitle, tartist};
              }

              trackPool[trackPoolCount].title = ttitle;
              trackPool[trackPoolCount].artist = tartist;
              trackPool[trackPoolCount].file = tfile;
              trackPoolCount++;
              cnt++;
            }

            if (cnt > 0) {
              ce.kind = PK_ALBUM_TRACKS;
              ce.trackStart = start;
              ce.trackCount = cnt;
            }
          }
        }
      }
    }

    cardCount++;
  }

  Serial.print("Loaded cards: ");
  Serial.println(cardCount);

  // Optional: quick sanity print for selectors
  for (size_t i = 0; i < cardCount; i++) {
    if (cards[i].role == "game_selector") {
      Serial.print("Selector UID ");
      Serial.print(cards[i].uid);
      Serial.print(" -> gameId=");
      Serial.println(cards[i].gameId);
    }
  }

  return true;
}

static volatile bool isPaused = false;
static volatile bool isPlaying = false;

static volatile bool autoAdvance = false;   // Only true for album/playlist
static volatile bool playlistEnded = false; // Is set when last track is played

static volatile bool advanceNeeded = false; // signal from audioTask -> loop
static volatile int advanceIndex = -1;      // index used when starting

static void startTrack(const char *path) {
  if (!SD.exists(path)) {
    Serial.print("Missing file: ");
    Serial.println(path);
    return;
  }

  if (mp3) {
    mp3->stop();
    delete mp3;
    mp3 = nullptr;
  }
  if (file) {
    delete file;
    file = nullptr;
  }

  file = new AudioFileSourceSD(path);
  mp3 = new AudioGeneratorMP3();

  bool ok = mp3->begin(file, out);
  isPlaying = ok;
  isPaused = false;

  Serial.print("Playing ");
  Serial.print(path);
  Serial.print(" begin=");
  Serial.println(ok ? "OK" : "FAIL");
}

// Audio task on core 1 (Ensures the loop runs smooth)
static void audioTask(void *pv) {
  AudioCmd cmd{};

  for (;;) {
    while (xQueueReceive(audioQ, &cmd, 0) == pdTRUE) {
      if (cmd.type == CMD_PLAY_FILE) {
        isPaused = false;
        startTrack(cmd.path);
      } else if (cmd.type == CMD_TOGGLE_PAUSE) {
        isPaused = !isPaused;
      }
    }

    if (mp3 && !isPaused) {
      if (!mp3->loop()) {
        mp3->stop();
        isPlaying = false;

        // Autoplay for album/playlist: Plan next track
        if (autoAdvance && activeCount > 0 && activeIndex >= 0) {
          int next = activeIndex + 1;
          if (next < (int)activeCount) {
            advanceIndex = next;
            advanceNeeded = true;
          } else {
            // Last track is done -> stop playlist
            playlistEnded = true;
            autoAdvance = false; // stop autoplay until user starts again
          }
        }
      }
    }

    vTaskDelay(1);
  }
}

static void changeVolume(float delta) {
  currentVolume += delta;

  if (currentVolume < VOL_MIN)
    currentVolume = VOL_MIN;
  if (currentVolume > VOL_MAX)
    currentVolume = VOL_MAX;

  out->SetGain(currentVolume);

  Serial.print("Volume: ");
  Serial.println(currentVolume, 2);

  volDirty = true;
  volLastChangedAt = millis();
}

static void maybeSaveVolume(uint32_t now) {
  if (!volDirty)
    return;
  if (now - volLastChangedAt < VOL_SAVE_DELAY_MS)
    return;

  float eff = getEffectiveVolume();
  int v = (int)lroundf(eff * 100.0f);
  if (v < 0) v = 0;
  if (v > 100) v = 100;

  prefs.putInt(PREF_KEY_VOL, v);
  volDirty = false;

  Serial.print("Saved volume: ");
  Serial.println(v);
  Serial.print(volumeLocked ? " (locked)" : " (free)");
Serial.println();
}


// ================= GAME ENGINE START =================

// ---- Game data limits ----
static constexpr size_t MAX_GAMES = 10;
static constexpr size_t MAX_QUESTIONS = 40;
static constexpr size_t MAX_RULE_TAGS = 6; // max tags in a rule
static constexpr size_t MAX_PENDING = 2;   // you want 1 or 2 cards
// static constexpr size_t MAX_CARD_TAGS = 8;      // max tags on an answer card
static uint32_t nextCardDueAt = 0;
static uint8_t repeatCount =
    0; // antal gange vi har gentaget spørgsmålet pga. inaktivitet
static uint8_t nextCardRepeatCount =
    0; // antal nextCard reminders i nuværende forsøg
static bool stopToMusicAfterAudio =
    false; // når idleStop er afspillet, går vi til music

static bool doneAnnounced = false;

static bool gameNoticeActive = false;
static bool replayPromptAfterNotice = false;

enum class GameState : uint8_t { IDLE, INTRO, PROMPT, COLLECT, FEEDBACK, DONE };

enum class RuleType : uint8_t { REQUIRE_TAGS, SUM };

enum class MatchMode : uint8_t { ANY, ALL };

struct GameAudio {
  String intro;
  String correct;
  String wrong;
  String done;
  String nextCardForAnswer;
  String musicHint;
  String idleStop;
};

struct QuestionAudioOverride {
  String correct; // optional
  String wrong;   // optional
};

struct AnswerRule {
  RuleType type = RuleType::REQUIRE_TAGS;

  // requireTags
  MatchMode mode = MatchMode::ANY;
  String tags[MAX_RULE_TAGS];
  uint8_t tagCount = 0;

  // sum
  int equals = 0;                    // target
  uint8_t cards = 1;                 // required cards (default 1)
  String requireTags[MAX_RULE_TAGS]; // e.g. ["tal"]
  uint8_t requireTagCount = 0;
};

struct Question {
  String prompt;
  QuestionAudioOverride audio; // optional overrides
  AnswerRule rule;
};

struct GameTiming {
  uint32_t nextCardRepeatMs = 8000;
  uint32_t answerTimeoutMs = 25000;
  uint32_t maxRepeat = 3;
};

struct GameDef {
  String id;
  String titel;
  GameAudio audio;
  GameTiming timing;
  Question questions[MAX_QUESTIONS];
  uint8_t questionCount = 0;
};

// ---- Loaded games ----
static GameDef games[MAX_GAMES];
static uint8_t gameCount = 0;

// ---- Runtime state ----
static volatile bool gameModeActive = false;
static int activeGameIdx = -1;
static GameState gameState = GameState::IDLE;
static uint8_t questionIdx = 0;

// pending answer cards (store minimal extracted data)
struct PendingCard {
  String uid;
  String tags[MAX_CARD_TAGS];
  uint8_t tagCount = 0;
  int value = -1;
};

static PendingCard pending[MAX_PENDING];
static uint8_t pendingCount = 0;

/// @brief Randomize question ordr
/// @param g Array to sort
static void shuffleQuestions(GameDef &g) {
  if (g.questionCount <= 1)
    return;

  for (int i = g.questionCount - 1; i > 0; i--) {
    int j = random(i + 1); // 0..i
    if (i != j) {
      Question tmp = g.questions[i];
      g.questions[i] = g.questions[j];
      g.questions[j] = tmp;
    }
  }
}

static void uiSetGameProgress(int qIndex0, int total) {
  if (total <= 0) {
    oledLine2 = "Spil klar";
    return;
  }
  char buf[32];
  snprintf(buf, sizeof(buf), "Spg %d/%d", qIndex0 + 1, total);
  oledLine3 = String(buf);
}

// Forward declarations (you already have these helpers somewhere)
static void
playPath(const String &path); // your existing function that enqueues
                              // CMD_PLAY_FILE + persists lastPath
static const CardEntry *findCardByUid(const String &uid); // you already have
static String uidToHexUpper(const MFRC522::Uid &u);       // you already have

// ---- Utility ----
static MatchMode parseMode(const char *s) {
  if (!s)
    return MatchMode::ANY;
  if (strcmp(s, "all") == 0)
    return MatchMode::ALL;
  return MatchMode::ANY;
}

static bool hasTag(const PendingCard &c, const String &tag) {
  for (uint8_t i = 0; i < c.tagCount; i++) {
    if (c.tags[i] == tag)
      return true;
  }
  return false;
}

static bool hasTag(const CardEntry &c, const String &tag) {
  for (uint8_t i = 0; i < c.tagCount; i++) {
    if (c.tags[i] == tag)
      return true;
  }
  return false;
}

static bool unionHasTag(const String &unionTags, const String &tag) {
  // simple contains with delimiters; we use "|tag|" encoding in buildUnionTags
  String needle = "|" + tag + "|";
  return unionTags.indexOf(needle) >= 0;
}

static String buildUnionTags() {
  // Encode as |tag1||tag2|... to avoid partial matches
  String u = "";
  for (uint8_t i = 0; i < pendingCount; i++) {
    for (uint8_t t = 0; t < pending[i].tagCount; t++) {
      String enc = "|" + pending[i].tags[t] + "|";
      if (u.indexOf(enc) < 0)
        u += enc;
    }
  }
  return u;
}

static bool allPendingHaveRequiredTags(const AnswerRule &r) {
  if (r.requireTagCount == 0)
    return true;
  for (uint8_t i = 0; i < pendingCount; i++) {
    for (uint8_t k = 0; k < r.requireTagCount; k++) {
      if (!hasTag(pending[i], r.requireTags[k]))
        return false;
    }
  }
  return true;
}

static bool evalRule(const AnswerRule &r) {
  if (pendingCount == 0)
    return false;

  if (r.type == RuleType::REQUIRE_TAGS) {
    // cards default 1
    uint8_t need = r.cards ? r.cards : 1;
    if (pendingCount < need)
      return false;

    String ut = buildUnionTags();

    if (r.mode == MatchMode::ANY) {
      for (uint8_t i = 0; i < r.tagCount; i++) {
        if (unionHasTag(ut, r.tags[i]))
          return true;
      }
      return false;
    } else { // ALL
      for (uint8_t i = 0; i < r.tagCount; i++) {
        if (!unionHasTag(ut, r.tags[i]))
          return false;
      }
      return true;
    }
  }

  if (r.type == RuleType::SUM) {
    uint8_t need = r.cards ? r.cards : 1;
    if (pendingCount < need)
      return false;

    if (!allPendingHaveRequiredTags(r))
      return false;

    int s = 0;
    for (uint8_t i = 0; i < need; i++) {
      if (pending[i].value < 0)
        return false;
      s += pending[i].value;
    }
    return s == r.equals;
  }

  return false;
}

static const String &selectCorrectAudio(const GameDef &g, const Question &q) {
  if (q.audio.correct.length() > 0)
    return q.audio.correct;
  return g.audio.correct;
}

static const String &selectWrongAudio(const GameDef &g, const Question &q) {
  if (q.audio.wrong.length() > 0)
    return q.audio.wrong;
  return g.audio.wrong;
}

static void clearPending() {
  pendingCount = 0;
  nextCardDueAt = 0;
  nextCardRepeatCount = 0;
  for (uint8_t i = 0; i < MAX_PENDING; i++) {
    pending[i].uid = "";
    pending[i].tagCount = 0;
    pending[i].value = -1;
  }
}

static void gameEnterIdle() {
  gameModeActive = false;
  activeGameIdx = -1;
  gameState = GameState::IDLE;
  questionIdx = 0;
  doneAnnounced = false;
  nextCardDueAt = 0; // clearPending() will do this

  gameNoticeActive = false;
  replayPromptAfterNotice = false;
  clearPending();
}

static void uiSetGameLine3(const GameDef &g, int qIdx, const char *suffix) {
  char buf[32];
  snprintf(buf, sizeof(buf), "Spg %d/%d %s", qIdx + 1, g.questionCount, suffix);
  oledLine3 = String(buf);
}

static void uiSetCollectLine3(const GameDef &g, int qIdx, int pending,
                              int needed) {
  char buf[32];
  if (needed <= 1) {
    snprintf(buf, sizeof(buf), "Spg %d/%d - venter", qIdx + 1, g.questionCount);
  } else {
    snprintf(buf, sizeof(buf), "Spg %d/%d - %d/%d kort", qIdx + 1,
             g.questionCount, pending, needed);
  }
  oledLine3 = String(buf);
}

static void gameStartById(const String &id, const String gameTitel) {

  oledLine2 = gameTitel;
  oledLine3 = "";
  // ---------- Stop music / playlist state ----------
  autoAdvance = false;
  playlistEnded = false;
  advanceNeeded = false;
  advanceIndex = -1;
  clearActivePlaylist(); // or: activeCount = 0; activeIndex = -1;

  repeatCount = 0;
  nextCardRepeatCount = 0;
  nextCardDueAt = 0;
  stopToMusicAfterAudio = false;

  // ---------- Hard reset of game runtime ----------
  gameNoticeActive = false;
  replayPromptAfterNotice = false;

  doneAnnounced = false; // if you use it
  nextCardDueAt = 0;
  clearPending();

  gameModeActive = false;
  activeGameIdx = -1;
  questionIdx = 0;
  gameState = GameState::IDLE;

  // ---------- Find game ----------
  int idx = -1;
  for (uint8_t i = 0; i < gameCount; i++) {
    if (games[i].id == id) {
      idx = (int)i;
      break;
    }
  }

  if (idx < 0) {
    Serial.print("Game not found: ");
    Serial.println(id);
    return;
  }

  // ---------- Activate selected game ----------
  gameModeActive = true;
  activeGameIdx = idx;
  shuffleQuestions(games[activeGameIdx]);
  questionIdx = 0;
  clearPending();

  const GameDef &g = games[activeGameIdx];
  uiSetGameLine3(g, questionIdx, ""); // <-- viser "Spg 1/X"

  // ---------- No questions? ----------
  if (g.questionCount == 0) {
    Serial.print("Game has no questions: ");
    Serial.println(g.id);
    gameState = GameState::DONE;
    if (g.audio.done.length() > 0) {
      playPath(g.audio.done);
    }
    return;
  }

  // ---------- Start intro or first prompt ----------
  if (g.audio.intro.length() > 0) {
    gameState = GameState::INTRO;
    Serial.print("Game start (intro): ");
    Serial.println(g.id);
    playPath(g.audio.intro);
  } else {
    gameState = GameState::PROMPT;
    Serial.print("Game start (no intro): ");
    Serial.println(g.id);
    playPath(g.questions[0].prompt);
  }
}

static void gamePlayMusicHint(bool alsoReplayPrompt) {
  if (!gameModeActive || activeGameIdx < 0)
    return;

  const GameDef &g = games[activeGameIdx];
  Serial.print("Play Music Hint 1");
  if (g.audio.musicHint.length() == 0)
    return;
  Serial.print("Play Music Hint 2");

  gameNoticeActive = true;
  replayPromptAfterNotice = alsoReplayPrompt;

  // Vi bruger FEEDBACK state for at "blokere" scanning mens beskeden spiller,
  // men vi rører ikke pendingCount osv.
  gameState = GameState::FEEDBACK;

  playPath(g.audio.musicHint);
}

bool lastAnswerWasCorrect = false;
static void gamePlayCurrentPrompt() {
  repeatCount = 0;
  nextCardRepeatCount = 0;
  nextCardDueAt = 0;

  lastAnswerWasCorrect = false;

  if (activeGameIdx < 0)
    return;
  GameDef &g = games[activeGameIdx];
  if (questionIdx >= g.questionCount) {
    gameState = GameState::DONE;
    return;
  }

  clearPending();
  gameState = GameState::PROMPT;

  const Question &q = g.questions[questionIdx];
  Serial.print("Prompt q=");
  Serial.println(questionIdx);

  playPath(q.prompt);
}

static void gameOnAnswerScanned(const CardEntry &card) {
  // Accept answer cards only while collecting
  if (!gameModeActive)
    return;
  if (gameState != GameState::COLLECT)
    return;
  if (activeGameIdx < 0)
    return;

  GameDef &g = games[activeGameIdx];
  const Question &q = g.questions[questionIdx];
  const AnswerRule &r = q.rule;

  uint8_t need = r.cards ? r.cards : 1;
  if (need > MAX_PENDING)
    need = MAX_PENDING;

    // -------- MASTER CARD (fail-safe) --------
  if (hasTag(card, "master")) {
    // Fuldfør spørgsmålet straks som korrekt (uanset rule)
    lastAnswerWasCorrect = true;

    
    gameState = GameState::FEEDBACK; // vi "springer" direkte til feedback + korrekt lyd
    if (uiMessages.mastercard_used.length() > 0) {
    playPath(uiMessages.mastercard_used);
  } else {
    playPath(selectCorrectAudio(g, q)); // fallback hvis ikke sat i JSON
  }

    // ryd pending så vi ikke efterlader state
    clearPending();

    // stop evt. "next card" reminder flow
    nextCardDueAt = 0;
    nextCardRepeatCount = 0;

    return;
  }
  if (pendingCount >= need)
    return; // already have enough

  // store
  PendingCard &p = pending[pendingCount];
  p.uid = card.uid;
  p.tagCount = 0;

  // Copy tags
  for (uint8_t i = 0; i < card.tagCount && i < MAX_CARD_TAGS; i++) {
    p.tags[p.tagCount++] = card.tags[i];
  }
  p.value = card.value;

  pendingCount++;

  Serial.print("Collected answer ");
  Serial.print(pendingCount);
  Serial.print("/");
  Serial.println(need);

  // -------- MULTI-CARD UX FIX --------
  if (need > 1 && pendingCount < need) {

    bool possible = true;

    // --- rule-specific early rejection ---
    if (r.type == RuleType::REQUIRE_TAGS && r.mode == MatchMode::ALL) {
      // card must contribute at least one required tag
      bool contributes = false;
      for (uint8_t i = 0; i < r.tagCount; i++) {
        if (hasTag(pending[pendingCount - 1], r.tags[i])) {
          contributes = true;
          break;
        }
      }
      if (!contributes)
        possible = false;
    }

    if (r.type == RuleType::SUM) {
      // must be a valid number card
      if (pending[pendingCount - 1].value < 0) {
        possible = false;
      }
    }

    if (!possible) {
      // early wrong
      gameState = GameState::FEEDBACK;
      playPath(selectWrongAudio(g, q));
      clearPending();
      return;
    }

    // valid first card → prompt for next
    if (g.audio.nextCardForAnswer.length() > 0) {
      gameState = GameState::FEEDBACK; // temporarily block scans
      playPath(g.audio.nextCardForAnswer);

      // start timeout for "next card"
      nextCardDueAt = millis() + g.timing.nextCardRepeatMs;
      nextCardRepeatCount = 0; // første reminder er lige spillet "nu" (vi
                               // tæller kun gentagelser via timeout)
    }

    return;
  }

  if (pendingCount >= need) {
    // Evaluate immediately (no extra state needed)
    lastAnswerWasCorrect = evalRule(r);

    if (lastAnswerWasCorrect) {
      gameState = GameState::FEEDBACK;
      playPath(selectCorrectAudio(g, q));
      // advance question after feedback completes
    } else {
      gameState = GameState::FEEDBACK;
      playPath(selectWrongAudio(g, q));
      // repeat same question after feedback completes
    }
  }
}

// xxxx1
static bool hasBufferedAnswerUid = false;
static String bufferedAnswerUid = "";
static uint32_t bufferedAnswerAt = 0;
static const uint32_t BUFFER_TTL_MS = 5000; // discard efter 5s

static void gameTick(uint32_t now, bool audioIsPlaying) {
  if (!gameModeActive)
    return;
  if (activeGameIdx < 0)
    return;

  GameDef &g = games[activeGameIdx];

  // Only advance when no audio is playing
  if (audioIsPlaying)
    return;

  // If we played idleStop and are supposed to return to music afterwards
  if (stopToMusicAfterAudio) {
    stopToMusicAfterAudio = false;
    gameEnterIdle();
    return;
  }

  // -------- TIMEOUT HANDLING while COLLECTING --------
  // We do timeouts only when we are waiting for cards (COLLECT) and no audio is
  // playing.
  if (gameState == GameState::COLLECT && questionIdx < g.questionCount) {
    const Question &q = g.questions[questionIdx];
    uint8_t need = q.rule.cards ? q.rule.cards : 1;

    // Ensure sane maxRepeat
    uint8_t maxRepeat = (uint8_t)g.timing.maxRepeat;
    if (maxRepeat < 1)
      maxRepeat = 1;

    // nextCard max = maxRepeat-1, but minimum 1
    uint8_t maxNextCardRepeat =
        (maxRepeat > 1) ? (uint8_t)(maxRepeat - 1) : (uint8_t)1;

    // Arm deadline if not armed yet
    if (nextCardDueAt == 0) {
      if (pendingCount == 0) {
        nextCardDueAt = millis() + g.timing.answerTimeoutMs;
      } else if (need > 1 && pendingCount < need) {
        nextCardDueAt = millis() + g.timing.nextCardRepeatMs;
      }
    }

    uint32_t now = millis();
    if (nextCardDueAt != 0 && (int32_t)(now - nextCardDueAt) >= 0) {

      // ---- Case A: waiting for 2nd card (multi-card) ----
      if (need > 1 && pendingCount > 0 && pendingCount < need) {

        if (nextCardRepeatCount >= maxNextCardRepeat) {
          // Too many "next card" reminders -> repeat the whole question
          repeatCount++;

          clearPending(); // resets pendingCount, nextCardDueAt,
                          // nextCardRepeatCount
          gameState = GameState::PROMPT;
          playPath(q.prompt);

          // If the question itself has been repeated too many times -> stop
          // game to music
          if (repeatCount >= maxRepeat) {
            if (g.audio.idleStop.length() > 0) {
              stopToMusicAfterAudio = true;
              gameState = GameState::FEEDBACK;

              Serial.print("IdleStop path  (3): ");
              Serial.println(g.audio.idleStop);
              Serial.print("Exists: ");
              Serial.println(SD.exists(g.audio.idleStop) ? "YES" : "NO");

              playPath(g.audio.idleStop);
            } else {
              gameEnterIdle();
            }
          }
          return;
        }

        // Otherwise play nextCard reminder again
        nextCardRepeatCount++;

        if (g.audio.nextCardForAnswer.length() > 0) {
          gameState = GameState::FEEDBACK; // block scans while prompt plays
          playPath(g.audio.nextCardForAnswer);
        }

        nextCardDueAt = now + g.timing.nextCardRepeatMs;
        return;
      }

      // ---- Case B: waiting for 1st card (no input) ----
      if (pendingCount == 0) {
        repeatCount++;

        if (repeatCount >= maxRepeat) {
          // Stop game due to inactivity
          if (g.audio.idleStop.length() > 0) {
            stopToMusicAfterAudio = true;
            gameState = GameState::FEEDBACK;
            playPath(g.audio.idleStop);
          } else {
            gameEnterIdle();
          }
          return;
        }
        // Repeat current question prompt
        gameState = GameState::PROMPT;
        playPath(q.prompt);
        nextCardDueAt = now + g.timing.answerTimeoutMs;
        return;
      }
    }
  }

  // -------- NORMAL STATE MACHINE --------
  switch (gameState) {
  case GameState::INTRO:
    // intro finished -> play first question prompt
    gamePlayCurrentPrompt(); // should set PROMPT state + play prompt + reset
                             // repeatCount
    break;

  case GameState::PROMPT:
    // prompt finished -> collect answers
    gameState = GameState::COLLECT;
    clearPending();

    if (hasBufferedAnswerUid &&
        (uint32_t)(now - bufferedAnswerAt) < BUFFER_TTL_MS) {
      const CardEntry *be = findCardByUid(bufferedAnswerUid);
      hasBufferedAnswerUid = false;
      bufferedAnswerUid = "";
      ledStopBlink();
      if (be && be->role == "answer") {
        gameOnAnswerScanned(*be);
      }
    }

    // Do NOT reset repeatCount here; it counts how many times we repeated
    // prompt due to inactivity.
    Serial.println("Collecting answers...");
    // uiSetGameLine3(g, questionIdx,"Venter");
    {
      const Question &q = g.questions[questionIdx];
      int needed = q.rule.cards ? q.rule.cards : 1;
      uiSetCollectLine3(g, questionIdx, 0, needed);
    }
    break;

  case GameState::FEEDBACK: {

    // 1) Notice overlay (music hint etc.)
    if (gameNoticeActive) {
      gameNoticeActive = false;

      if (replayPromptAfterNotice) {
        replayPromptAfterNotice = false;
        gameState = GameState::PROMPT;
        const Question &q = g.questions[questionIdx];
        playPath(q.prompt);
      } else {
        gameState = GameState::COLLECT;
        const Question &q = g.questions[questionIdx];
        int needed = q.rule.cards ? q.rule.cards : 1;
        uiSetCollectLine3(g, questionIdx, pendingCount, needed);
      }
      return;
    }

    // 2) If we were waiting for more cards, resume collecting after the short
    // prompt ends
    if (questionIdx < g.questionCount) {
      const Question &q = g.questions[questionIdx];
      int needed = q.rule.cards ? q.rule.cards : 1;

      if (pendingCount > 0 && pendingCount < needed) {
        gameState = GameState::COLLECT;
        uiSetCollectLine3(g, questionIdx, pendingCount, needed);
        return;
      }
    }

    // 3) Normal correct/wrong feedback just finished -> advance or repeat
    const Question &q = g.questions[questionIdx];
    // bool ok = evalRule(q.rule);

    if (lastAnswerWasCorrect) { // Was set by gameOnAnswerScanned()
      questionIdx++;
      uiSetGameLine3(g, questionIdx, "");
      clearPending();
      nextCardDueAt = 0;
      nextCardRepeatCount = 0;
      repeatCount = 0; // new question starts fresh

      if (questionIdx >= g.questionCount) {
        gameState = GameState::DONE;
        oledLine2 = "";
        oledLine3 = "Færdig, vælg nyt spil, eller musik";
        if (g.audio.done.length() > 0)
          playPath(g.audio.done);
      } else {
        uiSetGameLine3(g, questionIdx, "");
        gamePlayCurrentPrompt(); // should reset repeatCount for new question
      }
    } else {
      // repeat same question (wrong)
      clearPending();
      nextCardDueAt = 0;
      nextCardRepeatCount = 0;
      // repeatCount NOT incremented here; it's for inactivity timeouts, not
      // wrong answers
      gamePlayCurrentPrompt();
    }
    break;
  }

  case GameState::DONE:
    if (!doneAnnounced) {
      Serial.println("Game done. Waiting for MUSIC button or a new GAME");
      doneAnnounced = true;
    }
    oledLine3 = "Vælg nyt spil";
    // Stay in DONE; selector can restart via RFID handler
    break;

  default:
    break;
  }
}

// ---- JSON loading for games[] ----
// Call this after SD is ready and settings.json exists.
// It re-opens settings.json and parses only the "games" array.
static bool loadGamesJson(const char *jsonPath) {
  gameCount = 0;

  File f = SD.open(jsonPath, FILE_READ);
  if (!f) {
    Serial.print("Could not open JSON for games: ");
    Serial.println(jsonPath);
    return false;
  }

  //DynamicJsonDocument doc(16384);
  DynamicJsonDocument doc(DOC_SIZE);
  DeserializationError err = deserializeJson(doc, f);
  Serial.println("loadGamesJson");
  Serial.print("JSON capacity: ");
  Serial.println(doc.capacity());

  Serial.print("JSON memoryUsage: ");
  Serial.println(doc.memoryUsage());
  f.close();
  if (err) {
    Serial.print("Games JSON parse error: ");
    Serial.println(err.c_str());
    return false;
  }

  JsonArray arr = doc["games"].as<JsonArray>();
  if (arr.isNull()) {
    Serial.println("JSON missing 'games' array");
    return false;
  }

  for (JsonObject g : arr) {
    if (gameCount >= MAX_GAMES)
      break;

    const char *id = g["id"] | "";
    if (strlen(id) == 0)
      continue;

    GameDef &gd = games[gameCount];
    gd.id = String(id);
    const char *titel = g["titel"] | "Ingen titel";
    gd.titel = String(titel);
    gd.questionCount = 0;

    // audio
    JsonObject audio = g["audio"].as<JsonObject>();
    if (!audio.isNull()) {
      gd.audio.intro = String((const char *)(audio["intro"] | ""));
      gd.audio.correct = String((const char *)(audio["correct"] | ""));
      gd.audio.wrong = String((const char *)(audio["wrong"] | ""));
      gd.audio.done = String((const char *)(audio["done"] | ""));
      gd.audio.nextCardForAnswer =
          String((const char *)(audio["nextCardForAnswer"] | ""));
      gd.audio.musicHint = String((const char *)(audio["musicHint"] | ""));
      gd.audio.idleStop = String((const char *)(audio["idleStop"] | ""));
    }
    /*
    Serial.print("Game ");
    Serial.print(gd.id);
    Serial.print(" idleStop=");
    Serial.println(gd.audio.idleStop);
    */

    JsonObject timing = g["timing"].as<JsonObject>();
    if (!timing.isNull()) {
      gd.timing.answerTimeoutMs = (uint32_t)(timing["answerTimeoutMs"] | 25000);
      gd.timing.nextCardRepeatMs =
          (uint32_t)(timing["nextCardRepeatMs"] | 18000);
      gd.timing.maxRepeat = (uint32_t)(timing["maxRepeat"] | 3);
    } else {
      gd.timing.answerTimeoutMs = 25000;
      gd.timing.nextCardRepeatMs = 18000;
      gd.timing.maxRepeat = 3;
    }

    // questions
    JsonArray qs = g["questions"].as<JsonArray>();
    if (!qs.isNull()) {
      for (JsonObject q : qs) {
        if (gd.questionCount >= MAX_QUESTIONS)
          break;

        Question &qq = gd.questions[gd.questionCount];
        qq.prompt = String((const char *)(q["prompt"] | ""));

        // question audio override (optional): audio.correct / audio.wrong
        JsonObject qa = q["audio"].as<JsonObject>();
        if (!qa.isNull()) {
          qq.audio.correct = String((const char *)(qa["correct"] | ""));
          qq.audio.wrong = String((const char *)(qa["wrong"] | ""));
        } else {
          qq.audio.correct = "";
          qq.audio.wrong = "";
        }

        // answer rule
        JsonObject a = q["answer"].as<JsonObject>();
        AnswerRule &r = qq.rule;

        r.cards = (uint8_t)(a["cards"] | 1);

        const char *type = a["type"] | "requireTags";
        if (strcmp(type, "requireTags") == 0) {
          r.type = RuleType::REQUIRE_TAGS;
          r.mode = parseMode(a["mode"] | "any");

          r.tagCount = 0;
          JsonArray tags = a["tags"].as<JsonArray>();
          if (!tags.isNull()) {
            for (JsonVariant tv : tags) {
              if (r.tagCount >= MAX_RULE_TAGS)
                break;
              r.tags[r.tagCount++] = String(tv.as<const char *>());
            }
          }
        } else if (strcmp(type, "sum") == 0) {
          r.type = RuleType::SUM;
          r.equals = (int)(a["equals"] | 0);

          // requireTags for sum (using "tags" field in your schema)
          r.requireTagCount = 0;
          JsonArray req = a["tags"].as<JsonArray>();
          if (!req.isNull()) {
            for (JsonVariant tv : req) {
              if (r.requireTagCount >= MAX_RULE_TAGS)
                break;
              r.requireTags[r.requireTagCount++] =
                  String(tv.as<const char *>());
            }
          }
        } else {
          // fallback: treat as requireTags
          r.type = RuleType::REQUIRE_TAGS;
          r.mode = MatchMode::ANY;
          r.tagCount = 0;
        }

        gd.questionCount++;
      }
    }

    Serial.print("Loaded game ");
    Serial.print(gd.id);
    Serial.print(" questions=");
    Serial.println(gd.questionCount);

    gameCount++;
  }

  Serial.print("Total games loaded: ");
  Serial.println(gameCount);
  return true;
}

// ================= GAME ENGINE END ===================

static void handleAction(Action a) {
  switch (a) {
  case ACT_PLAY_PAUSE: {
    if (isPlaying) {
      AudioCmd c{};
      c.type = CMD_TOGGLE_PAUSE;
      if (xQueueSend(audioQ, &c, 0) != pdTRUE) {
        Serial.println("audioQ full (toggle pause)");
      }
    } else {
      if (playlistEnded && activeCount > 0) {
        playlistEnded = false;
        autoAdvance = true;
        playActiveIndex(0);
        break;
      }
      if (hasLastPath) {
        AudioCmd c{};
        c.type = CMD_PLAY_FILE;
        strncpy(c.path, lastPath, sizeof(c.path) - 1);
        c.path[sizeof(c.path) - 1] = '\0';
        if (xQueueSend(audioQ, &c, 0) != pdTRUE) {
          Serial.println("audioQ full (play last)");
        }
      } else {
        for (int i = 0; i < 2; i++) {
          // digitalWrite(PIN_LED_CARD, HIGH);
          ledSetNormal(true);
          delay(60);
          // digitalWrite(PIN_LED_CARD, LOW);
          ledSetNormal(false);
          delay(60);
        }
      }
    }

    Serial.println("PLAY/PAUSE");
    break;
  }

  case ACT_NEXT:
    if (activeCount == 0)
      break;

    if (playlistEnded) {
      playlistEnded = false;
      autoAdvance = true; // restart autoplay
      playActiveIndex(0);
      break;
    }
    if (activeCount > 0) {
      int next = activeIndex + 1;
      if (next >= (int)activeCount)
        next = 0; // wrap
      playActiveIndex(next);
    }
    Serial.println("BTN: NEXT");
    break;

  case ACT_PREV:
    if (activeCount > 0) {
      int prev = activeIndex - 1;
      if (prev < 0)
        prev = (int)activeCount - 1; // wrap
      playActiveIndex(prev);
    }
    Serial.println("BTN: PREV");
    break;
  case ACT_VOL_UP:
    if (!volumeLocked) {
      changeVolume(+VOL_STEP);
      Serial.println("BTN: VOL UP");
    }
    break;
  case ACT_VOL_DOWN:
    if (!volumeLocked) {
      changeVolume(-VOL_STEP);
       Serial.println("BTN: VOL DOWN");
    }
    break;
  case ACT_MODE_MUSIC:
    Serial.println("MODE: MUSIC");
    // Music button pressed, but no music yet selected
    oledLine2 = "";
    oledLine3 = "";
    bool cameFromGame = gameModeActive;
    gameEnterIdle(); // THIS is your rule: only music button exits game
    Serial.println("Came from game: ");
    Serial.println(cameFromGame ? "YES" :"NO");
    Serial.print(uiMessages.musicModeInfo.length());
     if (cameFromGame && uiMessages.musicModeInfo.length() > 0) {
    playPath(uiMessages.musicModeInfo);
  }
    break;
  }
}

static void pollButtons(uint32_t now) {
  // Active LOW (INPUT_PULLUP): push = LOW
  for (auto &b : buttons) {
    bool reading = digitalRead(b.pin);

    // --- Debounce + edge detection ---
    if (reading != b.lastState) {
      if (b.lastChange == 0)
        b.lastChange = now;

      if (now - b.lastChange >= DEBOUNCE_MS) {
        b.lastState = reading;
        b.lastChange = 0;

        if (b.lastState == LOW) {
          // Button pushed down: registrér time + make single action
          b.pressedAt = now;
          b.lastRepeat = now;
          handleAction(b.action); // Short push = 1 step (Also for VOL)
        } else {
          // Slipped
          b.pressedAt = 0;
          b.lastRepeat = 0;
        }
      }
    } else {
      b.lastChange = 0;
    }

    // --- Long-press repeat only for VOL_UP / VOL_DOWN ---
    if (b.lastState == LOW && b.pressedAt != 0) {
      if (b.action == ACT_VOL_UP || b.action == ACT_VOL_DOWN) {
        uint32_t heldMs = now - b.pressedAt;

        if (heldMs >= LONGPRESS_START_MS) {
          if (now - b.lastRepeat >= LONGPRESS_REPEAT_MS) {
            b.lastRepeat = now;
            handleAction(b.action); // Repeat volume-step
          }
        }
      }
    }
  }
}

// Display

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/U8X8_PIN_NONE);

static void oledInit() {
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  // Optional: Wire.setClock(400000); // kan sættes senere

  u8g2.begin();
  u8g2.enableUTF8Print();
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 12, "OLED OK");
  u8g2.sendBuffer();
}

void oledShowText(const String &line1) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);

  u8g2.drawStr(0, 12, line1.c_str());

  u8g2.sendBuffer();
}


// Tegner en bar baseret på currentVolume i intervallet [VOL_MIN..VOL_MAX]
static void drawVolumeBar(float vol, float volMin, float volMax) {
  vol = clampf(vol, volMin, volMax);

  const int x = 0;
  const int y = 52;
  const int w = 128;
  const int h = 10;

  u8g2.drawFrame(x, y, w, h);

  // Normaliser til 0..1
  float t = (vol - volMin) / (volMax - volMin);
  if (t < 0)
    t = 0;
  if (t > 1)
    t = 1;

  int fillW = (int)((w - 2) * t + 0.5f); // afrunding
  if (fillW > 0) {
    u8g2.drawBox(x + 1, y + 1, fillW, h - 2);
  }
}

void oledShowStatus(const String &line1, const String &line2, float vol,
                    float volMax) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);

  u8g2.drawStr(0, 12, line1.c_str());
  u8g2.drawStr(0, 28, line2.c_str());

  // Vis vol som procent (giver mening for mennesker)
  // (volMax=0.9, volMin=0.05)
  float volMin = VOL_MIN;
  vol = clampf(vol, volMin, volMax);
  int pct = (int)(100.0f * (vol - volMin) / (volMax - volMin) + 0.5f);

  char vbuf[16];
  snprintf(vbuf, sizeof(vbuf), "Vol %d%%", pct);
  u8g2.drawStr(86, 44, vbuf);

  drawVolumeBar(vol, VOL_MIN, volMax);

  u8g2.sendBuffer();
}

static void drawMaybeScrollLine(int y, const String &text, uint32_t now) {
  u8g2.setFont(u8g2_font_6x12_tf);
  int w = u8g2.getStrWidth(text.c_str());
  const int viewW = 128;

  if (w <= viewW) {
    u8g2.drawStr(0, y, text.c_str());
    return;
  }

  // scroll 25 px/s, med 800ms pause ved start
  const uint32_t pauseMs = 800;
  const uint32_t speed = 25; // px/s
  uint32_t t = now % (pauseMs + (uint32_t)((w - viewW + 20) * 1000 / speed));
  int offset = 0;
  if (t > pauseMs)
    offset = (int)(((t - pauseMs) * speed) / 1000);

  u8g2.drawStr(-offset, y, text.c_str());
}

/*
static void drawScrollWithPauses(int x, int y, const String &text,
                                 uint32_t now) {
  int w = u8g2.getStrWidth(text.c_str());
  const int viewW = 128;

  if (w <= viewW) {
    u8g2.drawUTF8(x, y, text.c_str());
    return;
  }

  const int maxOffset = w - viewW;

  const uint32_t holdStartMs = 2000;
  const uint32_t holdEndMs = 2000;
  const uint32_t speedPxPerSec = 25;

  const uint32_t scrollMs = (uint32_t)((maxOffset * 1000UL) / speedPxPerSec);
  const uint32_t cycleMs = holdStartMs + scrollMs + holdEndMs;

  uint32_t t = now % cycleMs;

  int offset = 0;
  if (t < holdStartMs) {
    offset = 0;
  } else if (t < holdStartMs + scrollMs) {
    uint32_t tt = t - holdStartMs;
    offset = (int)((tt * speedPxPerSec) / 1000UL);
    if (offset > maxOffset)
      offset = maxOffset;
  } else {
    offset = maxOffset;
  }

  u8g2.drawUTF8(x - offset, y, text.c_str());
}
*/

static void drawScrollWithPauses(int x, int y, const String &text, uint32_t now)
{
  const int viewW = 120;           // samme som du fandt virker
  const uint32_t pauseMs = 1400;    // pause i enderne
  const uint32_t stepMs  = 35;     // ms pr pixel (lavere = hurtigere)

  int w = u8g2.getStrWidth(text.c_str());
  if (w <= viewW) {
    u8g2.drawUTF8(x, y, text.c_str());
    return;
  }

  static String lastText = "";
  static int offset = 0;
  static uint32_t lastStepAt = 0;
  static uint32_t pauseUntil = 0;

  // dir: +1 = mod venstre (offset øges), -1 = mod højre (offset mindskes)
  static int8_t dir = +1;
  static bool paused = true;

  // reset hvis teksten ændres
  if (text != lastText) {
    lastText = text;
    offset = 0;
    dir = +1;
    paused = true;
    pauseUntil = now + pauseMs;   // start med pause ved start
    lastStepAt = now;
  }

  const int maxOffset = w - viewW;

  if (paused) {
    if ((int32_t)(now - pauseUntil) >= 0) {
      paused = false;
      lastStepAt = now;
    }
  } else {
    if ((int32_t)(now - lastStepAt) >= (int32_t)stepMs) {
      int steps = (now - lastStepAt) / stepMs;
      lastStepAt += steps * stepMs;

      offset += dir * steps;

      if (offset <= 0) {
        offset = 0;
        paused = true;
        pauseUntil = now + pauseMs;  // pause ved start
        dir = +1;                    // vend: scroll mod venstre
      } else if (offset >= maxOffset) {
        offset = maxOffset;
        paused = true;
        pauseUntil = now + pauseMs;  // pause ved slut
        dir = -1;                    // vend: scroll mod højre
      }
    }
  }

  u8g2.drawUTF8(x - offset, y, text.c_str());
}



static void drawLockIcon(uint8_t x, uint8_t y)
{
  // bøjle
  u8g2.drawFrame(x + 2, y, 4, 4);
  // krop
  u8g2.drawBox(x, y + 4, 8, 6);
  // hul
  u8g2.drawBox(x + 3, y + 6, 2, 2);
}

static void drawNoRepeatIcon(uint8_t x, uint8_t y)
{
  // En lille cirkel som "repeat"
  u8g2.drawCircle(x + 5, y + 5, 4);

  // "pil-hoved" (lille trekant-ish)
  u8g2.drawLine(x + 6, y + 1, x + 9, y + 1);
  u8g2.drawLine(x + 9, y + 1, x + 8, y + 0);
  u8g2.drawLine(x + 9, y + 1, x + 8, y + 2);

  // forbudstreg
  u8g2.drawLine(x + 1, y + 9, x + 9, y + 1);
}


static uint32_t lastOledMs = 0;
static String last1, last2, last3;
static int lastPct = -1;
static bool lastLocked = false;
static bool lastAntiRepeat = false;

static void oledDraw3LinesIfChanged(uint32_t now, float currentVol) {
  if ((uint32_t)(now - lastOledMs) < 100) return;

  const float volMin = VOL_MIN;
  const float volMax = VOL_MAX;
  float vol = clampf(currentVol, volMin, volMax);
  int pct = (int)(100.0f * (vol - volMin) / (volMax - volMin) + 0.5f);

  String l1 = String("Mode: ") + (gameState != GameState::IDLE ? "Game" : "Music");
  const String &l2 = oledLine2;
  const String &l3 = oledLine3;

  // VIGTIGT: needScroll skal beregnes før change detection
  // (vi bruger den aktuelle font, så sørg for at font er sat inden getStrWidth)
  u8g2.setFont(u8g2_font_6x12_tf);
  const int viewW = 120; // = 128;
  bool needScroll = (u8g2.getStrWidth(l3.c_str()) > viewW);

  // Change detection:    hvis der skal scrolles, må vi IKKE returnere,
  // ellers får vi aldrig animationen.
  if (!needScroll &&
      l1 == last1 && l2 == last2 && l3 == last3 &&
      pct == lastPct &&
      volumeLocked == lastLocked &&
      parentalAntiRepeatEnabled == lastAntiRepeat) {
    return;
  }

  lastOledMs = now;
  last1 = l1; last2 = l2; last3 = l3;
  lastPct = pct;
  lastLocked = volumeLocked;
  lastAntiRepeat = parentalAntiRepeatEnabled;

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);

  u8g2.drawStr(0, 12, l1.c_str());
  u8g2.drawUTF8(0, 28, l2.c_str());

  if (needScroll) {
    drawScrollWithPauses(0, 44, l3, now);
  } else {
    u8g2.drawUTF8(0, 44, l3.c_str());
  }

  if (parentalAntiRepeatEnabled) drawNoRepeatIcon(96, 2);
  if (volumeLocked) drawLockIcon(110, 2);

  drawVolumeBar(vol, volMin, volMax);
  u8g2.sendBuffer();
}

void setup() {
  Serial.begin(115200);
  delay(200);
  randomSeed(esp_random());
  // CS pins stabile
  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH);
  pinMode(PIN_RC522_CS, OUTPUT);
  digitalWrite(PIN_RC522_CS, HIGH);

  // One shared SPI for both SD og RC522 (important!)
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI);

  for (auto &b : buttons) {
    pinMode(b.pin, INPUT_PULLUP);
  }

  pinMode(PIN_LED_CARD, OUTPUT);
  // digitalWrite(PIN_LED_CARD, LOW);
  ledSetNormal(false);

  Serial.println("Init SD...");
  while (
      !SD.begin(PIN_SD_CS, SPI, 8000000)) { // prøv 8MHz; hvis ustabilt: 4000000
    Serial.println("SD.begin FAILED, try to remove and reinsert the SD-card");
    digitalWrite(PIN_LED_CARD, HIGH);
    delay(1000);
    digitalWrite(PIN_LED_CARD, LOW);
    delay(5000);
  }
  /*
  while (!SD.begin(PIN_SD_CS, SPI, 8000000)) { // prøv 8MHz; hvis ustabilt:
  4000000 Serial.println("SD.begin FAILED"); while (true) delay(1000);
  }
  */

  Serial.println("SD OK");

  // I2S out (UDA1334)
  out = new AudioOutputI2S();
  out->SetPinout(PIN_I2S_BCLK, PIN_I2S_WSEL, PIN_I2S_DIN);
  out->SetOutputModeMono(true);
  prefs.begin(PREF_NS, false);

  String lp = prefs.getString(PREF_KEY_LASTPATH, "");
  if (lp.length() > 0 && lp.length() < sizeof(lastPath)) {
    strncpy(lastPath, lp.c_str(), sizeof(lastPath) - 1);
    hasLastPath = true;
    Serial.print("Loaded last track: ");
    Serial.println(lastPath);
  }

  int v = prefs.getInt(PREF_KEY_VOL, -1); // -1 = No stored value

  if (v >= 0 && v <= 100) {
    currentVolume = v / 100.0f;
  } else {
    currentVolume = 0.40f; // default
  }

  Serial.print("Loaded volume: ");
  Serial.println(currentVolume, 2);

  out->SetGain(currentVolume);

  // RC522 init
  mfrc522.PCD_Init();
  mfrc522.PCD_AntennaOn();
  mfrc522.PCD_SetAntennaGain(mfrc522.RxGain_max);
  mfrc522.PCD_DumpVersionToSerial();
  Serial.println("RC522 OK");

  audioQ = xQueueCreate(8, sizeof(AudioCmd));
  xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 3, nullptr, 1);

  // Load JSON
  loadCardsJson("/settings.json");
  loadGamesJson("/settings.json"); // games[]: rules + prompts + audio
  gameEnterIdle();
  oledInit();

  // DEBUG Remove when used
  Serial.printf("Meta entries: %d\n", (int)trackMetaByPath.size());
  auto it = trackMetaByPath.find(keyOfPath("/audio/hjulene_paa_bus.mp3"));
  if (it != trackMetaByPath.end()) {
    Serial.println(it->second.title);
    Serial.println(it->second.artist);
  }
}

void loop() {

  uint32_t now = millis();
  ledTick(now);
  // 1) Button must run continuesly (no early returns)
  pollButtons(now);

  oledDraw3LinesIfChanged(now, currentVolume);

  static uint32_t lastPoll = 0;
  gameTick(lastPoll, isPlaying && !isPaused);
  //Try this 
  //gameTick(now, isPlaying && !isPaused);

  // Handle auto-advance from audioTask
  if (!gameModeActive && advanceNeeded) {
    advanceNeeded = false;
    int idx = advanceIndex;
    advanceIndex = -1;
    if (idx >= 0 && idx < (int)activeCount) {
      playActiveIndex(idx);
    }
  }

  maybeSaveVolume(now);

 
  if ((uint32_t)(now - lastPoll) >= 25) {

    lastPoll = now;

    // Make sure SD is not choosen , while we communicate with RC522
    digitalWrite(PIN_SD_CS, HIGH);

    // If no new card -> turn off LED and leave
    if (!mfrc522.PICC_IsNewCardPresent()) {
      // digitalWrite(PIN_LED_CARD, LOW);
      ledSetNormal(false);
      return;
    }

    // New card detected -> LED on instantly
    // digitalWrite(PIN_LED_CARD, HIGH);
    ledSetNormal(true);

    if (!mfrc522.PICC_ReadCardSerial()) {
      // Card was detected, buth could not be read – Turn off LED again
      // digitalWrite(PIN_LED_CARD, LOW);
      ledSetNormal(false);
      return;
    }

    String uid = uidToHexUpper(mfrc522.uid);

    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();

    const CardEntry *e = findCardByUid(uid);
    if (!e) {
      Serial.print("Unknown UID: ");
      Serial.println(uid);
      ledSetNormal(false);
      // digitalWrite(PIN_LED_CARD, LOW);
      return;
    }

    Serial.print("Current role is: ");
    Serial.println(e->role);
    Serial.println(uid);

    if (e->role == "parent") {
      if (e->action == "toggle_anti_repeat") {
        parentalAntiRepeatEnabled = !parentalAntiRepeatEnabled;

        if (parentalAntiRepeatEnabled &&
            uiMessages.antiRepeatEnabled.length() > 0) {
          playPath(uiMessages.antiRepeatEnabled);
        } else if (!parentalAntiRepeatEnabled &&
                   uiMessages.antiRepeatDisabled.length() > 0) {
          playPath(uiMessages.antiRepeatDisabled);
        }
      }
      if (e->action == "toggle_volume_lock") {
        volumeLocked = !volumeLocked;

        if (volumeLocked) {
          // Lås til nuværende værdi
          lockedVolume = currentVolume;

          if (uiMessages.volumeLockOn.length() > 0)
            playPath(uiMessages.volumeLockOn);

        } else {
          // Når der låses op: fortsæt på den låste værdi
          currentVolume = lockedVolume;

          if (uiMessages.volumeLockOff.length() > 0)
            playPath(uiMessages.volumeLockOff);
        }
  return;
}

      return;
    }

    if (e->role == "game_selector") {
      Serial.println("GAME SELECT: " + e->gameId);

      gameStartById(e->gameId,
                    e->title); // always abort current + start selected
      return;
    }

    if (e->role == "answer") {
      // Hvis vi IKKE er klar til at modtage svar endnu (prompt/feedback
      // spiller),
      // så buffer UID så det tæller når vi går i COLLECT.
      if (!gameModeActive || gameState != GameState::COLLECT) {
        bufferedAnswerUid = uid;
        bufferedAnswerAt = now;
        hasBufferedAnswerUid = true;
        ledStartBlink(now, 1000);
        return;
      }
      gameOnAnswerScanned(*e);
      return;
    }

    if (e->role == "music") {
      if (gameModeActive) {
        gamePlayMusicHint(true);
        return;
      }

      if (e->kind == PK_SINGLE) {
        autoAdvance = false;
        playlistEnded = false;

        String path = e->file;
        if (!path.startsWith("/"))
          path = "/" + path;

        // Byg active-liste for next/prev i samme folder
        setActiveFromFolder(dirnameOf(path));

        // Find index i activeTracks hvis muligt (kun til navigation)
        int found = -1;
        for (size_t i = 0; i < activeCount; i++) {
          if (activeTracks[i] == path) {
            found = (int)i;
            break;
          }
        }
        if (found >= 0)
          activeIndex = found;
        else
          activeIndex = 0; // fallback, men afspilning styres stadig af 'path'
        oledLine2 = lookupAlbumTitleForTrackPath(path);
        if (oledLine2.length() == 0) oledLine2 = e->title; // evt fallback
        // Anti-repeat gate
        playTrackDirect(path);
        return;
      } else if (e->kind == PK_ALBUM_FOLDER) {
        autoAdvance = true;
        playlistEnded = false;
        String folder = e->folder;
        if (!folder.startsWith("/"))
          folder = "/" + folder;

        oledLine2 = e->title;
        oledLine3 = ""; // indtil JSON artist findes
        setActiveFromFolder(folder);
        playActiveIndex(0);
      } else if (e->kind == PK_ALBUM_TRACKS) {
        autoAdvance = true;
        playlistEnded = false;
        setActiveFromTrackPool(e->trackStart, e->trackCount);
        oledLine2 = e->title;
        oledLine3 = ""; // indtil JSON artist findes
        playActiveIndex(0);
      } else {
        Serial.println("Music card missing play info");
      }
    }

    Serial.print("UID ");
    Serial.print(uid);
  }

  static uint32_t lastDbg = 0;
if (now - lastDbg > 500) {
  lastDbg = now;
  Serial.print("activeCount: "); Serial.print(activeCount);
  Serial.print(" activeIndex: "); Serial.print(activeIndex);
  if (activeCount > 0) { Serial.print(" activeTracks[0]: "); Serial.print(activeTracks[0]); }
  Serial.println();
}


  // Turn off LED after play was queded
  // digitalWrite(PIN_LED_CARD, LOW);
  ledSetNormal(false);
}
