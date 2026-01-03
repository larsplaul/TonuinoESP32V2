#include <Arduino.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>

#include <ArduinoJson.h>
#include <MFRC522.h>

// ESP8266Audio 
#include "AudioFileSourceSD.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

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
//static constexpr uint8_t PIN_BTN_PLAY = 48;
static constexpr uint8_t PIN_BTN_BACK = 17;
static constexpr uint8_t PIN_BTN_VOL_UP = 18;
static constexpr uint8_t PIN_BTN_VOL_DOWN = 39;
static constexpr uint8_t PIN_BTN_MODE_MUS = 40;
static constexpr uint8_t PIN_BTN_PLAY    = 41;
//static constexpr uint8_t PIN_BTN_MODE_GA = 41;
//static constexpr uint8_t PIN_BTN_MODE_GB = 42;

// LED for RFID Card Detection
static constexpr uint8_t PIN_LED_CARD = 2;

// Track info
static constexpr size_t MAX_TRACKS = 300;



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

// Handle last track for play/pause
static constexpr const char *PREF_KEY_LASTPATH = "last_path";
static char lastPath[128] = {0}; // RAM copy
static bool hasLastPath = false;

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

// ---------------- RC522 ----------------
MFRC522 mfrc522(PIN_RC522_CS, PIN_RC522_RST);

// ---------------- Audio ----------------
AudioGeneratorMP3 *mp3 = nullptr;
AudioFileSourceSD *file = nullptr;
AudioOutputI2S *out = nullptr;

// Card tracking
enum PlayKind : uint8_t {
  PK_NONE = 0,
  PK_SINGLE,
  PK_ALBUM_FOLDER,
  PK_ALBUM_TRACKS
};

struct TrackItem {
  String title; // optional
  String file;  // absolute path
};

// A “pool” with all track-items from all album(track-lists)
static constexpr size_t MAX_TRACKPOOL = 600; //Adjust if needed
static TrackItem trackPool[MAX_TRACKPOOL];
static size_t trackPoolCount = 0;

// Tune these to your needs / memory budget
static constexpr uint8_t MAX_CARD_TAGS = 8;

struct CardEntry {
  // Common
  String uid;     // uppercase hex
  String role;    // "music", "answer", "game_selector"
  String title;

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
};


static constexpr size_t MAX_CARDS = 80;
CardEntry cards[MAX_CARDS];
size_t cardCount = 0;

// End of Card tracking

static constexpr size_t MAX_ACTIVE = 300;
static String activeTracks[MAX_ACTIVE];
static size_t activeCount = 0;
static int activeIndex = -1;

static void clearActivePlaylist() {
  activeCount = 0;
  activeIndex = -1;
}

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

static String dirnameOf(const String &fullPath) {
  int slash = fullPath.lastIndexOf('/');
  if (slash <= 0)
    return "/";
  return fullPath.substring(0, slash);
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

  activeIndex = idx;
  playPath(activeTracks[activeIndex]);
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

static const CardEntry *findCardByUid(const String &uid) {
  for (size_t i = 0; i < cardCount; i++) {
    if (cards[i].uid == uid)
      return &cards[i];
  }
  return nullptr;
}


static bool loadCardsJson(const char *jsonPath) {
  trackPoolCount = 0;
  cardCount = 0;

  File f = SD.open(jsonPath, FILE_READ);
  if (!f) {
    Serial.print("Could not open JSON: ");
    Serial.println(jsonPath);
    return false;
  }

  // Increase if JSON grows
  DynamicJsonDocument doc(16384);

  DeserializationError err = deserializeJson(doc, f);
  f.close();

  if (err) {
    Serial.print("JSON parse error: ");
    Serial.println(err.c_str());
    return false;
  }

  JsonArray arr = doc["cards"].as<JsonArray>();
  if (arr.isNull()) {
    Serial.println("JSON missing 'cards' array");
    return false;
  }

  for (JsonObject c : arr) {
    if (cardCount >= MAX_CARDS) break;

    const char *uid   = c["uid"]   | "";
    const char *role  = c["role"]  | "";
    const char *title = c["title"] | "";

    String suid(uid);
    suid.toUpperCase();
    if (suid.length() == 0) continue;

    // -------- common fields --------
    CardEntry &ce = cards[cardCount];
    ce.uid   = suid;
    ce.role  = String(role);
    ce.title = String(title);

    // -------- defaults (important!) --------
    ce.gameId   = "";
    ce.tagCount = 0;
    ce.value    = -1;

    ce.kind      = PK_NONE;
    ce.file      = "";
    ce.folder    = "";
    ce.trackStart = 0;
    ce.trackCount = 0;

    // -------- role-specific parsing --------
    if (ce.role == "game_selector") {
      const char* gid = c["gameId"] | "";
      ce.gameId = String(gid);
    }
    else if (ce.role == "answer") {
      // tags[]
      JsonArray tags = c["tags"].as<JsonArray>();
      if (!tags.isNull()) {
        for (JsonVariant tv : tags) {
          if (ce.tagCount >= MAX_CARD_TAGS) break;
          if (tv.is<const char*>()) {
            ce.tags[ce.tagCount++] = String(tv.as<const char*>());
          }
        }
      }

      // optional value (for sum games)
      if (c.containsKey("value")) {
        ce.value = (int)(c["value"] | -1);
      }
    }
    // music (and any other roles that have play object)
    // we only parse play for music cards to avoid accidental parsing on other roles
    if (ce.role == "music") {
      JsonObject play = c["play"].as<JsonObject>();
      if (!play.isNull()) {
        const char *kind = play["kind"] | "";

        if (strcmp(kind, "single") == 0) {
          const char *file = play["file"] | "";
          if (strlen(file) > 0) {
            ce.kind = PK_SINGLE;
            ce.file = String(file);
          }
        }
        else if (strcmp(kind, "album") == 0 || strcmp(kind, "playlist") == 0) {
          const char *folder = play["folder"] | "";
          JsonArray tracks = play["tracks"].as<JsonArray>();

          if (strlen(folder) > 0) {
            ce.kind = PK_ALBUM_FOLDER;
            ce.folder = String(folder);
          }
          else if (!tracks.isNull()) {
            // tracks[] playlist/album
            uint16_t start = (uint16_t)trackPoolCount;
            uint16_t cnt = 0;

            for (JsonVariant tv : tracks) {
              if (trackPoolCount >= MAX_TRACKPOOL) break;

              String ttitle = "";
              String tfile  = "";

              if (tv.is<const char*>()) {
                tfile = String(tv.as<const char*>());
              } else if (tv.is<JsonObject>()) {
                JsonObject to = tv.as<JsonObject>();
                ttitle = String((const char*)(to["title"] | ""));
                tfile  = String((const char*)(to["file"]  | ""));
              }

              if (tfile.length() == 0) continue;
              if (!tfile.startsWith("/")) tfile = "/" + tfile;

              trackPool[trackPoolCount].title = ttitle;
              trackPool[trackPoolCount].file  = tfile;
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

static volatile bool autoAdvance = false; // Only true for album/playlist
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

  int v = (int)lroundf(currentVolume * 100.0f);
  if (v < 0)
    v = 0;
  if (v > 100)
    v = 100;

  prefs.putInt(PREF_KEY_VOL, v);
  volDirty = false;

  Serial.print("Saved volume: ");
  Serial.println(v);
}



// ================= GAME ENGINE START =================

// ---- Game data limits ----
static constexpr size_t MAX_GAMES = 10;
static constexpr size_t MAX_QUESTIONS = 40;
static constexpr size_t MAX_RULE_TAGS = 6;      // max tags in a rule
static constexpr size_t MAX_PENDING = 2;        // you want 1 or 2 cards
//static constexpr size_t MAX_CARD_TAGS = 8;      // max tags on an answer card
static uint32_t nextCardDueAt = 0;
static uint8_t repeatCount = 0;             // antal gange vi har gentaget spørgsmålet pga. inaktivitet
static uint8_t nextCardRepeatCount = 0;     // antal nextCard reminders i nuværende forsøg
static bool stopToMusicAfterAudio = false;  // når idleStop er afspillet, går vi til music

static bool doneAnnounced = false;

static bool gameNoticeActive = false;
static bool replayPromptAfterNotice = false;


enum class GameState : uint8_t {
  IDLE,
  INTRO,
  PROMPT,
  COLLECT,
  FEEDBACK,
  DONE
};

enum class RuleType : uint8_t {
  REQUIRE_TAGS,
  SUM
};

enum class MatchMode : uint8_t {
  ANY,
  ALL
};

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
  int equals = 0;           // target
  uint8_t cards = 1;        // required cards (default 1)
  String requireTags[MAX_RULE_TAGS];  // e.g. ["tal"]
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

// Forward declarations (you already have these helpers somewhere)
static void playPath(const String& path);   // your existing function that enqueues CMD_PLAY_FILE + persists lastPath
static const CardEntry* findCardByUid(const String& uid); // you already have
static String uidToHexUpper(const MFRC522::Uid& u);       // you already have

// ---- Utility ----
static MatchMode parseMode(const char* s) {
  if (!s) return MatchMode::ANY;
  if (strcmp(s, "all") == 0) return MatchMode::ALL;
  return MatchMode::ANY;
}

static bool hasTag(const PendingCard& c, const String& tag) {
  for (uint8_t i = 0; i < c.tagCount; i++) {
    if (c.tags[i] == tag) return true;
  }
  return false;
}

static bool unionHasTag(const String& unionTags, const String& tag) {
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
      if (u.indexOf(enc) < 0) u += enc;
    }
  }
  return u;
}

static bool allPendingHaveRequiredTags(const AnswerRule& r) {
  if (r.requireTagCount == 0) return true;
  for (uint8_t i = 0; i < pendingCount; i++) {
    for (uint8_t k = 0; k < r.requireTagCount; k++) {
      if (!hasTag(pending[i], r.requireTags[k])) return false;
    }
  }
  return true;
}

static bool evalRule(const AnswerRule& r) {
  if (pendingCount == 0) return false;

  if (r.type == RuleType::REQUIRE_TAGS) {
    // cards default 1
    uint8_t need = r.cards ? r.cards : 1;
    if (pendingCount < need) return false;

    String ut = buildUnionTags();

    if (r.mode == MatchMode::ANY) {
      for (uint8_t i = 0; i < r.tagCount; i++) {
        if (unionHasTag(ut, r.tags[i])) return true;
      }
      return false;
    } else { // ALL
      for (uint8_t i = 0; i < r.tagCount; i++) {
        if (!unionHasTag(ut, r.tags[i])) return false;
      }
      return true;
    }
  }

  if (r.type == RuleType::SUM) {
    uint8_t need = r.cards ? r.cards : 1;
    if (pendingCount < need) return false;

    if (!allPendingHaveRequiredTags(r)) return false;

    int s = 0;
    for (uint8_t i = 0; i < need; i++) {
      if (pending[i].value < 0) return false;
      s += pending[i].value;
    }
    return s == r.equals;
  }

  return false;
}

static const String& selectCorrectAudio(const GameDef& g, const Question& q) {
  if (q.audio.correct.length() > 0) return q.audio.correct;
  return g.audio.correct;
}

static const String& selectWrongAudio(const GameDef& g, const Question& q) {
  if (q.audio.wrong.length() > 0) return q.audio.wrong;
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
  nextCardDueAt = 0; //clearPending() will do this
  
  gameNoticeActive = false;
  replayPromptAfterNotice = false;
  clearPending();
}

static void gameStartById(const String& id) {
  // ---------- Stop music / playlist state ----------
  autoAdvance   = false;
  playlistEnded = false;
  advanceNeeded = false;
  advanceIndex  = -1;
  clearActivePlaylist();   // or: activeCount = 0; activeIndex = -1;

  repeatCount = 0;
  nextCardRepeatCount = 0;
  nextCardDueAt = 0;
  stopToMusicAfterAudio = false;


  // ---------- Hard reset of game runtime ----------
  gameNoticeActive = false;
  replayPromptAfterNotice = false;

  doneAnnounced = false;   // if you use it
  nextCardDueAt = 0;
  clearPending();

  gameModeActive = false;
  activeGameIdx  = -1;
  questionIdx    = 0;
  gameState      = GameState::IDLE;

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
  activeGameIdx  = idx;
  questionIdx    = 0;
  clearPending();

  const GameDef& g = games[activeGameIdx];

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
  if (!gameModeActive || activeGameIdx < 0) return;

  const GameDef& g = games[activeGameIdx];
   Serial.print("Play Music Hint 1");
  if (g.audio.musicHint.length() == 0) return;
   Serial.print("Play Music Hint 2");

  gameNoticeActive = true;
  replayPromptAfterNotice = alsoReplayPrompt;

  // Vi bruger FEEDBACK state for at "blokere" scanning mens beskeden spiller,
  // men vi rører ikke pendingCount osv.
  gameState = GameState::FEEDBACK;
 
  playPath(g.audio.musicHint);
}

static void gamePlayCurrentPrompt() {
  repeatCount = 0;
  nextCardRepeatCount = 0;
  nextCardDueAt = 0;
    if (activeGameIdx < 0) return;
  GameDef& g = games[activeGameIdx];
  if (questionIdx >= g.questionCount) {
    gameState = GameState::DONE;
    return;
  }

  

  clearPending();
  gameState = GameState::PROMPT;

  const Question& q = g.questions[questionIdx];
  Serial.print("Prompt q=");
  Serial.println(questionIdx);

  playPath(q.prompt);
}

static void gameOnAnswerScanned(const CardEntry& card) {
  // Accept answer cards only while collecting
  if (!gameModeActive) return;
  if (gameState != GameState::COLLECT) return;
  if (activeGameIdx < 0) return;

  GameDef& g = games[activeGameIdx];
  const Question& q = g.questions[questionIdx];
  const AnswerRule& r = q.rule;

  uint8_t need = r.cards ? r.cards : 1;
  if (need > MAX_PENDING) need = MAX_PENDING;

  if (pendingCount >= need) return; // already have enough

  // store
  PendingCard& p = pending[pendingCount];
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
      if (hasTag(pending[pendingCount-1], r.tags[i])) {
        contributes = true;
        break;
      }
    }
    if (!contributes) possible = false;
  }

  if (r.type == RuleType::SUM) {
    // must be a valid number card
    if (pending[pendingCount-1].value < 0) {
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
  gameState = GameState::FEEDBACK;   // temporarily block scans
  playPath(g.audio.nextCardForAnswer);

  // start timeout for "next card"
  nextCardDueAt = millis() + g.timing.nextCardRepeatMs;
  nextCardRepeatCount = 0; // første reminder er lige spillet "nu" (vi tæller kun gentagelser via timeout)
}

  return;
}

  if (pendingCount >= need) {
    // Evaluate immediately (no extra state needed)
    bool ok = evalRule(r);

    if (ok) {
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

static void gameTick(bool audioIsPlaying) {
  if (!gameModeActive) return;
  if (activeGameIdx < 0) return;

  GameDef& g = games[activeGameIdx];

  // Only advance when no audio is playing
  if (audioIsPlaying) return;

  // If we played idleStop and are supposed to return to music afterwards
  if (stopToMusicAfterAudio) {
    stopToMusicAfterAudio = false;
    gameEnterIdle();
    return;
  }

  // -------- TIMEOUT HANDLING while COLLECTING --------
  // We do timeouts only when we are waiting for cards (COLLECT) and no audio is playing.
  if (gameState == GameState::COLLECT && questionIdx < g.questionCount) {
    const Question& q = g.questions[questionIdx];
    uint8_t need = q.rule.cards ? q.rule.cards : 1;

    // Ensure sane maxRepeat
    uint8_t maxRepeat = (uint8_t)g.timing.maxRepeat;
    if (maxRepeat < 1) maxRepeat = 1;

    // nextCard max = maxRepeat-1, but minimum 1
    uint8_t maxNextCardRepeat = (maxRepeat > 1) ? (uint8_t)(maxRepeat - 1) : (uint8_t)1;

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

          clearPending();          // resets pendingCount, nextCardDueAt, nextCardRepeatCount
          gameState = GameState::PROMPT;
          playPath(q.prompt);

          // If the question itself has been repeated too many times -> stop game to music
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
      gamePlayCurrentPrompt(); // should set PROMPT state + play prompt + reset repeatCount
      break;

    case GameState::PROMPT:
      // prompt finished -> collect answers
      gameState = GameState::COLLECT;
      clearPending();
      // Do NOT reset repeatCount here; it counts how many times we repeated prompt due to inactivity.
      Serial.println("Collecting answers...");
      break;

    case GameState::FEEDBACK: {

      // 1) Notice overlay (music hint etc.)
      if (gameNoticeActive) {
        gameNoticeActive = false;

        if (replayPromptAfterNotice) {
          replayPromptAfterNotice = false;
          gameState = GameState::PROMPT;
          const Question& q = g.questions[questionIdx];
          playPath(q.prompt);
        } else {
          gameState = GameState::COLLECT;
        }
        return;
      }

      // 2) If we were waiting for more cards, resume collecting after the short prompt ends
      if (questionIdx < g.questionCount) {
        const Question& q = g.questions[questionIdx];
        if (pendingCount > 0 && pendingCount < (q.rule.cards ? q.rule.cards : 1)) {
          gameState = GameState::COLLECT;
          return;
        }
      }

      // 3) Normal correct/wrong feedback just finished -> advance or repeat
      const Question& q = g.questions[questionIdx];
      bool ok = evalRule(q.rule);

      if (ok) {
        questionIdx++;
        clearPending();
        nextCardDueAt = 0;
        nextCardRepeatCount = 0;
        repeatCount = 0; // new question starts fresh

        if (questionIdx >= g.questionCount) {
          gameState = GameState::DONE;
          if (g.audio.done.length() > 0) playPath(g.audio.done);
        } else {
          gamePlayCurrentPrompt(); // should reset repeatCount for new question
        }
      } else {
        // repeat same question (wrong)
        clearPending();
        nextCardDueAt = 0;
        nextCardRepeatCount = 0;
        // repeatCount NOT incremented here; it's for inactivity timeouts, not wrong answers
        gamePlayCurrentPrompt();
      }
      break;
    }

    case GameState::DONE:
      if (!doneAnnounced) {
        Serial.println("Game done. Waiting for MUSIC button or a new GAME");
        doneAnnounced = true;
      }
      // Stay in DONE; selector can restart via RFID handler
      break;

    default:
      break;
  }
}



// ---- JSON loading for games[] ----
// Call this after SD is ready and settings.json exists.
// It re-opens settings.json and parses only the "games" array.
static bool loadGamesJson(const char* jsonPath) {
  gameCount = 0;

  File f = SD.open(jsonPath, FILE_READ);
  if (!f) {
    Serial.print("Could not open JSON for games: ");
    Serial.println(jsonPath);
    return false;
  }

  DynamicJsonDocument doc(16384);
  DeserializationError err = deserializeJson(doc, f);
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
    if (gameCount >= MAX_GAMES) break;

    const char* id = g["id"] | "";
    if (strlen(id) == 0) continue;

    GameDef& gd = games[gameCount];
    gd.id = String(id);
    gd.questionCount = 0;

    // audio
    JsonObject audio = g["audio"].as<JsonObject>();
    if (!audio.isNull()) {
      gd.audio.intro   = String((const char*)(audio["intro"]   | ""));
      gd.audio.correct = String((const char*)(audio["correct"] | ""));
      gd.audio.wrong   = String((const char*)(audio["wrong"]   | ""));
      gd.audio.done    = String((const char*)(audio["done"]    | ""));
      gd.audio.nextCardForAnswer = String((const char*)(audio["nextCardForAnswer"] | ""));
      gd.audio.musicHint = String((const char*)(audio["musicHint"] | ""));
      gd.audio.idleStop = String((const char*)(audio["idleStop"] | ""));
    }
    Serial.print("Game "); Serial.print(gd.id);
    Serial.print(" idleStop="); Serial.println(gd.audio.idleStop);
    
    JsonObject timing = g["timing"].as<JsonObject>();
    if (!timing.isNull()) {
      gd.timing.answerTimeoutMs  = (uint32_t)(timing["answerTimeoutMs"]  | 25000);
      gd.timing.nextCardRepeatMs  = (uint32_t)(timing["nextCardRepeatMs"]  | 18000);
      gd.timing.maxRepeat  = (uint32_t)(timing["maxRepeat"]  | 3);
} else {
      gd.timing.answerTimeoutMs  = 25000;
      gd.timing.nextCardRepeatMs  = 18000;
      gd.timing.maxRepeat  = 3;
}

    // questions
    JsonArray qs = g["questions"].as<JsonArray>();
    if (!qs.isNull()) {
      for (JsonObject q : qs) {
        if (gd.questionCount >= MAX_QUESTIONS) break;

        Question& qq = gd.questions[gd.questionCount];
        qq.prompt = String((const char*)(q["prompt"] | ""));

        // question audio override (optional): audio.correct / audio.wrong
        JsonObject qa = q["audio"].as<JsonObject>();
        if (!qa.isNull()) {
          qq.audio.correct = String((const char*)(qa["correct"] | ""));
          qq.audio.wrong   = String((const char*)(qa["wrong"]   | ""));
        } else {
          qq.audio.correct = "";
          qq.audio.wrong = "";
        }

        // answer rule
        JsonObject a = q["answer"].as<JsonObject>();
        AnswerRule& r = qq.rule;

        r.cards = (uint8_t)(a["cards"] | 1);

        const char* type = a["type"] | "requireTags";
        if (strcmp(type, "requireTags") == 0) {
          r.type = RuleType::REQUIRE_TAGS;
          r.mode = parseMode(a["mode"] | "any");

          r.tagCount = 0;
          JsonArray tags = a["tags"].as<JsonArray>();
          if (!tags.isNull()) {
            for (JsonVariant tv : tags) {
              if (r.tagCount >= MAX_RULE_TAGS) break;
              r.tags[r.tagCount++] = String(tv.as<const char*>());
            }
          }
        }
        else if (strcmp(type, "sum") == 0) {
          r.type = RuleType::SUM;
          r.equals = (int)(a["equals"] | 0);

          // requireTags for sum (using "tags" field in your schema)
          r.requireTagCount = 0;
          JsonArray req = a["tags"].as<JsonArray>();
          if (!req.isNull()) {
            for (JsonVariant tv : req) {
              if (r.requireTagCount >= MAX_RULE_TAGS) break;
              r.requireTags[r.requireTagCount++] = String(tv.as<const char*>());
            }
          }
        }
        else {
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
          digitalWrite(PIN_LED_CARD, HIGH);
          delay(60);
          digitalWrite(PIN_LED_CARD, LOW);
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
    changeVolume(+VOL_STEP);
    Serial.println("BTN: VOL UP");
    break;
  case ACT_VOL_DOWN:
    changeVolume(-VOL_STEP);
    Serial.println("BTN: VOL DOWN");
    break;
  case ACT_MODE_MUSIC:
    Serial.println("MODE: MUSIC");
    gameEnterIdle();  // THIS is your rule: only music button exits game
    Serial.println("BTN: MODE MUSIC");
    break;
    /*
  case ACT_MODE_GAME_A:
    Serial.println("BTN: MODE GAME A");
    break;
  case ACT_MODE_GAME_B:
    Serial.println("BTN: MODE GAME B");
    break;
      */
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


void setup() {
  Serial.begin(115200);
  delay(200);

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
  digitalWrite(PIN_LED_CARD, LOW);

  Serial.println("Init SD...");
  if (!SD.begin(PIN_SD_CS, SPI, 8000000)) { // prøv 8MHz; hvis ustabilt: 4000000
    Serial.println("SD.begin FAILED");
    while (true)
      delay(1000);
  }
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

  // Load JSON
  loadCardsJson("/settings.json");
  loadGamesJson("/settings.json");   // games[]: rules + prompts + audio
  gameEnterIdle();     

  audioQ = xQueueCreate(8, sizeof(AudioCmd));
  xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 3, nullptr, 1);
}

void loop() {
  uint32_t now = millis();

  // 1) Button must run continuesly (no early returns)
  pollButtons(now);

  gameTick(isPlaying && !isPaused);

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

  // 2) RFID poll - Only each 25 ms
  static uint32_t lastPoll = 0;
  if (now - lastPoll < 25) {
    return; // OK to return here, since buttons are already scanned above
  }
  lastPoll = now;

  // Make sure SD is not choosen , while we communicate with RC522
  digitalWrite(PIN_SD_CS, HIGH);

  // If no new card -> turn off LED and leave
  if (!mfrc522.PICC_IsNewCardPresent()) {
    digitalWrite(PIN_LED_CARD, LOW);
    return;
  }

  // New card detected -> LED on instantly
  digitalWrite(PIN_LED_CARD, HIGH);

  if (!mfrc522.PICC_ReadCardSerial()) {
    // Card was detected, buth could not be read – Turn off LED again
    digitalWrite(PIN_LED_CARD, LOW);
    return;
  }

  String uid = uidToHexUpper(mfrc522.uid);

  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();

  const CardEntry *e = findCardByUid(uid);
  if (!e) {
    Serial.print("Unknown UID: ");
    Serial.println(uid);
    digitalWrite(PIN_LED_CARD, LOW);
    return;
  }
  
  Serial.print("Curent Mode is: "+e->role);
  if (e->role == "game_selector") {
   Serial.println("GAME SELECT: " + e->gameId);
    gameStartById(e->gameId);   // always abort current + start selected
    return;
  }

if (e->role == "answer") {
  gameOnAnswerScanned(*e);
  return;
}

if (e->role == "music") {
  if (gameModeActive) {
    gamePlayMusicHint(true);
    //Serial.println("Music ignored while game active");
    return;
  }

  if (e->kind == PK_SINGLE) {
    autoAdvance = false;
    playlistEnded = false;
    String path = e->file;
    if (!path.startsWith("/"))
      path = "/" + path;

    // Active playlist = folder for this file (so next/prev is in “same folder”)
    setActiveFromFolder(dirnameOf(path));
    // find index in activeTracks
    activeIndex = -1;
    for (size_t i = 0; i < activeCount; i++) {
      if (activeTracks[i] == path) {
        activeIndex = (int)i;
        break;
      }
    }
    if (activeIndex < 0)
      activeIndex = 0;

    playActiveIndex(activeIndex);
  } else if (e->kind == PK_ALBUM_FOLDER) {
    autoAdvance = true;
    playlistEnded = false;
    String folder = e->folder;
    if (!folder.startsWith("/"))
      folder = "/" + folder;

    setActiveFromFolder(folder);
    playActiveIndex(0);
  } else if (e->kind == PK_ALBUM_TRACKS) {
    autoAdvance = true;
    playlistEnded = false;
    setActiveFromTrackPool(e->trackStart, e->trackCount);
    playActiveIndex(0);
  } else {
    Serial.println("Music card missing play info");
  }
}

  Serial.print("UID ");
  Serial.print(uid);

  Serial.print("activeCount: ");
  Serial.print(activeCount);
  Serial.print("activeIndex: ");
  Serial.print(+activeIndex);
  Serial.print("activeTracks[0]: ");
  Serial.print(activeTracks[0]);
  // Turn off LED after play was queded
  digitalWrite(PIN_LED_CARD, LOW);
}
