/*
  PIXELDECK / FNK0104N
  A landscape LVGL arcade console for the Freenove FNK0104N.

  Built-in games are native firmware. The public store also loads bounded,
  validated Script API 2/3 cartridges; downloaded files never contain executable code.
*/

#include <Arduino.h>
#define FNK0104N_3P5_320x480_ST77922
#include <lvgl.h>
#include <ST77922.h>
#include <ST77922_Touch.h>
#include <FS.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <strings.h>
#include <Wire.h>
#include <ESP_I2S.h>
#include <Audio.h>
#include <TJpg_Decoder.h>
#include <esp_heap_caps.h>

static const uint16_t SCREEN_W = 480;
static const uint16_t SCREEN_H = 320;
static const uint16_t SD_CLK_PIN = 5;
static const uint16_t SD_CMD_PIN = 4;
static const uint16_t SD_D0_PIN = 6;
static const uint16_t SD_D1_PIN = 7;
static const uint16_t SD_D2_PIN = 2;
static const uint16_t SD_D3_PIN = 3;
static const char *STORE_ROOT_DEFAULT = "https://raw.githubusercontent.com/ziggyzag20/retro-esp/main/";
static const char *GAMES_DIR = "/ARCADE/GAMES";
static const uint8_t MAX_GAMES = 32;
static const uint8_t MAX_STORE_ITEMS = 24;
static const size_t MAX_CART_BYTES = 98304;
static const uint16_t MAX_SCRIPT_INSTRUCTIONS = 4096;
static const uint16_t MAX_SCRIPT_EVENT_STEPS = 8192;
static const uint8_t MAX_SCRIPT_REPEAT = 128;
static const uint8_t MAX_SCRIPT_NESTING = 16;
static const uint8_t MAX_SCRIPT_ENTITIES = 160;
static const uint8_t MAX_SCRIPT_VARIABLES = 128;
static const uint8_t MAX_SCRIPT_LABELS = 16;
static const uint8_t MAX_GAME_ASSETS = 8;
static const size_t MAX_ASSET_BYTES = 512 * 1024;
static const size_t MAX_TOTAL_ASSET_BYTES = 2 * 1024 * 1024;
static const uint32_t MAX_BITMAP_PIXELS = 65536;
static const uint8_t MAX_RENDER_ENTITIES = 160;
static const uint8_t I2S_MCLK_PIN = 17;
static const uint8_t I2S_BCLK_PIN = 18;
static const uint8_t I2S_DIN_PIN = 16;
static const uint8_t I2S_DOUT_PIN = 15;
static const uint8_t I2S_WS_PIN = 21;
static const uint8_t AUDIO_AMP_ENABLE_PIN = 1;
static const uint8_t AUDIO_I2C_SDA_PIN = 38;
static const uint8_t AUDIO_I2C_SCL_PIN = 39;

// The local LVGL configuration is RGB565 and already enables Montserrat 14/20.
static lv_color_t displayBuffer[SCREEN_W * 20];
static lv_disp_draw_buf_t displayDrawBuffer;
static lv_disp_drv_t displayDriver;
static lv_indev_drv_t touchDriver;
static ST77922 *panel = nullptr;
static ST77922_TOUCH *touch = nullptr;
static Preferences preferences;
static bool sdReady = false;
static bool wifiAttemptActive = false;
static uint32_t wifiAttemptStarted = 0;
static String savedSsid;
static String savedPassword;
static String savedStoreRoot;

enum GameType : uint8_t { GAME_NONE, GAME_SNAKE, GAME_PONG, GAME_BREAKOUT, GAME_INVADERS, GAME_SCRIPT };
enum ScreenMode : uint8_t { SCREEN_HOME, SCREEN_GAMES, SCREEN_STORE, SCREEN_SETTINGS, SCREEN_HELP, SCREEN_PLAY };

struct GameAssetDef {
  char name[16];
  char filename[40];
};

struct GameCard {
  char id[24];
  char title[40];
  char description[72];
  GameType type;
  uint8_t speed;
  uint16_t tickMs;
  uint32_t accent;
  bool builtIn;
  uint8_t assetCount;
  GameAssetDef assets[MAX_GAME_ASSETS];
};

enum ScriptEvent : uint8_t {
  SCRIPT_NO_EVENT, SCRIPT_START, SCRIPT_UPDATE,
  SCRIPT_LEFT_DOWN, SCRIPT_LEFT_UP, SCRIPT_UP_DOWN, SCRIPT_UP_UP,
  SCRIPT_DOWN_DOWN, SCRIPT_DOWN_UP, SCRIPT_RIGHT_DOWN, SCRIPT_RIGHT_UP,
  SCRIPT_A, SCRIPT_B
};

enum ScriptOp : uint8_t {
  SCRIPT_OP_INVALID, SCRIPT_OP_IF, SCRIPT_OP_ELSE, SCRIPT_OP_ENDIF, SCRIPT_OP_REPEAT, SCRIPT_OP_ENDREPEAT,
  SCRIPT_OP_SET, SCRIPT_OP_ADD, SCRIPT_OP_SUB, SCRIPT_OP_MUL, SCRIPT_OP_DIV,
  SCRIPT_OP_MOD, SCRIPT_OP_RANDOM, SCRIPT_OP_RECT, SCRIPT_OP_MOVE,
  SCRIPT_OP_POSITION, SCRIPT_OP_VELOCITY, SCRIPT_OP_PHYSICS,
  SCRIPT_OP_COLLIDE, SCRIPT_OP_GETX, SCRIPT_OP_GETY, SCRIPT_OP_GETVX,
  SCRIPT_OP_GETVY, SCRIPT_OP_EXISTS, SCRIPT_OP_HIDE, SCRIPT_OP_SHOW,
  SCRIPT_OP_REMOVE, SCRIPT_OP_TWEEN, SCRIPT_OP_LABEL, SCRIPT_OP_TEXT,
  SCRIPT_OP_CLEARLABEL, SCRIPT_OP_IMAGE, SCRIPT_OP_MUSIC, SCRIPT_OP_SFX,
  SCRIPT_OP_AUDIO_STOP, SCRIPT_OP_AUDIO_VOLUME, SCRIPT_OP_CIRCLE,
  SCRIPT_OP_BACKGROUND, SCRIPT_OP_ALPHA,
  SCRIPT_OP_OVER, SCRIPT_OP_WIN
};

struct ScriptInstruction {
  uint32_t sourceOffset;
  uint16_t jump;
  uint8_t event;
  uint8_t op;
};

struct ScriptEntity {
  lv_obj_t *object;
  float x, y, vx, vy;
  int16_t w, h;
  uint32_t tint;
  uint8_t radius;
  uint8_t bitmapIndex;
  uint8_t opacity;
  uint16_t zoom;
  bool active, visible, wrap, tweening, isImage, imageObject;
  float fromX, fromY, targetX, targetY;
  uint32_t tweenStarted, tweenDuration;
};

struct ScriptVariable { char name[16]; int32_t value; bool used; };

struct BitmapAsset {
  char name[16];
  lv_img_dsc_t image;
  lv_color_t *pixels;
  uint16_t width, height;
  bool loaded;
};

struct StoreItem {
  GameCard game;
  bool installed;
};

static GameCard games[MAX_GAMES];
static uint8_t gameCount = 0;
static StoreItem storeItems[MAX_STORE_ITEMS];
static uint8_t storeCount = 0;
static char libraryNotice[96] = {};
static ScreenMode screenMode = SCREEN_HOME;
static GameCard activeGame;
static char *scriptProgram = nullptr;
static size_t scriptProgramCapacity = 0;
static ScriptInstruction scriptCode[MAX_SCRIPT_INSTRUCTIONS];
static uint16_t scriptCodeCount = 0;
static uint32_t scriptCompileErrorLine = 0;
static ScriptEntity scriptEntities[MAX_SCRIPT_ENTITIES];
static ScriptVariable scriptVariables[MAX_SCRIPT_VARIABLES];
static lv_obj_t *scriptLabels[MAX_SCRIPT_LABELS];
static BitmapAsset bitmapAssets[MAX_GAME_ASSETS];
static uint8_t bitmapAssetCount = 0;
static Audio audio;
static I2SClass es8311_i2s;
static bool audioReady = false;
static uint8_t consoleVolume = 10;
static char backgroundAudioName[16] = {};
static uint8_t backgroundAudioVolume = 10;
static bool effectAudioActive = false;
static bool backgroundAudioLoops = false;
static uint8_t scriptLevel = 1;
static uint32_t scriptStartedAt = 0;
static int16_t pointerX = 0, pointerY = 0;
static bool pointerDown = false;

static lv_obj_t *statusWifi = nullptr;
static lv_obj_t *statusSd = nullptr;
static lv_obj_t *storeStatus = nullptr;
static lv_obj_t *storeGrid = nullptr;
static lv_obj_t *gameStage = nullptr;
static lv_obj_t *gameScoreLabel = nullptr;
static lv_obj_t *gameHint = nullptr;
static lv_obj_t *pauseButtonLabel = nullptr;
static lv_obj_t *keyboard = nullptr;
static lv_obj_t *ssidField = nullptr;
static lv_obj_t *passwordField = nullptr;
static lv_obj_t *storeField = nullptr;
static lv_obj_t *settingsStatus = nullptr;
static lv_obj_t *entities[MAX_RENDER_ENTITIES];

static bool controls[4] = {false, false, false, false}; // left, up, down, right
static bool actionA = false, actionB = false;
static bool paused = false;
static bool gameOver = false;
static bool shotActive = false;
static uint32_t score = 0;
static uint32_t bestScore = 0;
static uint32_t lastGameTick = 0;
static uint32_t lastStatusTick = 0;
static uint32_t lastStarTick = 0;

// Snake state
static int snakeX[32], snakeY[32], snakeLength = 4, snakeDirection = 3, nextDirection = 3;
static int foodX = 15, foodY = 5;

// Shared arcade physics
static float ballX = 220, ballY = 90, ballVX = 4, ballVY = 3;
static float paddleX = 190, paddleY = 70, opponentY = 70;
static uint8_t lives = 3;
static int bricksRemaining = 32;

// Invaders state
static int alienX[28], alienY[28], alienDirection = 1, alienMoveCounter = 0;
static bool alienAlive[28];
static int shipX = 210, bulletX = 0, bulletY = 0;

static const uint32_t C_BG = 0x070B14;
static const uint32_t C_PANEL = 0x101827;
static const uint32_t C_PANEL_HI = 0x17243A;
static const uint32_t C_CYAN = 0x56E5FF;
static const uint32_t C_MAGENTA = 0xF356B8;
static const uint32_t C_LIME = 0xB9F66A;
static const uint32_t C_AMBER = 0xFFC857;
static const uint32_t C_WHITE = 0xF4F7FF;
static const uint32_t C_MUTED = 0x8D9BB4;
static const uint32_t C_RED = 0xFF5D73;

static void showHome();
static void showGames();
static void showStore();
static void showSettings();
static void showHelp();
static void startGame(const GameCard *game);
static void resetGame();
static void refreshStatus();
static void onBack(lv_event_t *event);
static bool compileScript(const char *source, size_t length);
static void runScriptEvent(ScriptEvent event);
static void updateScriptTweens();
static void resetScriptRuntime();
static bool parseCart(const String &payload, GameCard &card);
static bool beginAudio();
static void stopGameAudio();
static void playGameAudio(const char *assetName, bool loopFile, uint8_t volume, bool effect = false);
static void freeBitmapAssets();
static void loadGameBitmapAssets();

static lv_color_t color(uint32_t hex) { return lv_color_hex(hex); }

static void copyText(char *out, size_t capacity, const String &value) {
  if (capacity == 0) return;
  value.toCharArray(out, capacity);
}

static void copyLiteral(char *out, size_t capacity, const char *value) {
  if (capacity == 0) return;
  strncpy(out, value, capacity - 1);
  out[capacity - 1] = '\0';
}

static bool es8311WriteRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(0x18);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

// FNK0104N uses an ES8311 codec. This small DAC-only setup follows Freenove's
// published 44.1 kHz, 16-bit I2S configuration while keeping the driver local.
static bool configureEs8311() {
  static const uint8_t setup[][2] = {
    {0x00, 0x1F}, {0x00, 0x00}, {0x00, 0x80},
    {0x01, 0x3F}, {0x02, 0x00}, {0x03, 0x10}, {0x04, 0x10},
    {0x05, 0x00}, {0x06, 0x03}, {0x07, 0x00}, {0x08, 0xFF},
    {0x09, 0x0C}, {0x0A, 0x0C}, {0x0D, 0x01}, {0x0E, 0x02},
    {0x12, 0x00}, {0x13, 0x10}, {0x1C, 0x6A}, {0x32, 0xD8}, {0x37, 0x08}
  };
  for (size_t i = 0; i < sizeof(setup) / sizeof(setup[0]); ++i) {
    if (!es8311WriteRegister(setup[i][0], setup[i][1])) return false;
    if (i == 0) delay(20);
  }
  return true;
}

static bool beginAudio() {
  pinMode(AUDIO_AMP_ENABLE_PIN, OUTPUT);
  digitalWrite(AUDIO_AMP_ENABLE_PIN, LOW);
  Wire.begin(AUDIO_I2C_SDA_PIN, AUDIO_I2C_SCL_PIN, 400000);
  es8311_i2s.setPins(I2S_BCLK_PIN, I2S_WS_PIN, I2S_DOUT_PIN, I2S_DIN_PIN, I2S_MCLK_PIN);
  if (!es8311_i2s.begin(I2S_MODE_STD, 44100, I2S_DATA_BIT_WIDTH_16BIT,
                       I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_LEFT)) {
    Serial.println("[AUDIO] FNK0104N I2S initialization failed.");
    return false;
  }
  if (!configureEs8311()) {
    Serial.println("[AUDIO] ES8311 did not acknowledge its setup registers.");
    return false;
  }
  audio.setPinout(I2S_BCLK_PIN, I2S_WS_PIN, I2S_DOUT_PIN, I2S_MCLK_PIN);
  audio.setVolume(consoleVolume);
  Serial.println("[AUDIO] FNK0104N speaker path ready.");
  return true;
}

static void stopGameAudio() {
  if (audioReady && audio.isRunning()) audio.stopSong();
  backgroundAudioName[0] = '\0';
  effectAudioActive = false;
  backgroundAudioLoops = false;
}

static const GameAssetDef *findGameAsset(const char *name) {
  if (!name) return nullptr;
  for (uint8_t i = 0; i < activeGame.assetCount; ++i) {
    if (!strcasecmp(activeGame.assets[i].name, name)) return &activeGame.assets[i];
  }
  return nullptr;
}

static bool isImageAsset(const char *filename) {
  String name(filename ? filename : "");
  name.toLowerCase();
  return name.endsWith(".jpg") || name.endsWith(".jpeg") || name.endsWith(".bmp");
}

static bool isAudioAsset(const char *filename) {
  String name(filename ? filename : "");
  name.toLowerCase();
  return name.endsWith(".mp3") || name.endsWith(".wav") || name.endsWith(".aac") ||
    name.endsWith(".flac") || name.endsWith(".m4a") || name.endsWith(".ogg") ||
    name.endsWith(".oga") || name.endsWith(".opus");
}

static bool validAssetName(const String &name) {
  if (name.length() == 0 || name.length() >= 16) return false;
  for (size_t i = 0; i < name.length(); ++i) {
    const char c = name[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
  }
  return true;
}

static bool validAssetFilename(const String &filename) {
  if (filename.length() == 0 || filename.length() >= 40 ||
      filename.indexOf('/') >= 0 || filename.indexOf('\\') >= 0 ||
      filename.indexOf("..") >= 0) return false;
  for (size_t i = 0; i < filename.length(); ++i) {
    const char c = filename[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) return false;
  }
  const bool image = isImageAsset(filename.c_str());
  const bool audioFile = isAudioAsset(filename.c_str());
  if (!image && !audioFile) return false;
  // ESP32-audioI2S 3.0.13 selects its decoder by a case-sensitive extension.
  if (audioFile) {
    String extension = filename.substring(filename.lastIndexOf('.'));
    String lowercaseExtension = extension;
    lowercaseExtension.toLowerCase();
    if (extension != lowercaseExtension) return false;
  }
  return true;
}

struct JpegDecodeTarget { lv_color_t *pixels; uint16_t width, height; };
static JpegDecodeTarget *jpegTarget = nullptr;

static bool jpegToLvglImage(int16_t x, int16_t y, uint16_t width, uint16_t height, uint16_t *data) {
  if (!jpegTarget || !jpegTarget->pixels || !data) return false;
  for (uint16_t row = 0; row < height; ++row) {
    const uint16_t py = (uint16_t)max(0, (int)y + row);
    if (py >= jpegTarget->height) continue;
    for (uint16_t col = 0; col < width; ++col) {
      const uint16_t px = (uint16_t)max(0, (int)x + col);
      if (px >= jpegTarget->width) continue;
      const uint16_t rgb = data[(size_t)row * width + col];
      const uint8_t red = (uint8_t)(((rgb >> 11) & 0x1F) * 255 / 31);
      const uint8_t green = (uint8_t)(((rgb >> 5) & 0x3F) * 255 / 63);
      const uint8_t blue = (uint8_t)((rgb & 0x1F) * 255 / 31);
      jpegTarget->pixels[(size_t)py * jpegTarget->width + px] = lv_color_make(red, green, blue);
    }
  }
  return true;
}

static uint16_t readLittle16(const uint8_t *bytes) {
  return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint32_t readLittle32(const uint8_t *bytes) {
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
    ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static bool decodeBmpAsset(const String &path, BitmapAsset &bitmap) {
  File file = SD_MMC.open(path, FILE_READ);
  if (!file || file.isDirectory() || file.size() < 54) { if (file) file.close(); return false; }
  uint8_t header[54];
  if (file.read(header, sizeof(header)) != sizeof(header) || header[0] != 'B' || header[1] != 'M') {
    file.close();
    return false;
  }
  const uint32_t dataOffset = readLittle32(header + 10);
  const int32_t signedWidth = (int32_t)readLittle32(header + 18);
  const int32_t signedHeight = (int32_t)readLittle32(header + 22);
  const uint16_t bitsPerPixel = readLittle16(header + 28);
  const uint32_t compression = readLittle32(header + 30);
  if (signedWidth <= 0 || signedHeight == 0 || signedHeight == INT32_MIN ||
      signedWidth > 2047 || abs(signedHeight) > 2047 || readLittle16(header + 26) != 1 ||
      bitsPerPixel != 24 || compression != 0) {
    file.close();
    return false;
  }
  const uint16_t sourceWidth = (uint16_t)signedWidth;
  const uint16_t sourceHeight = (uint16_t)abs(signedHeight);
  uint8_t scale = 1;
  while (scale < 8 && ((uint32_t)((sourceWidth + scale - 1) / scale) *
                       ((sourceHeight + scale - 1) / scale) > MAX_BITMAP_PIXELS ||
                       (sourceWidth + scale - 1) / scale > 512 ||
                       (sourceHeight + scale - 1) / scale > 512)) scale *= 2;
  bitmap.width = (uint16_t)((sourceWidth + scale - 1) / scale);
  bitmap.height = (uint16_t)((sourceHeight + scale - 1) / scale);
  if ((uint32_t)bitmap.width * bitmap.height > MAX_BITMAP_PIXELS) { file.close(); return false; }
  const size_t bytes = (size_t)bitmap.width * bitmap.height * sizeof(lv_color_t);
  bitmap.pixels = (lv_color_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!bitmap.pixels) bitmap.pixels = (lv_color_t *)malloc(bytes);
  if (!bitmap.pixels) { file.close(); return false; }
  const uint32_t rowBytes = ((uint32_t)sourceWidth * 3 + 3) & ~3UL;
  uint8_t row[6144];
  bool ok = true;
  for (uint16_t outY = 0; outY < bitmap.height && ok; ++outY) {
    const uint16_t sourceY = (uint16_t)(outY * scale);
    const uint16_t fileY = signedHeight > 0 ? (sourceHeight - 1 - sourceY) : sourceY;
    if (!file.seek(dataOffset + (uint32_t)fileY * rowBytes) || file.read(row, rowBytes) != rowBytes) {
      ok = false;
      break;
    }
    for (uint16_t outX = 0; outX < bitmap.width; ++outX) {
      const uint16_t sourceX = (uint16_t)(outX * scale);
      const uint8_t *pixel = row + (size_t)sourceX * 3;
      bitmap.pixels[(size_t)outY * bitmap.width + outX] = lv_color_make(pixel[2], pixel[1], pixel[0]);
    }
  }
  file.close();
  if (!ok) { free(bitmap.pixels); bitmap.pixels = nullptr; return false; }
  bitmap.image.header.cf = LV_IMG_CF_TRUE_COLOR;
  bitmap.image.header.always_zero = 0;
  bitmap.image.header.reserved = 0;
  bitmap.image.header.w = bitmap.width;
  bitmap.image.header.h = bitmap.height;
  bitmap.image.data_size = (uint32_t)bytes;
  bitmap.image.data = (const uint8_t *)bitmap.pixels;
  bitmap.loaded = true;
  return true;
}

static void freeBitmapAssets() {
  for (uint8_t i = 0; i < MAX_GAME_ASSETS; ++i) {
    if (bitmapAssets[i].pixels) free(bitmapAssets[i].pixels);
    memset(&bitmapAssets[i], 0, sizeof(bitmapAssets[i]));
  }
  bitmapAssetCount = 0;
  jpegTarget = nullptr;
}

static void loadGameBitmapAssets() {
  freeBitmapAssets();
  if (!sdReady || activeGame.type != GAME_SCRIPT) return;
  bitmapAssetCount = activeGame.assetCount;
  TJpgDec.setSwapBytes(false);
  TJpgDec.setCallback(jpegToLvglImage);
  for (uint8_t i = 0; i < activeGame.assetCount; ++i) {
    const GameAssetDef &definition = activeGame.assets[i];
    if (!isImageAsset(definition.filename)) continue;
    BitmapAsset &bitmap = bitmapAssets[i];
    copyLiteral(bitmap.name, sizeof(bitmap.name), definition.name);
    const String path = String(GAMES_DIR) + "/" + activeGame.id + "/assets/" + definition.filename;
    String imageFilename(definition.filename);
    imageFilename.toLowerCase();
    if (imageFilename.endsWith(".bmp")) {
      if (!decodeBmpAsset(path, bitmap)) Serial.printf("[BITMAP] Could not decode %s\n", path.c_str());
      continue;
    }
    uint16_t sourceWidth = 0, sourceHeight = 0;
    if (TJpgDec.getFsJpgSize(&sourceWidth, &sourceHeight, path, SD_MMC) != JDR_OK ||
        sourceWidth == 0 || sourceHeight == 0 || sourceWidth > 2048 || sourceHeight > 2048) continue;
    uint8_t scale = 1;
    while (scale < 8 && ((uint32_t)(sourceWidth / scale) * (sourceHeight / scale) > MAX_BITMAP_PIXELS ||
                         sourceWidth / scale > 512 || sourceHeight / scale > 512)) scale *= 2;
    bitmap.width = max((uint16_t)1, (uint16_t)((sourceWidth + scale - 1) / scale));
    bitmap.height = max((uint16_t)1, (uint16_t)((sourceHeight + scale - 1) / scale));
    if ((uint32_t)bitmap.width * bitmap.height > MAX_BITMAP_PIXELS) continue;
    bitmap.pixels = (lv_color_t *)heap_caps_malloc((size_t)bitmap.width * bitmap.height * sizeof(lv_color_t),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!bitmap.pixels) bitmap.pixels = (lv_color_t *)malloc((size_t)bitmap.width * bitmap.height * sizeof(lv_color_t));
    if (!bitmap.pixels) continue;
    memset(bitmap.pixels, 0, (size_t)bitmap.width * bitmap.height * sizeof(lv_color_t));
    JpegDecodeTarget target = {bitmap.pixels, bitmap.width, bitmap.height};
    jpegTarget = &target;
    TJpgDec.setJpgScale(scale);
    const JRESULT result = TJpgDec.drawFsJpg(0, 0, path, SD_MMC);
    jpegTarget = nullptr;
    if (result != JDR_OK) {
      free(bitmap.pixels);
      memset(&bitmap, 0, sizeof(bitmap));
      continue;
    }
    bitmap.image.header.cf = LV_IMG_CF_TRUE_COLOR;
    bitmap.image.header.always_zero = 0;
    bitmap.image.header.reserved = 0;
    bitmap.image.header.w = bitmap.width;
    bitmap.image.header.h = bitmap.height;
    bitmap.image.data_size = (uint32_t)bitmap.width * bitmap.height * sizeof(lv_color_t);
    bitmap.image.data = (const uint8_t *)bitmap.pixels;
    bitmap.loaded = true;
  }
  TJpgDec.setJpgScale(1);
}

static void playGameAudio(const char *assetName, bool loopFile, uint8_t volume, bool effect) {
  if (!audioReady || !sdReady || activeGame.type != GAME_SCRIPT) return;
  const GameAssetDef *asset = findGameAsset(assetName);
  if (!asset || !isAudioAsset(asset->filename)) return;
  const String path = String(GAMES_DIR) + "/" + activeGame.id + "/assets/" + asset->filename;
  if (!SD_MMC.exists(path)) return;
  if (effect) effectAudioActive = true;
  else {
    copyLiteral(backgroundAudioName, sizeof(backgroundAudioName), assetName);
    backgroundAudioVolume = volume;
    backgroundAudioLoops = loopFile;
    effectAudioActive = false;
  }
  audio.stopSong();
  audio.setVolume(constrain(volume, 0, 21));
  audio.setFileLoop(loopFile);
  if (!audio.connecttoFS(SD_MMC, path.c_str())) {
    Serial.printf("[AUDIO] Could not open %s\n", path.c_str());
  }
}

static bool validId(const String &id) {
  if (id.length() == 0 || id.length() >= 24) return false;
  for (size_t i = 0; i < id.length(); ++i) {
    const char c = id[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
  }
  return true;
}

static GameType parseGameType(String type) {
  type.trim();
  type.toUpperCase();
  if (type == "SNAKE") return GAME_SNAKE;
  if (type == "PONG") return GAME_PONG;
  if (type == "BREAKOUT") return GAME_BREAKOUT;
  if (type == "INVADERS") return GAME_INVADERS;
  if (type == "SCRIPT" || type == "PIXELDECK") return GAME_SCRIPT;
  return GAME_NONE;
}

static const char *gameTypeName(GameType type) {
  switch (type) {
    case GAME_SNAKE: return "SNAKE";
    case GAME_PONG: return "PONG";
    case GAME_BREAKOUT: return "BREAKOUT";
    case GAME_INVADERS: return "INVADERS";
    case GAME_SCRIPT: return "SCRIPT V2";
    default: return "ARCADE";
  }
}

static bool sameId(const char *a, const char *b) {
  return strcasecmp(a, b) == 0;
}

static void addGame(const GameCard &card) {
  if (card.type == GAME_NONE || !validId(String(card.id))) return;
  for (uint8_t i = 0; i < gameCount; ++i) {
    if (sameId(games[i].id, card.id)) {
      if (!games[i].builtIn && card.builtIn) games[i] = card;
      return;
    }
  }
  if (gameCount < MAX_GAMES) games[gameCount++] = card;
}

static void addBuiltIn(const char *id, const char *title, const char *description,
                       GameType type, uint8_t speed, uint32_t accent) {
  GameCard card = {};
  copyLiteral(card.id, sizeof(card.id), id);
  copyLiteral(card.title, sizeof(card.title), title);
  copyLiteral(card.description, sizeof(card.description), description);
  card.type = type;
  card.speed = speed;
  card.tickMs = 0;
  card.accent = accent;
  card.builtIn = true;
  addGame(card);
}

static String highScoreKey(const GameCard &game) {
  uint32_t hash = 2166136261UL;
  for (const char *p = game.id; *p; ++p) hash = (hash ^ (uint8_t)*p) * 16777619UL;
  char key[10];
  snprintf(key, sizeof(key), "h%08lX", (unsigned long)hash);
  return String(key);
}

static uint32_t highScoreFor(const GameCard &game) {
  const String key = highScoreKey(game);
  return preferences.getUInt(key.c_str(), 0);
}

static void saveHighScore() {
  if (score <= bestScore) return;
  bestScore = score;
  const String key = highScoreKey(activeGame);
  preferences.putUInt(key.c_str(), bestScore);
}

static lv_obj_t *makeLabel(lv_obj_t *parent, const char *text, int x, int y, int w, int h,
                           uint32_t textColor = C_WHITE, const lv_font_t *font = &lv_font_montserrat_14) {
  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, text);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_set_pos(label, x, y);
  lv_obj_set_size(label, w, h);
  lv_obj_set_style_text_color(label, color(textColor), 0);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_line_space(label, 2, 0);
  return label;
}

static lv_obj_t *makeButton(lv_obj_t *parent, const char *text, int x, int y, int w, int h,
                            uint32_t fill, lv_event_cb_t callback = nullptr, void *userData = nullptr,
                            const lv_font_t *font = &lv_font_montserrat_14) {
  lv_obj_t *button = lv_btn_create(parent);
  lv_obj_set_pos(button, x, y);
  lv_obj_set_size(button, w, h);
  lv_obj_set_style_bg_color(button, color(fill), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(button, color(fill), LV_PART_MAIN);
  lv_obj_set_style_radius(button, 8, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(button, 4, LV_PART_MAIN);
  lv_obj_t *label = lv_label_create(button);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(label, color(C_WHITE), 0);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_center(label);
  if (callback) lv_obj_add_event_cb(button, callback, LV_EVENT_ALL, userData);
  return button;
}

static void screenBackground() {
  lv_obj_t *screen = lv_scr_act();
  lv_obj_set_style_bg_color(screen, color(C_BG), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(screen, 0, 0);
  lv_obj_set_style_border_width(screen, 0, 0);
  lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
}

static void topBar(const char *title, bool back, bool showStatus = true) {
  lv_obj_t *bar = lv_obj_create(lv_scr_act());
  lv_obj_set_pos(bar, 0, 0);
  lv_obj_set_size(bar, SCREEN_W, 42);
  lv_obj_set_style_bg_color(bar, color(C_PANEL), 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(bar, 0, 0);
  lv_obj_set_style_radius(bar, 0, 0);
  lv_obj_set_style_pad_all(bar, 0, 0);
  if (back) makeButton(bar, "<", 8, 6, 42, 30, C_PANEL_HI, onBack);
  makeLabel(bar, title, back ? 58 : 16, 10, back ? 244 : 276, 22, C_CYAN, &lv_font_montserrat_14);
  if (showStatus) {
    statusWifi = makeLabel(bar, "-- WIFI", 314, 11, 78, 20, C_MUTED);
    statusSd = makeLabel(bar, "-- SD", 397, 11, 72, 20, C_MUTED);
    refreshStatus();
  }
}

static void onBack(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  if (screenMode == SCREEN_PLAY) {
    stopGameAudio();
    showGames();
    freeBitmapAssets();
  }
  else showHome();
}

static void refreshStatus() {
  if (statusWifi) {
    const bool online = WiFi.status() == WL_CONNECTED;
    lv_label_set_text(statusWifi, online ? "WIFI ON" : "WIFI --");
    lv_obj_set_style_text_color(statusWifi, color(online ? C_LIME : C_MUTED), 0);
  }
  if (statusSd) {
    lv_label_set_text(statusSd, sdReady ? "SD READY" : "NO SD");
    lv_obj_set_style_text_color(statusSd, color(sdReady ? C_LIME : C_AMBER), 0);
  }
}

static void clearUiRefs() {
  statusWifi = nullptr;
  statusSd = nullptr;
  storeStatus = nullptr;
  storeGrid = nullptr;
  gameStage = nullptr;
  gameScoreLabel = nullptr;
  gameHint = nullptr;
  pauseButtonLabel = nullptr;
  keyboard = nullptr;
  ssidField = nullptr;
  passwordField = nullptr;
  storeField = nullptr;
  settingsStatus = nullptr;
}

static void onMenuCard(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  const ScreenMode next = (ScreenMode)(intptr_t)lv_event_get_user_data(event);
  if (next == SCREEN_GAMES) showGames();
  else if (next == SCREEN_STORE) showStore();
  else if (next == SCREEN_SETTINGS) showSettings();
  else showHelp();
}

static void homeCard(const char *title, const char *subtitle, int x, int y,
                     uint32_t accent, ScreenMode target) {
  lv_obj_t *card = lv_btn_create(lv_scr_act());
  lv_obj_set_pos(card, x, y);
  lv_obj_set_size(card, 210, 72);
  lv_obj_set_style_bg_color(card, color(C_PANEL), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, color(accent), 0);
  lv_obj_set_style_radius(card, 10, 0);
  lv_obj_set_style_shadow_width(card, 0, 0);
  lv_obj_add_event_cb(card, onMenuCard, LV_EVENT_CLICKED, (void *)(intptr_t)target);
  makeLabel(card, title, 13, 10, 184, 22, accent, &lv_font_montserrat_20);
  makeLabel(card, subtitle, 13, 39, 184, 24, C_MUTED);
}

static void showHome() {
  screenMode = SCREEN_HOME;
  clearUiRefs();
  lv_obj_clean(lv_scr_act());
  screenBackground();
  topBar("PIXELDECK  //  FNK0104N", false);
  makeLabel(lv_scr_act(), "NOVA ARCADE", 24, 58, 300, 36, C_WHITE, &lv_font_montserrat_20);
  makeLabel(lv_scr_act(), "A pocket-sized cabinet. Four original games. Your next high score starts here.",
            24, 91, 432, 24, C_MUTED);
  homeCard("PLAY", "Built-in + installed games", 24, 126, C_CYAN, SCREEN_GAMES);
  homeCard("GAME STORE", "Fetch new carts from GitHub", 246, 126, C_MAGENTA, SCREEN_STORE);
  homeCard("SETTINGS", "Wi-Fi, store and save data", 24, 210, C_LIME, SCREEN_SETTINGS);
  homeCard("HOW TO PLAY", "Controls, SD card and setup", 246, 210, C_AMBER, SCREEN_HELP);
  makeLabel(lv_scr_act(), "TOUCH TO START     //     MADE FOR FNK0104N", 24, 294, 432, 18, C_MUTED);
}

static void onStartCard(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    startGame((const GameCard *)lv_event_get_user_data(event));
  }
}

static void showGames() {
  screenMode = SCREEN_GAMES;
  clearUiRefs();
  lv_obj_clean(lv_scr_act());
  screenBackground();
  topBar("CARTRIDGE LIBRARY", true);
  makeLabel(lv_scr_act(), libraryNotice[0] ? libraryNotice : "SELECT A GAME",
            18, 50, 440, 22, libraryNotice[0] ? C_AMBER : C_MUTED);

  lv_obj_t *grid = lv_obj_create(lv_scr_act());
  lv_obj_set_pos(grid, 14, 76);
  lv_obj_set_size(grid, 452, 230);
  lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(grid, 0, 0);
  lv_obj_set_style_pad_all(grid, 3, 0);
  lv_obj_set_style_pad_row(grid, 8, 0);
  lv_obj_set_style_pad_column(grid, 8, 0);
  lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_scroll_dir(grid, LV_DIR_VER);

  for (uint8_t i = 0; i < gameCount; ++i) {
    lv_obj_t *card = lv_btn_create(grid);
    lv_obj_set_size(card, 210, 82);
    lv_obj_set_style_bg_color(card, color(C_PANEL), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, color(games[i].accent), 0);
    lv_obj_set_style_radius(card, 9, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_add_event_cb(card, onStartCard, LV_EVENT_CLICKED, &games[i]);
    makeLabel(card, games[i].title, 10, 8, 188, 21, games[i].accent, &lv_font_montserrat_20);
    makeLabel(card, gameTypeName(games[i].type), 10, 35, 78, 18, C_MUTED);
    makeLabel(card, games[i].builtIn ? "BUILT IN" : "SD CART", 130, 35, 68, 18, C_LIME);
    makeLabel(card, games[i].description, 10, 55, 188, 20, C_WHITE);
  }
}

static void onPause(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED || screenMode != SCREEN_PLAY) return;
  paused = !paused;
  lv_label_set_text(pauseButtonLabel, paused ? ">" : "II");
  if (gameHint) {
    lv_label_set_text(gameHint, paused ? "PAUSED" : "");
    if (paused) lv_obj_clear_flag(gameHint, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(gameHint, LV_OBJ_FLAG_HIDDEN);
  }
}

static void drawFlush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *pixels) {
  if (panel) {
    const uint16_t w = (uint16_t)(area->x2 - area->x1 + 1);
    const uint16_t h = (uint16_t)(area->y2 - area->y1 + 1);
    panel->Fill_Colors((uint16_t)area->x1, (uint16_t)area->y1, w, h, (uint16_t *)pixels);
  }
  lv_disp_flush_ready(driver);
}

static void readTouch(lv_indev_drv_t *, lv_indev_data_t *data) {
  data->state = LV_INDEV_STATE_REL;
  if (!touch || touch->max_points == 0) return;
  uint8_t activePoints = 0;
  touch->Read_Data(STATUS, &activePoints, 1);
  if ((activePoints & 0x0F) == 0) {
    pointerDown = false;
    for (uint8_t i = 0; i < touch->max_points; ++i) {
      touch->touch.x[i] = 0;
      touch->touch.y[i] = 0;
    }
    return;
  }
  touch->Get_Touch();
  const uint16_t x = touch->touch.x[0];
  const uint16_t y = touch->touch.y[0];
  if (x >= SCREEN_W || y >= SCREEN_H) { pointerDown = false; return; }
  pointerX = (int16_t)x;
  pointerY = (int16_t)y;
  pointerDown = true;
  data->point.x = x;
  data->point.y = y;
  data->state = LV_INDEV_STATE_PR;
}

static lv_obj_t *makeEntity(uint8_t index, int x, int y, int w, int h, uint32_t fill, int radius = 2) {
  if (index >= MAX_RENDER_ENTITIES || !gameStage) return nullptr;
  if (!entities[index]) {
    entities[index] = lv_obj_create(gameStage);
    lv_obj_set_style_border_width(entities[index], 0, 0);
    lv_obj_set_style_pad_all(entities[index], 0, 0);
    lv_obj_set_style_shadow_width(entities[index], 0, 0);
  }
  lv_obj_clear_flag(entities[index], LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_pos(entities[index], x, y);
  lv_obj_set_size(entities[index], w, h);
  lv_obj_set_style_bg_color(entities[index], color(fill), 0);
  lv_obj_set_style_bg_opa(entities[index], LV_OPA_COVER, 0);
  lv_obj_set_style_radius(entities[index], radius, 0);
  return entities[index];
}

static void hideEntity(uint8_t index) {
  if (index < MAX_RENDER_ENTITIES && entities[index]) lv_obj_add_flag(entities[index], LV_OBJ_FLAG_HIDDEN);
}

static void makeStarfield() {
  for (uint8_t i = 0; i < 15; ++i) {
    const int x = random(8, 440);
    const int y = random(8, 172);
    makeEntity(48 + i, x, y, (i % 4 == 0) ? 2 : 1, (i % 4 == 0) ? 2 : 1,
               (i % 3 == 0) ? 0x25334B : 0x172438, 1);
  }
}

static void setScore(uint32_t value) {
  score = value;
  saveHighScore();
  if (gameScoreLabel) {
    char label[72];
    snprintf(label, sizeof(label), "SCORE %lu   BEST %lu",
             (unsigned long)score, (unsigned long)bestScore);
    lv_label_set_text(gameScoreLabel, label);
  }
}

static void showGameHint(const char *message) {
  if (!gameHint) return;
  lv_label_set_text(gameHint, message);
  if (message[0]) lv_obj_clear_flag(gameHint, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(gameHint, LV_OBJ_FLAG_HIDDEN);
}

static void updateSnakeObjects() {
  const uint32_t accent = activeGame.accent;
  for (int i = 0; i < 32; ++i) {
    if (i < snakeLength) {
      makeEntity(i, snakeX[i] * 14, snakeY[i] * 14 + 5, 12, 12,
                 i == 0 ? C_WHITE : accent, 3);
    } else hideEntity(i);
  }
  makeEntity(32, foodX * 14, foodY * 14 + 5, 10, 10, C_MAGENTA, 5);
}

static void randomFood() {
  bool occupied;
  do {
    foodX = random(0, 32);
    foodY = random(0, 12);
    occupied = false;
    for (int i = 0; i < snakeLength; ++i) {
      if (snakeX[i] == foodX && snakeY[i] == foodY) { occupied = true; break; }
    }
  } while (occupied);
}

static void updateGameScoreText() {
  if (!gameScoreLabel) return;
  char label[72];
  snprintf(label, sizeof(label), "SCORE %lu   BEST %lu",
           (unsigned long)score, (unsigned long)bestScore);
  lv_label_set_text(gameScoreLabel, label);
}

static void resetGame() {
  paused = false;
  gameOver = false;
  shotActive = false;
  actionA = actionB = false;
  lives = 3;
  memset(controls, 0, sizeof(controls));
  bestScore = highScoreFor(activeGame);
  setScore(0);
  showGameHint("");
  if (pauseButtonLabel) lv_label_set_text(pauseButtonLabel, "II");

  switch (activeGame.type) {
    case GAME_SNAKE:
      snakeLength = 4;
      snakeDirection = nextDirection = 3;
      for (int i = 0; i < snakeLength; ++i) {
        snakeX[i] = 12 - i;
        snakeY[i] = 5;
      }
      randomFood();
      updateSnakeObjects();
      break;

    case GAME_PONG:
      paddleY = opponentY = 72;
      ballX = 219;
      ballY = 87;
      ballVX = (random(0, 2) == 0) ? -4 : 4;
      ballVY = (random(0, 2) == 0) ? -3 : 3;
      makeEntity(0, 14, (int)paddleY, 7, 42, activeGame.accent, 2);
      makeEntity(1, 427, 68, 7, 42, C_MAGENTA, 2);
      makeEntity(2, (int)ballX, (int)ballY, 8, 8, C_WHITE, 4);
      for (int i = 0; i < 9; ++i) makeEntity(3 + i, 223, 8 + i * 20, 2, 10, 0x344259, 1);
      break;

    case GAME_BREAKOUT:
      paddleX = 190;
      bricksRemaining = 32;
      ballX = 218;
      ballY = 145;
      ballVX = 4;
      ballVY = -4;
      for (int i = 0; i < 32; ++i) {
        const int col = i % 8, row = i / 8;
        const uint32_t brickColor = row == 0 ? C_MAGENTA : (row == 1 ? C_AMBER : (row == 2 ? C_LIME : C_CYAN));
        makeEntity(i, 17 + col * 55, 10 + row * 19, 45, 12, brickColor, 3);
      }
      makeEntity(32, (int)paddleX, 160, 66, 8, activeGame.accent, 4);
      makeEntity(33, (int)ballX, (int)ballY, 8, 8, C_WHITE, 4);
      break;

    case GAME_INVADERS:
      alienDirection = 1;
      alienMoveCounter = 0;
      shipX = 210;
      bulletY = -20;
      for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 7; ++col) {
          const int i = row * 7 + col;
          alienX[i] = 28 + col * 54;
          alienY[i] = 18 + row * 24;
          alienAlive[i] = true;
          makeEntity(i, alienX[i], alienY[i], 20, 13, row == 0 ? C_MAGENTA : activeGame.accent, 3);
        }
      }
      makeEntity(32, shipX, 155, 24, 10, C_LIME, 3);
      hideEntity(33);
      break;

    case GAME_SCRIPT:
      resetScriptRuntime();
      runScriptEvent(SCRIPT_START);
      break;

    default:
      break;
  }
  updateGameScoreText();
}

static void finishRun(const char *message) {
  gameOver = true;
  shotActive = false;
  showGameHint(message);
  saveHighScore();
  updateGameScoreText();
}

static void updateSnake() {
  if (controls[0] && snakeDirection != 3) nextDirection = 0;
  else if (controls[1] && snakeDirection != 2) nextDirection = 1;
  else if (controls[2] && snakeDirection != 1) nextDirection = 2;
  else if (controls[3] && snakeDirection != 0) nextDirection = 3;
  snakeDirection = nextDirection;

  for (int i = snakeLength - 1; i > 0; --i) {
    snakeX[i] = snakeX[i - 1];
    snakeY[i] = snakeY[i - 1];
  }
  if (snakeDirection == 0) --snakeX[0];
  else if (snakeDirection == 1) --snakeY[0];
  else if (snakeDirection == 2) ++snakeY[0];
  else ++snakeX[0];

  bool hit = snakeX[0] < 0 || snakeX[0] >= 32 || snakeY[0] < 0 || snakeY[0] >= 12;
  for (int i = 1; i < snakeLength; ++i) {
    if (snakeX[0] == snakeX[i] && snakeY[0] == snakeY[i]) hit = true;
  }
  if (hit) {
    finishRun("SIGNAL LOST  //  TAP A TO RESTART");
    return;
  }
  if (snakeX[0] == foodX && snakeY[0] == foodY) {
    score += 10;
    if (snakeLength < 32) ++snakeLength;
    setScore(score);
    randomFood();
  }
  updateSnakeObjects();
}

static void updatePong() {
  if (controls[1]) paddleY -= 7;
  if (controls[2]) paddleY += 7;
  paddleY = constrain(paddleY, 2, 136);
  if (ballY < opponentY + 20) opponentY -= 0.5f;
  else if (ballY > opponentY + 20) opponentY += 0.5f;
  opponentY = constrain(opponentY, 2, 136);

  ballX += ballVX;
  ballY += ballVY;
  if (ballY <= 2 || ballY >= 170) ballVY = -ballVY;
  if (ballVX < 0 && ballX < 25 && ballY + 8 >= paddleY && ballY <= paddleY + 42) {
    ballVX = fabs(ballVX) + 0.2f;
    ballVY += (ballY - (paddleY + 21)) * 0.08f;
    score += 1;
    setScore(score);
  }
  if (ballVX > 0 && ballX > 414 && ballY + 8 >= opponentY && ballY <= opponentY + 42) ballVX = -fabs(ballVX);
  if (ballX < -8 || ballX > 448) {
    const bool escapedLeft = ballX < 0;
    score += 5;
    setScore(score);
    ballX = 220;
    ballY = random(30, 140);
    ballVX = escapedLeft ? 4 : -4;
    ballVY = (random(0, 2) == 0) ? -3 : 3;
  }
  makeEntity(0, 14, (int)paddleY, 7, 42, activeGame.accent, 2);
  makeEntity(1, 427, (int)opponentY, 7, 42, C_MAGENTA, 2);
  makeEntity(2, (int)ballX, (int)ballY, 8, 8, C_WHITE, 4);
}

static void resetBricks() {
  for (int i = 0; i < 32; ++i) {
    const int col = i % 8, row = i / 8;
    const uint32_t brickColor = row == 0 ? C_MAGENTA : (row == 1 ? C_AMBER : (row == 2 ? C_LIME : C_CYAN));
    makeEntity(i, 17 + col * 55, 10 + row * 19, 45, 12, brickColor, 3);
  }
}

static void updateBreakout() {
  if (controls[0]) paddleX -= 9;
  if (controls[3]) paddleX += 9;
  paddleX = constrain((int)paddleX, 0, 382);
  ballX += ballVX;
  ballY += ballVY;
  if (ballX <= 0 || ballX >= 440) ballVX = -ballVX;
  if (ballY <= 0) ballVY = fabs(ballVY);
  if (ballY + 8 >= 160 && ballX + 8 >= paddleX && ballX <= paddleX + 66) {
    ballVY = -fabs(ballVY);
    ballVX += ((ballX + 4) - (paddleX + 33)) * 0.04f;
  }
  for (int i = 0; i < 32; ++i) {
    if (!entities[i] || (lv_obj_has_flag(entities[i], LV_OBJ_FLAG_HIDDEN))) continue;
    const int col = i % 8, row = i / 8;
    const int bx = 17 + col * 55, by = 10 + row * 19;
    if (ballX + 8 >= bx && ballX <= bx + 45 && ballY + 8 >= by && ballY <= by + 12) {
      hideEntity(i);
      --bricksRemaining;
      ballVY = -ballVY;
      score += (4 - row) * 5;
      setScore(score);
      if (bricksRemaining <= 0) {
        score += 100;
        setScore(score);
        finishRun("SECTOR CLEAR  //  TAP A TO RESTART");
      }
      break;
    }
  }
  if (ballY > 184) {
    if (lives > 1) {
      --lives;
      ballX = paddleX + 29;
      ballY = 145;
      ballVY = -4;
    } else {
      finishRun("CABINET OVER  //  TAP A TO RESTART");
    }
  }
  makeEntity(32, (int)paddleX, 160, 66, 8, activeGame.accent, 4);
  makeEntity(33, (int)ballX, (int)ballY, 8, 8, C_WHITE, 4);
}

static void shootBullet() {
  if (activeGame.type != GAME_INVADERS || gameOver || shotActive) return;
  shotActive = true;
  bulletX = shipX + 10;
  bulletY = 148;
  makeEntity(33, bulletX, bulletY, 3, 11, C_AMBER, 1);
}

static void updateInvaders() {
  if (controls[0]) shipX -= 7;
  if (controls[3]) shipX += 7;
  shipX = constrain(shipX, 0, 424);
  ++alienMoveCounter;
  if (alienMoveCounter >= max(1, 7 - activeGame.speed / 4)) {
    alienMoveCounter = 0;
    bool edge = false;
    for (int i = 0; i < 28; ++i) if (alienAlive[i] &&
      ((alienDirection > 0 && alienX[i] > 424) || (alienDirection < 0 && alienX[i] < 4))) edge = true;
    if (edge) alienDirection = -alienDirection;
    bool any = false;
    for (int i = 0; i < 28; ++i) {
      if (!alienAlive[i]) continue;
      any = true;
      alienX[i] += alienDirection * 4;
      if (edge) alienY[i] += 8;
      makeEntity(i, alienX[i], alienY[i], 20, 13, (i / 7 == 0) ? C_MAGENTA : activeGame.accent, 3);
      if (alienY[i] >= 145) finishRun("INVASION  //  TAP A TO RESTART");
    }
    if (!any) {
      score += 100;
      setScore(score);
      finishRun("SECTOR CLEAR  //  TAP A TO RESTART");
    }
  }
  if (shotActive) {
    bulletY -= 10;
    if (bulletY < 0) {
      shotActive = false;
      hideEntity(33);
    } else {
      makeEntity(33, bulletX, bulletY, 3, 11, C_AMBER, 1);
      for (int i = 0; i < 28; ++i) {
        if (alienAlive[i] && bulletX >= alienX[i] && bulletX <= alienX[i] + 20 &&
            bulletY <= alienY[i] + 13 && bulletY + 11 >= alienY[i]) {
          alienAlive[i] = false;
          hideEntity(i);
          shotActive = false;
          hideEntity(33);
          score += 10;
          setScore(score);
          break;
        }
      }
    }
  }
  makeEntity(32, shipX, 155, 24, 10, C_LIME, 3);
}

static void gameStep() {
  if (screenMode != SCREEN_PLAY || paused || gameOver) return;
  switch (activeGame.type) {
    case GAME_SNAKE: updateSnake(); break;
    case GAME_PONG: updatePong(); break;
    case GAME_BREAKOUT: updateBreakout(); break;
    case GAME_INVADERS: updateInvaders(); break;
    case GAME_SCRIPT:
      updateScriptTweens();
      runScriptEvent(SCRIPT_UPDATE);
      break;
    default: break;
  }
}

static void onGameControl(lv_event_t *event) {
  const int code = (int)(intptr_t)lv_event_get_user_data(event);
  const lv_event_code_t eventCode = lv_event_get_code(event);
  if (code >= 0 && code < 4) {
    if (eventCode == LV_EVENT_PRESSED) {
      controls[code] = true;
      if (activeGame.type == GAME_SCRIPT) {
        const ScriptEvent downEvents[4] = {SCRIPT_LEFT_DOWN, SCRIPT_UP_DOWN, SCRIPT_DOWN_DOWN, SCRIPT_RIGHT_DOWN};
        runScriptEvent(downEvents[code]);
      }
      if (activeGame.type == GAME_SNAKE) {
        const int wanted = code;
        if (!((snakeDirection == 0 && wanted == 3) || (snakeDirection == 3 && wanted == 0) ||
              (snakeDirection == 1 && wanted == 2) || (snakeDirection == 2 && wanted == 1))) {
          nextDirection = wanted;
        }
      }
    } else if (eventCode == LV_EVENT_RELEASED || eventCode == LV_EVENT_PRESS_LOST) {
      controls[code] = false;
      if (activeGame.type == GAME_SCRIPT) {
        const ScriptEvent upEvents[4] = {SCRIPT_LEFT_UP, SCRIPT_UP_UP, SCRIPT_DOWN_UP, SCRIPT_RIGHT_UP};
        runScriptEvent(upEvents[code]);
      }
    }
  } else if ((code == 4 || code == 5) && (eventCode == LV_EVENT_PRESSED ||
             eventCode == LV_EVENT_RELEASED || eventCode == LV_EVENT_PRESS_LOST)) {
    const bool pressed = eventCode == LV_EVENT_PRESSED;
    if (code == 4) actionA = pressed;
    else actionB = pressed;
  } else if ((code == 4 || code == 5) && eventCode == LV_EVENT_CLICKED) {
    if (gameOver) resetGame();
    else if (code == 4 && activeGame.type == GAME_INVADERS) shootBullet();
    else if (code == 4 && activeGame.type == GAME_SNAKE) resetGame();
    else if (code == 4 && activeGame.type == GAME_BREAKOUT) {
      ballVX = (random(0, 2) == 0) ? -4 : 4;
      ballVY = -4;
    } else if (activeGame.type == GAME_SCRIPT) {
      if (code == 4) actionA = true;
      else actionB = true;
      runScriptEvent(code == 4 ? SCRIPT_A : SCRIPT_B);
      actionA = actionB = false;
    }
  }
}

static void startGame(const GameCard *game) {
  if (!game || game->type == GAME_NONE) return;
  libraryNotice[0] = '\0';
  if (game->type == GAME_SCRIPT) {
    if (!sdReady) {
      copyLiteral(libraryNotice, sizeof(libraryNotice), "INSERT THE SD CARD TO OPEN THIS SCRIPT CART");
      showGames();
      return;
    }
    const String path = String(GAMES_DIR) + "/" + String(game->id) + ".cart";
    File file = SD_MMC.open(path, FILE_READ);
    if (!file || file.isDirectory() || file.size() > MAX_CART_BYTES) {
      if (file) file.close();
      copyLiteral(libraryNotice, sizeof(libraryNotice), "SCRIPT CART IS MISSING OR TOO LARGE");
      showGames();
      return;
    }
    String payload;
    payload.reserve(file.size());
    while (file.available()) payload += (char)file.read();
    file.close();
    GameCard loaded = {};
    if (!parseCart(payload, loaded) || loaded.type != GAME_SCRIPT || !sameId(loaded.id, game->id)) {
      if (scriptCompileErrorLine) snprintf(libraryNotice, sizeof(libraryNotice), "SCRIPT ERROR ON LINE %u", (unsigned)scriptCompileErrorLine);
      else copyLiteral(libraryNotice, sizeof(libraryNotice), "SCRIPT CART FAILED MANIFEST VALIDATION");
      showGames();
      return;
    }
    activeGame = loaded;
  } else {
    activeGame = *game;
    scriptCodeCount = 0;
  }
  screenMode = SCREEN_PLAY;
  clearUiRefs();
  stopGameAudio();
  lv_obj_clean(lv_scr_act());
  freeBitmapAssets();
  screenBackground();
  memset(entities, 0, sizeof(entities));
  memset(scriptLabels, 0, sizeof(scriptLabels));
  memset(scriptEntities, 0, sizeof(scriptEntities));
  actionA = actionB = false;
  topBar(activeGame.title, true, false);

  gameScoreLabel = makeLabel(lv_scr_act(), "", 300, 10, 120, 22, C_WHITE);
  lv_obj_set_style_text_align(gameScoreLabel, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_t *pauseButton = makeButton(lv_scr_act(), "II", 426, 6, 42, 30, C_PANEL_HI, onPause);
  pauseButtonLabel = lv_obj_get_child(pauseButton, 0);

  gameStage = lv_obj_create(lv_scr_act());
  lv_obj_set_pos(gameStage, 16, 48);
  lv_obj_set_size(gameStage, 448, 184);
  lv_obj_set_style_bg_color(gameStage, color(0x080D18), 0);
  lv_obj_set_style_bg_opa(gameStage, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(gameStage, 1, 0);
  lv_obj_set_style_border_color(gameStage, color(0x27364D), 0);
  lv_obj_set_style_radius(gameStage, 9, 0);
  lv_obj_set_style_pad_all(gameStage, 0, 0);
  lv_obj_set_scrollbar_mode(gameStage, LV_SCROLLBAR_MODE_OFF);
  loadGameBitmapAssets();
  gameHint = makeLabel(gameStage, "", 40, 82, 368, 24, C_AMBER, &lv_font_montserrat_20);
  lv_obj_set_style_text_align(gameHint, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_add_flag(gameHint, LV_OBJ_FLAG_HIDDEN);
  if (activeGame.type != GAME_SCRIPT) makeStarfield();

  const char *labels[6] = {"<", "^", "v", ">", "A", "B"};
  const int xs[6] = {34, 94, 154, 214, 324, 390};
  const uint32_t fills[6] = {C_PANEL_HI, C_PANEL_HI, C_PANEL_HI, C_PANEL_HI, C_MAGENTA, C_CYAN};
  for (int i = 0; i < 6; ++i) {
    makeButton(lv_scr_act(), labels[i], xs[i], 248, 52, 54, fills[i], onGameControl,
               (void *)(intptr_t)i, &lv_font_montserrat_20);
  }
  resetGame();
  lastGameTick = millis();
}

// PixelDeck Script API 2/3 is an event-driven, bounded instruction VM. Cartridge
// files contain data and commands only; no downloaded C/C++ or machine code runs.
static char *scriptTrim(char *text) {
  while (*text && (uint8_t)*text <= ' ') ++text;
  size_t length = strlen(text);
  while (length && (uint8_t)text[length - 1] <= ' ') text[--length] = '\0';
  return text;
}

static uint8_t scriptFields(char *line, char **fields, uint8_t capacity) {
  if (capacity == 0) return 0;
  uint8_t count = 1;
  fields[0] = scriptTrim(line);
  for (char *at = fields[0]; *at; ++at) {
    if (*at != '|') continue;
    if (count == capacity) break;
    *at = '\0';
    fields[count++] = scriptTrim(at + 1);
  }
  return count;
}

static ScriptEvent scriptEventFromName(const char *name) {
  if (!strcasecmp(name, "start")) return SCRIPT_START;
  if (!strcasecmp(name, "update") || !strcasecmp(name, "tick")) return SCRIPT_UPDATE;
  if (!strcasecmp(name, "left_down")) return SCRIPT_LEFT_DOWN;
  if (!strcasecmp(name, "left_up")) return SCRIPT_LEFT_UP;
  if (!strcasecmp(name, "up_down")) return SCRIPT_UP_DOWN;
  if (!strcasecmp(name, "up_up")) return SCRIPT_UP_UP;
  if (!strcasecmp(name, "down_down")) return SCRIPT_DOWN_DOWN;
  if (!strcasecmp(name, "down_up")) return SCRIPT_DOWN_UP;
  if (!strcasecmp(name, "right_down")) return SCRIPT_RIGHT_DOWN;
  if (!strcasecmp(name, "right_up")) return SCRIPT_RIGHT_UP;
  if (!strcasecmp(name, "a")) return SCRIPT_A;
  if (!strcasecmp(name, "b")) return SCRIPT_B;
  return SCRIPT_NO_EVENT;
}

static ScriptOp scriptOpFromName(const char *name) {
  if (!strcasecmp(name, "if")) return SCRIPT_OP_IF;
  if (!strcasecmp(name, "else")) return SCRIPT_OP_ELSE;
  if (!strcasecmp(name, "endif")) return SCRIPT_OP_ENDIF;
  if (!strcasecmp(name, "repeat")) return SCRIPT_OP_REPEAT;
  if (!strcasecmp(name, "endrepeat")) return SCRIPT_OP_ENDREPEAT;
  if (!strcasecmp(name, "set")) return SCRIPT_OP_SET;
  if (!strcasecmp(name, "add")) return SCRIPT_OP_ADD;
  if (!strcasecmp(name, "sub")) return SCRIPT_OP_SUB;
  if (!strcasecmp(name, "mul")) return SCRIPT_OP_MUL;
  if (!strcasecmp(name, "div")) return SCRIPT_OP_DIV;
  if (!strcasecmp(name, "mod")) return SCRIPT_OP_MOD;
  if (!strcasecmp(name, "random")) return SCRIPT_OP_RANDOM;
  if (!strcasecmp(name, "rect")) return SCRIPT_OP_RECT;
  if (!strcasecmp(name, "move")) return SCRIPT_OP_MOVE;
  if (!strcasecmp(name, "position")) return SCRIPT_OP_POSITION;
  if (!strcasecmp(name, "velocity")) return SCRIPT_OP_VELOCITY;
  if (!strcasecmp(name, "physics")) return SCRIPT_OP_PHYSICS;
  if (!strcasecmp(name, "collide")) return SCRIPT_OP_COLLIDE;
  if (!strcasecmp(name, "getx")) return SCRIPT_OP_GETX;
  if (!strcasecmp(name, "gety")) return SCRIPT_OP_GETY;
  if (!strcasecmp(name, "getvx")) return SCRIPT_OP_GETVX;
  if (!strcasecmp(name, "getvy")) return SCRIPT_OP_GETVY;
  if (!strcasecmp(name, "exists")) return SCRIPT_OP_EXISTS;
  if (!strcasecmp(name, "hide")) return SCRIPT_OP_HIDE;
  if (!strcasecmp(name, "show")) return SCRIPT_OP_SHOW;
  if (!strcasecmp(name, "remove")) return SCRIPT_OP_REMOVE;
  if (!strcasecmp(name, "tween")) return SCRIPT_OP_TWEEN;
  if (!strcasecmp(name, "label")) return SCRIPT_OP_LABEL;
  if (!strcasecmp(name, "text")) return SCRIPT_OP_TEXT;
  if (!strcasecmp(name, "clearlabel")) return SCRIPT_OP_CLEARLABEL;
  if (!strcasecmp(name, "image") || !strcasecmp(name, "sprite")) return SCRIPT_OP_IMAGE;
  if (!strcasecmp(name, "music")) return SCRIPT_OP_MUSIC;
  if (!strcasecmp(name, "sfx") || !strcasecmp(name, "sound")) return SCRIPT_OP_SFX;
  if (!strcasecmp(name, "audio_stop")) return SCRIPT_OP_AUDIO_STOP;
  if (!strcasecmp(name, "audio_volume")) return SCRIPT_OP_AUDIO_VOLUME;
  if (!strcasecmp(name, "circle")) return SCRIPT_OP_CIRCLE;
  if (!strcasecmp(name, "background")) return SCRIPT_OP_BACKGROUND;
  if (!strcasecmp(name, "alpha")) return SCRIPT_OP_ALPHA;
  if (!strcasecmp(name, "over")) return SCRIPT_OP_OVER;
  if (!strcasecmp(name, "win")) return SCRIPT_OP_WIN;
  return SCRIPT_OP_INVALID;
}

static bool scriptCompileFail(uint32_t line, const char *reason) {
  scriptCompileErrorLine = line;
  Serial.printf("[SCRIPT] API 2/3 rejected at line %u: %s\n", (unsigned)line, reason);
  return false;
}

static bool scriptValidArity(ScriptOp op, uint8_t count) {
  switch (op) {
    case SCRIPT_OP_IF: return count == 2 || count == 4;
    case SCRIPT_OP_ELSE: case SCRIPT_OP_ENDIF: case SCRIPT_OP_ENDREPEAT: return count == 1;
    case SCRIPT_OP_REPEAT: return count == 2;
    case SCRIPT_OP_PHYSICS: return count == 1 || count == 2;
    case SCRIPT_OP_SET: case SCRIPT_OP_ADD: case SCRIPT_OP_SUB: case SCRIPT_OP_MUL:
    case SCRIPT_OP_DIV: case SCRIPT_OP_MOD: return count == 3;
    case SCRIPT_OP_RANDOM: return count == 4;
    case SCRIPT_OP_RECT: return count == 8;
    case SCRIPT_OP_MOVE: case SCRIPT_OP_POSITION: case SCRIPT_OP_COLLIDE: return count == 4;
    case SCRIPT_OP_GETX: case SCRIPT_OP_GETY: case SCRIPT_OP_GETVX: case SCRIPT_OP_GETVY:
    case SCRIPT_OP_EXISTS: return count == 3;
    case SCRIPT_OP_TWEEN: return count == 5;
    case SCRIPT_OP_VELOCITY: return count == 4 || count == 5;
    case SCRIPT_OP_IMAGE: return count == 6;
    case SCRIPT_OP_MUSIC: return count == 2 || count == 3;
    case SCRIPT_OP_SFX: return count == 3;
    case SCRIPT_OP_AUDIO_STOP: return count == 1;
    case SCRIPT_OP_AUDIO_VOLUME: return count == 2;
    case SCRIPT_OP_CIRCLE: return count == 6;
    case SCRIPT_OP_BACKGROUND: return count == 2;
    case SCRIPT_OP_ALPHA: return count == 3;
    case SCRIPT_OP_HIDE: case SCRIPT_OP_SHOW: case SCRIPT_OP_REMOVE:
    case SCRIPT_OP_CLEARLABEL: return count == 2;
    case SCRIPT_OP_LABEL: return count >= 7;
    case SCRIPT_OP_TEXT: case SCRIPT_OP_OVER: case SCRIPT_OP_WIN: return count >= 3 ||
      ((op == SCRIPT_OP_OVER || op == SCRIPT_OP_WIN) && count == 2);
    default: return false;
  }
}

static bool compileScript(const char *source, size_t length) {
  scriptCompileErrorLine = 0;
  if (!source || length == 0 || length > MAX_CART_BYTES) return scriptCompileFail(0, "invalid or oversized source");
  if (length + 1 > scriptProgramCapacity) {
    char *expanded = (char *)heap_caps_malloc(length + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!expanded) expanded = (char *)malloc(length + 1);
    if (!expanded) return scriptCompileFail(0, "not enough PSRAM for program source");
    free(scriptProgram);
    scriptProgram = expanded;
    scriptProgramCapacity = length + 1;
  }
  memcpy(scriptProgram, source, length);
  scriptProgram[length] = '\0';
  scriptCodeCount = 0;
  memset(scriptCode, 0, sizeof(scriptCode));

  struct BlockFrame { uint16_t instruction; int16_t elseInstruction; uint8_t event; ScriptOp type; };
  BlockFrame stack[MAX_SCRIPT_NESTING];
  uint8_t depth = 0;
  uint8_t event = SCRIPT_NO_EVENT;
  bool sawStart = false, sawUpdate = false;
  char *cursor = scriptProgram;
  const char *limit = scriptProgram + length;
  uint32_t sourceLine = 0;
  while (cursor < limit) {
    ++sourceLine;
    char *lineStart = cursor;
    char *newline = strchr(cursor, '\n');
    if (newline) { *newline = '\0'; cursor = newline + 1; }
    else cursor = scriptProgram + length;
    char *line = scriptTrim(lineStart);
    if (!*line || *line == '#') continue;
    if (strlen(line) > 384) return scriptCompileFail(sourceLine, "line exceeds 384 characters");
    char parseLine[388];
    strncpy(parseLine, line, sizeof(parseLine) - 1);
    parseLine[sizeof(parseLine) - 1] = '\0';
    char *fields[16] = {};
    const uint8_t count = scriptFields(parseLine, fields, 16);
    if (!strcasecmp(fields[0], "on")) {
      if (event != SCRIPT_NO_EVENT || depth != 0 || count != 2) return scriptCompileFail(sourceLine, "malformed event header");
      event = scriptEventFromName(fields[1]);
      if (event == SCRIPT_NO_EVENT) return scriptCompileFail(sourceLine, "unknown event name");
      if (event == SCRIPT_START) sawStart = true;
      if (event == SCRIPT_UPDATE) sawUpdate = true;
      continue;
    }
    if (!strcasecmp(fields[0], "end")) {
      if (event == SCRIPT_NO_EVENT || depth != 0 || count != 1) return scriptCompileFail(sourceLine, "unexpected or incomplete event end");
      event = SCRIPT_NO_EVENT;
      continue;
    }
    if (event == SCRIPT_NO_EVENT) return scriptCompileFail(sourceLine, "command outside an event");
    if (scriptCodeCount >= MAX_SCRIPT_INSTRUCTIONS) return scriptCompileFail(sourceLine, "more than 4096 commands");
    const ScriptOp op = scriptOpFromName(fields[0]);
    if (op == SCRIPT_OP_INVALID || !scriptValidArity(op, count)) return scriptCompileFail(sourceLine, "unknown command or wrong number of fields");
    const uint16_t index = scriptCodeCount++;
    scriptCode[index].sourceOffset = (uint32_t)(line - scriptProgram);
    scriptCode[index].event = event;
    scriptCode[index].op = op;
    if (op == SCRIPT_OP_IF) {
      if (depth >= MAX_SCRIPT_NESTING) return scriptCompileFail(sourceLine, "control nesting exceeds 16 levels");
      stack[depth++] = {index, -1, event, SCRIPT_OP_IF};
    } else if (op == SCRIPT_OP_ELSE) {
      if (depth == 0 || stack[depth - 1].event != event || stack[depth - 1].type != SCRIPT_OP_IF || stack[depth - 1].elseInstruction >= 0) return scriptCompileFail(sourceLine, "else without matching if");
      stack[depth - 1].elseInstruction = (int16_t)index;
      scriptCode[stack[depth - 1].instruction].jump = index + 1;
    } else if (op == SCRIPT_OP_ENDIF) {
      if (depth == 0 || stack[depth - 1].event != event || stack[depth - 1].type != SCRIPT_OP_IF) return scriptCompileFail(sourceLine, "endif without matching if");
      const BlockFrame frame = stack[--depth];
      if (frame.elseInstruction >= 0) scriptCode[frame.elseInstruction].jump = index + 1;
      else scriptCode[frame.instruction].jump = index + 1;
    } else if (op == SCRIPT_OP_REPEAT) {
      if (depth >= MAX_SCRIPT_NESTING) return scriptCompileFail(sourceLine, "control nesting exceeds 16 levels");
      stack[depth++] = {index, -1, event, SCRIPT_OP_REPEAT};
    } else if (op == SCRIPT_OP_ENDREPEAT) {
      if (depth == 0 || stack[depth - 1].event != event || stack[depth - 1].type != SCRIPT_OP_REPEAT) return scriptCompileFail(sourceLine, "endrepeat without matching repeat");
      const BlockFrame frame = stack[--depth];
      scriptCode[frame.instruction].jump = index + 1;
      scriptCode[index].jump = frame.instruction;
    }
  }
  if (event != SCRIPT_NO_EVENT || depth != 0) return scriptCompileFail(sourceLine, "unclosed event or if block");
  if (!sawStart || !sawUpdate || scriptCodeCount == 0) return scriptCompileFail(sourceLine, "start and update events are required");
  return true;
}

static bool scriptReservedName(const char *name) {
  static const char *reserved[] = {
    "score", "best", "lives", "level", "time", "touch", "touch_x", "touch_y",
    "screen_x", "screen_y", "left", "input_left", "up", "input_up", "down",
    "input_down", "right", "input_right", "button_a", "button_b"
  };
  for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); ++i) {
    if (!strcasecmp(name, reserved[i])) return true;
  }
  return false;
}

static int scriptVariableIndex(const char *name, bool create) {
  if (!name || !*name) return -1;
  while (*name == '$') ++name;
  if (!*name || strlen(name) >= sizeof(scriptVariables[0].name)) return -1;
  if (create && scriptReservedName(name)) return -1;
  for (uint8_t i = 0; i < MAX_SCRIPT_VARIABLES; ++i) {
    if (scriptVariables[i].used && !strcasecmp(scriptVariables[i].name, name)) return i;
  }
  if (!create) return -1;
  for (uint8_t i = 0; i < MAX_SCRIPT_VARIABLES; ++i) {
    if (!scriptVariables[i].used) {
      strncpy(scriptVariables[i].name, name, sizeof(scriptVariables[i].name) - 1);
      scriptVariables[i].name[sizeof(scriptVariables[i].name) - 1] = '\0';
      scriptVariables[i].used = true;
      return i;
    }
  }
  return -1;
}

static int32_t scriptClamp(int64_t value) {
  if (value > 1000000000LL) return 1000000000;
  if (value < -1000000000LL) return -1000000000;
  return (int32_t)value;
}

static int32_t scriptValue(const char *token) {
  if (!token || !*token) return 0;
  while (*token == '$') ++token;
  if (!strcasecmp(token, "score")) return (int32_t)score;
  if (!strcasecmp(token, "best")) return (int32_t)bestScore;
  if (!strcasecmp(token, "lives")) return lives;
  if (!strcasecmp(token, "level")) return scriptLevel;
  if (!strcasecmp(token, "time")) return (int32_t)((millis() - scriptStartedAt) / 1000);
  if (!strcasecmp(token, "touch")) return pointerDown && pointerX >= 16 && pointerX < 464 &&
    pointerY >= 48 && pointerY < 232 ? 1 : 0;
  if (!strcasecmp(token, "touch_x")) return pointerX - 16;
  if (!strcasecmp(token, "touch_y")) return pointerY - 48;
  if (!strcasecmp(token, "screen_x")) return pointerX;
  if (!strcasecmp(token, "screen_y")) return pointerY;
  if (!strcasecmp(token, "left") || !strcasecmp(token, "input_left")) return controls[0] ? 1 : 0;
  if (!strcasecmp(token, "up") || !strcasecmp(token, "input_up")) return controls[1] ? 1 : 0;
  if (!strcasecmp(token, "down") || !strcasecmp(token, "input_down")) return controls[2] ? 1 : 0;
  if (!strcasecmp(token, "right") || !strcasecmp(token, "input_right")) return controls[3] ? 1 : 0;
  if (!strcasecmp(token, "button_a")) return actionA ? 1 : 0;
  if (!strcasecmp(token, "button_b")) return actionB ? 1 : 0;
  char *end = nullptr;
  const long long numeric = strtoll(token, &end, 10);
  if (end && *end == '\0') return scriptClamp(numeric);
  const int index = scriptVariableIndex(token, false);
  return index >= 0 ? scriptVariables[index].value : 0;
}

static void scriptSetValue(const char *name, int32_t value) {
  while (name && *name == '$') ++name;
  if (!name || !*name) return;
  if (!strcasecmp(name, "score")) {
    score = value > 0 ? (uint32_t)value : 0;
    updateGameScoreText();
    return;
  }
  if (!strcasecmp(name, "lives")) { lives = (uint8_t)constrain(value, 0, 99); return; }
  if (!strcasecmp(name, "level")) { scriptLevel = (uint8_t)constrain(value, 1, 99); return; }
  const int index = scriptVariableIndex(name, true);
  if (index >= 0) scriptVariables[index].value = scriptClamp(value);
}

static bool scriptCompare(int32_t left, const char *op, int32_t right) {
  if (!strcmp(op, "=") || !strcmp(op, "==") || !strcasecmp(op, "eq")) return left == right;
  if (!strcmp(op, "!=") || !strcasecmp(op, "ne")) return left != right;
  if (!strcmp(op, ">") || !strcasecmp(op, "gt")) return left > right;
  if (!strcmp(op, "<") || !strcasecmp(op, "lt")) return left < right;
  if (!strcmp(op, ">=") || !strcasecmp(op, "ge")) return left >= right;
  if (!strcmp(op, "<=") || !strcasecmp(op, "le")) return left <= right;
  return false;
}

static uint32_t scriptColor(const char *text) {
  if (!text) return C_WHITE;
  while (*text == '#') ++text;
  return strtoul(text, nullptr, 16) & 0xFFFFFF;
}

static void drawScriptEntity(uint8_t id) {
  if (id >= MAX_SCRIPT_ENTITIES) return;
  ScriptEntity &entity = scriptEntities[id];
  if (!entity.active) return;
  if (!entity.visible) { hideEntity(id); return; }
  if (entity.isImage && entity.bitmapIndex < bitmapAssetCount && bitmapAssets[entity.bitmapIndex].loaded) {
    BitmapAsset &bitmap = bitmapAssets[entity.bitmapIndex];
    if (!entity.object || !entity.imageObject) {
      if (entity.object) lv_obj_del(entity.object);
      entity.object = lv_img_create(gameStage);
      entity.imageObject = true;
      entities[id] = entity.object;
      lv_obj_set_style_bg_opa(entity.object, LV_OPA_TRANSP, 0);
      lv_img_set_pivot(entity.object, 0, 0);
      lv_img_set_antialias(entity.object, true);
    }
    lv_img_set_src(entity.object, &bitmap.image);
    lv_img_set_zoom(entity.object, entity.zoom);
    lv_obj_set_pos(entity.object, (int)entity.x, (int)entity.y);
    lv_obj_set_style_opa(entity.object, entity.opacity, 0);
    lv_obj_clear_flag(entity.object, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  if (entity.object && entity.imageObject) {
    lv_obj_del(entity.object);
    entity.object = nullptr;
    entity.imageObject = false;
    entities[id] = nullptr;
  }
  entity.object = makeEntity(id, (int)entity.x, (int)entity.y, entity.w, entity.h, entity.tint, entity.radius);
  if (entity.object) lv_obj_set_style_opa(entity.object, entity.opacity, 0);
}

static bool scriptCollides(uint8_t first, uint8_t second) {
  if (first >= MAX_SCRIPT_ENTITIES || second >= MAX_SCRIPT_ENTITIES || first == second) return false;
  const ScriptEntity &a = scriptEntities[first], &b = scriptEntities[second];
  if (!a.active || !a.visible || !b.active || !b.visible) return false;
  return a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y;
}

static void scriptMovePhysics(int id) {
  const int first = id < 0 ? 0 : id;
  const int last = id < 0 ? MAX_SCRIPT_ENTITIES : id + 1;
  for (int i = first; i < last; ++i) {
    ScriptEntity &entity = scriptEntities[i];
    if (!entity.active || (entity.vx == 0 && entity.vy == 0)) continue;
    entity.x += entity.vx;
    entity.y += entity.vy;
    if (entity.wrap) {
      if (entity.x > 448) entity.x = -entity.w;
      if (entity.x + entity.w < 0) entity.x = 448;
      if (entity.y > 184) entity.y = -entity.h;
      if (entity.y + entity.h < 0) entity.y = 184;
    } else {
      if (entity.x < 0) { entity.x = 0; entity.vx = fabs(entity.vx); }
      if (entity.x + entity.w > 448) { entity.x = 448 - entity.w; entity.vx = -fabs(entity.vx); }
      if (entity.y < 0) { entity.y = 0; entity.vy = fabs(entity.vy); }
      if (entity.y + entity.h > 184) { entity.y = 184 - entity.h; entity.vy = -fabs(entity.vy); }
    }
    drawScriptEntity((uint8_t)i);
  }
}

static void updateScriptTweens() {
  const uint32_t now = millis();
  for (uint8_t i = 0; i < MAX_SCRIPT_ENTITIES; ++i) {
    ScriptEntity &entity = scriptEntities[i];
    if (!entity.active || !entity.tweening) continue;
    const uint32_t elapsed = now - entity.tweenStarted;
    const float amount = entity.tweenDuration == 0 || elapsed >= entity.tweenDuration
      ? 1.0f : (float)elapsed / entity.tweenDuration;
    entity.x = entity.fromX + (entity.targetX - entity.fromX) * amount;
    entity.y = entity.fromY + (entity.targetY - entity.fromY) * amount;
    if (amount >= 1.0f) entity.tweening = false;
    drawScriptEntity(i);
  }
}

static void scriptExpandText(const char *input, char *output, size_t capacity) {
  if (!capacity) return;
  size_t out = 0;
  while (input && *input && out + 1 < capacity) {
    if (*input != '$') { output[out++] = *input++; continue; }
    ++input;
    char name[16] = {};
    uint8_t n = 0;
    while ((*input == '_' || (*input >= 'A' && *input <= 'Z') ||
            (*input >= 'a' && *input <= 'z') || (*input >= '0' && *input <= '9')) &&
           n + 1 < sizeof(name)) name[n++] = *input++;
    if (!n) { output[out++] = '$'; continue; }
    char number[16];
    snprintf(number, sizeof(number), "%ld", (long)scriptValue(name));
    for (const char *p = number; *p && out + 1 < capacity; ++p) output[out++] = *p;
  }
  output[out] = '\0';
}

static void scriptSetLabel(uint8_t id, int x, int y, int width, uint32_t tint, const char *text) {
  if (id >= MAX_SCRIPT_LABELS || !gameStage) return;
  char expanded[385];
  scriptExpandText(text, expanded, sizeof(expanded));
  if (!scriptLabels[id]) {
    scriptLabels[id] = lv_label_create(gameStage);
    lv_obj_set_style_text_font(scriptLabels[id], &lv_font_montserrat_14, 0);
  }
  lv_label_set_text(scriptLabels[id], expanded);
  lv_obj_set_pos(scriptLabels[id], constrain(x, -448, 896), constrain(y, -184, 368));
  lv_obj_set_size(scriptLabels[id], constrain(width, 8, 448), 24);
  lv_obj_set_style_text_color(scriptLabels[id], color(tint), 0);
}

static void resetScriptRuntime() {
  memset(scriptVariables, 0, sizeof(scriptVariables));
  scriptLevel = 1;
  scriptStartedAt = millis();
  for (uint8_t i = 0; i < MAX_SCRIPT_ENTITIES; ++i) {
    ScriptEntity &entity = scriptEntities[i];
    entity.x = entity.y = entity.vx = entity.vy = 0;
    entity.w = entity.h = 0;
    entity.active = entity.visible = entity.wrap = entity.tweening = entity.isImage = false;
    entity.radius = 0;
    entity.opacity = LV_OPA_COVER;
    entity.zoom = 256;
    entity.bitmapIndex = 0;
    hideEntity(i);
  }
  for (uint8_t i = 0; i < MAX_SCRIPT_LABELS; ++i) {
    if (scriptLabels[i]) lv_label_set_text(scriptLabels[i], "");
  }
}

static void runScriptEvent(ScriptEvent event) {
  if (scriptCodeCount == 0 || event == SCRIPT_NO_EVENT) return;
  struct LoopFrame { uint16_t begin; uint16_t end; uint8_t remaining; };
  LoopFrame loops[MAX_SCRIPT_NESTING];
  uint8_t loopDepth = 0;
  char line[388];
  char *fields[16];
  for (uint16_t pc = 0, budget = 0; pc < scriptCodeCount && budget < MAX_SCRIPT_EVENT_STEPS; ++pc) {
    const ScriptInstruction &instruction = scriptCode[pc];
    if (instruction.event != event) continue;
    ++budget;
    if (screenMode == SCREEN_PLAY && gameOver && event != SCRIPT_START) break;
    strncpy(line, scriptProgram + instruction.sourceOffset, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    const uint8_t count = scriptFields(line, fields, 16);
    const ScriptOp op = (ScriptOp)instruction.op;
    auto value = [&](uint8_t index) -> int32_t { return index < count ? scriptValue(fields[index]) : 0; };
    switch (op) {
      case SCRIPT_OP_IF: {
        const bool condition = count == 2 ? value(1) != 0 : scriptCompare(value(1), fields[2], value(3));
        if (!condition && instruction.jump > pc) pc = instruction.jump - 1;
        break;
      }
      case SCRIPT_OP_ELSE:
        if (instruction.jump > pc) pc = instruction.jump - 1;
        break;
      case SCRIPT_OP_ENDIF: break;
      case SCRIPT_OP_REPEAT: {
        const uint8_t repetitions = (uint8_t)constrain(value(1), 0, (int)MAX_SCRIPT_REPEAT);
        if (repetitions == 0) {
          if (instruction.jump > pc) pc = instruction.jump - 1;
        } else if (loopDepth < MAX_SCRIPT_NESTING && instruction.jump > 0) {
          loops[loopDepth++] = {pc, (uint16_t)(instruction.jump - 1), repetitions};
        }
        break;
      }
      case SCRIPT_OP_ENDREPEAT:
        if (loopDepth > 0 && loops[loopDepth - 1].end == pc) {
          if (loops[loopDepth - 1].remaining > 1) {
            --loops[loopDepth - 1].remaining;
            pc = loops[loopDepth - 1].begin;
          } else --loopDepth;
        }
        break;
      case SCRIPT_OP_SET: scriptSetValue(fields[1], value(2)); break;
      case SCRIPT_OP_ADD: scriptSetValue(fields[1], scriptClamp((int64_t)scriptValue(fields[1]) + value(2))); break;
      case SCRIPT_OP_SUB: scriptSetValue(fields[1], scriptClamp((int64_t)scriptValue(fields[1]) - value(2))); break;
      case SCRIPT_OP_MUL: scriptSetValue(fields[1], scriptClamp((int64_t)scriptValue(fields[1]) * value(2))); break;
      case SCRIPT_OP_DIV: if (value(2) != 0) scriptSetValue(fields[1], scriptValue(fields[1]) / value(2)); break;
      case SCRIPT_OP_MOD: if (value(2) != 0) scriptSetValue(fields[1], scriptValue(fields[1]) % value(2)); break;
      case SCRIPT_OP_RANDOM: {
        const int32_t low = value(2), high = value(3);
        if (high > low) scriptSetValue(fields[1], low + (int32_t)(esp_random() % (uint32_t)((int64_t)high - low)));
        break;
      }
      case SCRIPT_OP_RECT: {
        const int id = value(1);
        if (id < 0 || id >= MAX_SCRIPT_ENTITIES) break;
        ScriptEntity &entity = scriptEntities[id];
        entity.x = constrain(value(2), -448, 896); entity.y = constrain(value(3), -184, 368);
        entity.w = (int16_t)constrain(value(4), 1, 448);
        entity.h = (int16_t)constrain(value(5), 1, 184);
        entity.tint = scriptColor(fields[6]);
        entity.radius = (uint8_t)constrain(value(7), 0, 24);
        entity.isImage = false;
        entity.active = entity.visible = true;
        drawScriptEntity((uint8_t)id);
        break;
      }
      case SCRIPT_OP_CIRCLE: {
        const int id = value(1);
        if (id < 0 || id >= MAX_SCRIPT_ENTITIES) break;
        ScriptEntity &entity = scriptEntities[id];
        const int diameter = constrain(value(4), 1, 184);
        entity.x = constrain(value(2), -448, 896);
        entity.y = constrain(value(3), -184, 368);
        entity.w = entity.h = diameter;
        entity.tint = scriptColor(fields[5]);
        entity.radius = diameter / 2;
        entity.isImage = false;
        entity.active = entity.visible = true;
        drawScriptEntity((uint8_t)id);
        break;
      }
      case SCRIPT_OP_BACKGROUND:
        if (gameStage) {
          lv_obj_set_style_bg_color(gameStage, color(scriptColor(fields[1])), 0);
          lv_obj_set_style_bg_opa(gameStage, LV_OPA_COVER, 0);
        }
        break;
      case SCRIPT_OP_ALPHA: {
        const int id = value(1);
        if (id >= 0 && id < MAX_SCRIPT_ENTITIES && scriptEntities[id].active) {
          scriptEntities[id].opacity = (uint8_t)constrain(value(2), 0, 255);
          drawScriptEntity((uint8_t)id);
        }
        break;
      }
      case SCRIPT_OP_IMAGE: {
        const int id = value(1);
        if (id < 0 || id >= MAX_SCRIPT_ENTITIES) break;
        int assetIndex = -1;
        for (uint8_t i = 0; i < bitmapAssetCount; ++i) {
          if (bitmapAssets[i].loaded && !strcasecmp(bitmapAssets[i].name, fields[2])) {
            assetIndex = i;
            break;
          }
        }
        if (assetIndex < 0) break;
        ScriptEntity &entity = scriptEntities[id];
        BitmapAsset &bitmap = bitmapAssets[assetIndex];
        const uint16_t percent = (uint16_t)constrain(value(5), 25, 400);
        entity.bitmapIndex = (uint8_t)assetIndex;
        entity.zoom = (uint16_t)(percent * 256U / 100U);
        entity.x = constrain(value(3), -448, 896);
        entity.y = constrain(value(4), -184, 368);
        entity.w = max(1, (int)bitmap.width * percent / 100);
        entity.h = max(1, (int)bitmap.height * percent / 100);
        entity.isImage = true;
        entity.active = entity.visible = true;
        drawScriptEntity((uint8_t)id);
        break;
      }
      case SCRIPT_OP_MOVE: case SCRIPT_OP_POSITION: {
        const int id = value(1);
        if (id < 0 || id >= MAX_SCRIPT_ENTITIES || !scriptEntities[id].active) break;
        ScriptEntity &entity = scriptEntities[id];
        if (op == SCRIPT_OP_MOVE) {
          entity.x = constrain(entity.x + value(2), -448.0f, 896.0f);
          entity.y = constrain(entity.y + value(3), -184.0f, 368.0f);
        } else {
          entity.x = constrain(value(2), -448, 896);
          entity.y = constrain(value(3), -184, 368);
        }
        drawScriptEntity((uint8_t)id);
        break;
      }
      case SCRIPT_OP_VELOCITY: {
        const int id = value(1);
        if (id < 0 || id >= MAX_SCRIPT_ENTITIES || !scriptEntities[id].active) break;
        scriptEntities[id].vx = constrain(value(2), -64, 64);
        scriptEntities[id].vy = constrain(value(3), -64, 64);
        scriptEntities[id].wrap = count == 5 && !strcasecmp(fields[4], "wrap");
        break;
      }
      case SCRIPT_OP_PHYSICS: {
        const int id = count == 1 ? -1 : value(1);
        if (id == -1 || (id >= 0 && id < MAX_SCRIPT_ENTITIES)) scriptMovePhysics(id);
        break;
      }
      case SCRIPT_OP_COLLIDE: {
        const int first = value(2), second = value(3);
        scriptSetValue(fields[1], first >= 0 && first < MAX_SCRIPT_ENTITIES &&
          second >= 0 && second < MAX_SCRIPT_ENTITIES && scriptCollides(first, second) ? 1 : 0);
        break;
      }
      case SCRIPT_OP_GETX: case SCRIPT_OP_GETY: case SCRIPT_OP_GETVX: case SCRIPT_OP_GETVY:
      case SCRIPT_OP_EXISTS: {
        const int id = value(2);
        int32_t result = 0;
        if (id >= 0 && id < MAX_SCRIPT_ENTITIES) {
          const ScriptEntity &entity = scriptEntities[id];
          if (op == SCRIPT_OP_EXISTS) result = entity.active ? 1 : 0;
          else if (op == SCRIPT_OP_GETX) result = (int32_t)entity.x;
          else if (op == SCRIPT_OP_GETY) result = (int32_t)entity.y;
          else if (op == SCRIPT_OP_GETVX) result = (int32_t)entity.vx;
          else result = (int32_t)entity.vy;
        }
        scriptSetValue(fields[1], result);
        break;
      }
      case SCRIPT_OP_HIDE: case SCRIPT_OP_SHOW: case SCRIPT_OP_REMOVE: {
        const int id = value(1);
        if (id < 0 || id >= MAX_SCRIPT_ENTITIES) break;
        ScriptEntity &entity = scriptEntities[id];
        if (op == SCRIPT_OP_REMOVE) { entity.active = false; entity.visible = false; hideEntity(id); }
        else { entity.visible = op == SCRIPT_OP_SHOW; drawScriptEntity((uint8_t)id); }
        break;
      }
      case SCRIPT_OP_TWEEN: {
        const int id = value(1);
        if (id < 0 || id >= MAX_SCRIPT_ENTITIES || !scriptEntities[id].active) break;
        ScriptEntity &entity = scriptEntities[id];
        entity.fromX = entity.x; entity.fromY = entity.y;
        entity.targetX = constrain(value(2), -448, 896); entity.targetY = constrain(value(3), -184, 368);
        entity.tweenStarted = millis(); entity.tweenDuration = (uint32_t)constrain(value(4), 0, 60000);
        entity.tweening = true;
        break;
      }
      case SCRIPT_OP_LABEL: {
        const int id = value(1);
        if (id >= 0 && id < MAX_SCRIPT_LABELS) scriptSetLabel(id, value(2), value(3), value(4), scriptColor(fields[5]), fields[6]);
        break;
      }
      case SCRIPT_OP_TEXT: {
        const int id = value(1);
        if (id >= 0 && id < MAX_SCRIPT_LABELS && scriptLabels[id]) {
          char expanded[385]; scriptExpandText(fields[2], expanded, sizeof(expanded));
          lv_label_set_text(scriptLabels[id], expanded);
        }
        break;
      }
      case SCRIPT_OP_MUSIC:
        playGameAudio(fields[1], count == 2 || !strcasecmp(fields[2], "loop"), consoleVolume);
        break;
      case SCRIPT_OP_SFX:
        playGameAudio(fields[1], false, (uint8_t)constrain(value(2), 0, 21), true);
        break;
      case SCRIPT_OP_AUDIO_STOP:
        stopGameAudio();
        break;
      case SCRIPT_OP_AUDIO_VOLUME:
        consoleVolume = (uint8_t)constrain(value(1), 0, 21);
        backgroundAudioVolume = consoleVolume;
        if (audioReady) audio.setVolume(consoleVolume);
        break;
      case SCRIPT_OP_CLEARLABEL: {
        const int id = value(1);
        if (id >= 0 && id < MAX_SCRIPT_LABELS && scriptLabels[id]) lv_label_set_text(scriptLabels[id], "");
        break;
      }
      case SCRIPT_OP_OVER: case SCRIPT_OP_WIN: {
        finishRun(fields[1]);
        break;
      }
      default: break;
    }
  }
}

static String normalizeStoreRoot() {
  String root = savedStoreRoot;
  root.trim();
  if (root.length() == 0 || root == STORE_ROOT_DEFAULT) return "";
  if (!root.startsWith("https://raw.githubusercontent.com/")) return "";
  if (!root.endsWith("/")) root += "/";
  return root;
}

static bool getGithubText(const String &url, String &body, size_t limit) {
  WiFiClientSecure secureClient;
  // Packs are constrained text data and never executed as firmware or code.
  secureClient.setInsecure();
  HTTPClient http;
  http.setTimeout(9000);
  http.setConnectTimeout(9000);
  http.setUserAgent("PixelDeck-FNK0104N/2.0");
  if (!http.begin(secureClient, url)) return false;
  const int status = http.GET();
  if (status != HTTP_CODE_OK) {
    Serial.printf("[STORE] HTTP status %d for %s\n", status, url.c_str());
    http.end();
    return false;
  }
  const int length = http.getSize();
  if (length > (int)limit) {
    Serial.printf("[STORE] Refusing oversized response (%d bytes)\n", length);
    http.end();
    return false;
  }
  body = "";
  body.reserve(length > 0 ? (size_t)length : min(limit, (size_t)2048));
  Stream *stream = http.getStreamPtr();
  uint8_t chunk[512];
  uint32_t lastProgress = millis();
  bool overflow = false;
  while (stream && (length >= 0 ? body.length() < (size_t)length :
                    (http.connected() || stream->available()))) {
    const int available = stream->available();
    if (available <= 0) {
      if (millis() - lastProgress > 9000) break;
      delay(1);
      continue;
    }
    const size_t wanted = min(sizeof(chunk), (size_t)available);
    const size_t received = stream->readBytes(chunk, wanted);
    if (received == 0) break;
    if (body.length() + received > limit) { overflow = true; break; }
    body.concat((const char *)chunk, received);
    lastProgress = millis();
  }
  http.end();
  if (overflow || body.length() > limit || (length >= 0 && body.length() != (size_t)length)) {
    body = "";
    Serial.println("[STORE] Response exceeded its limit or ended early.");
    return false;
  }
  return body.length() > 0;
}

static bool downloadGithubAsset(const String &url, const String &path, size_t limit) {
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  http.setTimeout(12000);
  http.setConnectTimeout(12000);
  http.setUserAgent("PixelDeck-FNK0104N/3.0");
  if (!http.begin(secureClient, url)) return false;
  const int status = http.GET();
  if (status != HTTP_CODE_OK) {
    Serial.printf("[STORE] Asset HTTP status %d for %s\n", status, url.c_str());
    http.end();
    return false;
  }
  const int contentLength = http.getSize();
  if (contentLength <= 0 || (size_t)contentLength > limit) {
    http.end();
    return false;
  }
  File output = SD_MMC.open(path, FILE_WRITE);
  if (!output) {
    http.end();
    return false;
  }
  Stream *stream = http.getStreamPtr();
  uint8_t chunk[1024];
  size_t total = 0;
  uint32_t lastProgress = millis();
  bool ok = true;
  while (stream && total < (size_t)contentLength) {
    const int available = stream->available();
    if (available <= 0) {
      if (millis() - lastProgress > 12000) { ok = false; break; }
      delay(1);
      continue;
    }
    const size_t wanted = min(sizeof(chunk), min((size_t)available, (size_t)contentLength - total));
    const size_t received = stream->readBytes(chunk, wanted);
    if (!received || total + received > limit || output.write(chunk, received) != received) {
      ok = false;
      break;
    }
    total += received;
    lastProgress = millis();
    lv_timer_handler();
    delay(1);
  }
  output.close();
  http.end();
  if (!ok || total != (size_t)contentLength) {
    SD_MMC.remove(path);
    return false;
  }
  return true;
}

static bool parseCart(const String &payload, GameCard &card) {
  scriptCompileErrorLine = 0;
  if (payload.length() == 0 || payload.length() > MAX_CART_BYTES) return false;
  memset(&card, 0, sizeof(card));
  String id, title, description, type;
  int speed = 8;
  int tickMs = 0;
  int api = 1;
  uint32_t accent = C_CYAN;
  int pos = 0;
  int codeStart = -1;
  while (pos < (int)payload.length()) {
    int end = payload.indexOf('\n', pos);
    if (end < 0) end = payload.length();
    String line = payload.substring(pos, end);
    line.trim();
    pos = end + 1;
    if (line == "--SCRIPT--" || line == "--PROGRAM--") { codeStart = pos; break; }
    if (line.length() == 0 || line.startsWith("#")) continue;
    const int split = line.indexOf('=');
    if (split <= 0) continue;
    String key = line.substring(0, split);
    String value = line.substring(split + 1);
    key.trim();
    value.trim();
    if (key == "id") id = value;
    else if (key == "title") title = value;
    else if (key == "description") description = value;
    else if (key == "type") type = value;
    else if (key == "speed") speed = constrain(value.toInt(), 1, 20);
    else if (key == "tick_ms") tickMs = constrain(value.toInt(), 20, 250);
    else if (key == "api") api = value.toInt();
    else if (key == "accent") accent = strtoul(value.c_str(), nullptr, 16) & 0xFFFFFF;
    else if (key.startsWith("asset.")) {
      const String assetName = key.substring(6);
      if (!validAssetName(assetName) || !validAssetFilename(value) || card.assetCount >= MAX_GAME_ASSETS) return false;
      for (uint8_t i = 0; i < card.assetCount; ++i) {
        if (!strcasecmp(card.assets[i].name, assetName.c_str()) ||
            !strcasecmp(card.assets[i].filename, value.c_str())) return false;
      }
      GameAssetDef &asset = card.assets[card.assetCount++];
      copyText(asset.name, sizeof(asset.name), assetName);
      copyText(asset.filename, sizeof(asset.filename), value);
    }
  }
  const GameType gameType = parseGameType(type);
  if (!validId(id) || title.length() == 0 || gameType == GAME_NONE) return false;
  if (card.assetCount && gameType != GAME_SCRIPT) return false;
  if (gameType == GAME_SCRIPT) {
    if ((api != 2 && api != 3) || codeStart < 0 || (size_t)codeStart >= payload.length()) return false;
    const char *source = payload.c_str() + codeStart;
    const size_t codeLength = payload.length() - codeStart;
    if (!compileScript(source, codeLength)) return false;
  } else {
    scriptCodeCount = 0;
  }
  copyText(card.id, sizeof(card.id), id);
  copyText(card.title, sizeof(card.title), title);
  copyText(card.description, sizeof(card.description), description);
  card.type = gameType;
  card.speed = (uint8_t)speed;
  card.tickMs = (uint16_t)tickMs;
  card.accent = accent;
  card.builtIn = false;
  return true;
}

static bool loadStoreCatalog() {
  if (WiFi.status() != WL_CONNECTED) {
    if (storeStatus) lv_label_set_text(storeStatus, "Connect to Wi-Fi in Settings first.");
    return false;
  }
  const String root = normalizeStoreRoot();
  if (root.length() == 0) {
    if (storeStatus) lv_label_set_text(storeStatus, "Set a public GitHub raw repository in Settings.");
    return false;
  }
  storeCount = 0;
  String body;
  if (storeStatus) {
    lv_label_set_text(storeStatus, "CONTACTING THE PIXELDECK STORE...");
    lv_refr_now(nullptr);
  }
  if (!getGithubText(root + "catalog.txt", body, 24000)) {
    if (storeStatus) lv_label_set_text(storeStatus, "Store unavailable. Check Wi-Fi and repository URL.");
    return false;
  }
  storeCount = 0;
  int pos = 0;
  while (pos < (int)body.length() && storeCount < MAX_STORE_ITEMS) {
    int end = body.indexOf('\n', pos);
    if (end < 0) end = body.length();
    String line = body.substring(pos, end);
    line.trim();
    pos = end + 1;
    if (line.length() == 0 || line.startsWith("#")) continue;

    String fields[6];
    int fieldStart = 0;
    bool validLine = true;
    for (int f = 0; f < 5; ++f) {
      const int separator = line.indexOf('|', fieldStart);
      if (separator < 0) { validLine = false; break; }
      fields[f] = line.substring(fieldStart, separator);
      fieldStart = separator + 1;
    }
    if (!validLine) continue;
    fields[5] = line.substring(fieldStart);
    for (int f = 0; f < 6; ++f) fields[f].trim();
    if (!validId(fields[0])) continue;
    GameCard card = {};
    copyText(card.id, sizeof(card.id), fields[0]);
    copyText(card.title, sizeof(card.title), fields[1]);
    copyText(card.description, sizeof(card.description), fields[5]);
    card.type = parseGameType(fields[2]);
    card.speed = (uint8_t)constrain(fields[3].toInt(), 1, 20);
    card.accent = strtoul(fields[4].c_str(), nullptr, 16) & 0xFFFFFF;
    card.builtIn = false;
    if (card.type == GAME_NONE || fields[1].length() == 0) continue;
    storeItems[storeCount].game = card;
    storeItems[storeCount].installed = false;
    for (uint8_t g = 0; g < gameCount; ++g) {
      if (sameId(games[g].id, card.id)) storeItems[storeCount].installed = true;
    }
    ++storeCount;
  }
  if (storeStatus) {
    char message[70];
    snprintf(message, sizeof(message), "%u GAMES IN THE PUBLIC CATALOG", storeCount);
    lv_label_set_text(storeStatus, message);
  }
  return true;
}

static bool clearFlatDirectory(const String &path) {
  File directory = SD_MMC.open(path);
  if (!directory) return true;
  if (!directory.isDirectory()) { directory.close(); return false; }
  File entry = directory.openNextFile();
  while (entry) {
    String child = entry.name();
    const bool isDirectory = entry.isDirectory();
    entry.close();
    if (!child.startsWith("/")) child = path + "/" + child;
    if (isDirectory) {
      if (!clearFlatDirectory(child)) { directory.close(); return false; }
      SD_MMC.rmdir(child);
    } else if (!SD_MMC.remove(child)) {
      directory.close();
      return false;
    }
    entry = directory.openNextFile();
  }
  directory.close();
  return SD_MMC.rmdir(path);
}

static void installStoreItem(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  StoreItem *item = (StoreItem *)lv_event_get_user_data(event);
  if (!item || item->installed) return;
  if (!sdReady) {
    if (storeStatus) lv_label_set_text(storeStatus, "Insert a microSD card to save downloads.");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    if (storeStatus) lv_label_set_text(storeStatus, "Connect to Wi-Fi in Settings first.");
    return;
  }
  const String root = normalizeStoreRoot();
  if (root.length() == 0) {
    if (storeStatus) lv_label_set_text(storeStatus, "Set your public GitHub raw URL in Settings.");
    return;
  }

  String url = root + "games/" + String(item->game.id) + ".cart";
  String payload;
  if (storeStatus) {
    lv_label_set_text(storeStatus, "DOWNLOADING CART...");
    lv_refr_now(nullptr);
  }
  if (!getGithubText(url, payload, MAX_CART_BYTES)) {
    if (storeStatus) lv_label_set_text(storeStatus, "Download failed. The cart was not installed.");
    return;
  }
  GameCard downloaded;
  if (!parseCart(payload, downloaded) || !sameId(downloaded.id, item->game.id) ||
      downloaded.type != item->game.type) {
    if (storeStatus) {
      char message[80];
      if (scriptCompileErrorLine) snprintf(message, sizeof(message), "SCRIPT ERROR ON LINE %u. CART NOT SAVED.", (unsigned)scriptCompileErrorLine);
      else copyLiteral(message, sizeof(message), "Cart validation failed. Nothing was saved.");
      lv_label_set_text(storeStatus, message);
    }
    return;
  }

  String assetDirectory, stagedAssets, assetBackup;
  bool assetDirectoryHadPrevious = false;
  bool stagedAssetsReady = false;
  if (downloaded.assetCount > 0) {
    const String gameDirectory = String(GAMES_DIR) + "/" + downloaded.id;
    assetDirectory = gameDirectory + "/assets";
    stagedAssets = gameDirectory + "/assets_stage";
    assetBackup = gameDirectory + "/assets_backup";
    SD_MMC.mkdir(gameDirectory);
    if (SD_MMC.exists(stagedAssets) && !clearFlatDirectory(stagedAssets)) {
      if (storeStatus) lv_label_set_text(storeStatus, "Could not prepare the asset staging folder.");
      return;
    }
    if (!SD_MMC.mkdir(stagedAssets) && !SD_MMC.exists(stagedAssets)) {
      if (storeStatus) lv_label_set_text(storeStatus, "Could not create the asset folder on SD.");
      return;
    }
    size_t totalAssetBytes = 0;
    bool assetsOk = true;
    for (uint8_t i = 0; i < downloaded.assetCount; ++i) {
      const GameAssetDef &asset = downloaded.assets[i];
      const String assetUrl = root + "games/" + downloaded.id + "/assets/" + asset.filename;
      const String stagedPath = stagedAssets + "/" + asset.filename;
      if (storeStatus) {
        char message[72];
        snprintf(message, sizeof(message), "DOWNLOADING ASSET %u OF %u...", (unsigned)(i + 1), (unsigned)downloaded.assetCount);
        lv_label_set_text(storeStatus, message);
        lv_refr_now(nullptr);
      }
      if (!downloadGithubAsset(assetUrl, stagedPath, MAX_ASSET_BYTES)) {
        assetsOk = false;
        break;
      }
      File savedAsset = SD_MMC.open(stagedPath, FILE_READ);
      const size_t assetSize = savedAsset ? savedAsset.size() : 0;
      if (savedAsset) savedAsset.close();
      if (assetSize == 0 || totalAssetBytes + assetSize > MAX_TOTAL_ASSET_BYTES) {
        assetsOk = false;
        break;
      }
      totalAssetBytes += assetSize;
    }
    if (!assetsOk) {
      clearFlatDirectory(stagedAssets);
      if (storeStatus) lv_label_set_text(storeStatus, "Asset download failed or exceeded the 2 MB game limit.");
      return;
    }
    stagedAssetsReady = true;
  }

  const String target = String(GAMES_DIR) + "/" + downloaded.id + ".cart";
  const String temporary = target + ".tmp";
  File file = SD_MMC.open(temporary, FILE_WRITE);
  if (!file) {
    if (stagedAssetsReady) clearFlatDirectory(stagedAssets);
    if (storeStatus) lv_label_set_text(storeStatus, "Could not open the SD card for writing.");
    return;
  }
  const size_t written = file.print(payload);
  file.close();
  if (written != payload.length()) {
    SD_MMC.remove(temporary);
    if (stagedAssetsReady) clearFlatDirectory(stagedAssets);
    if (storeStatus) lv_label_set_text(storeStatus, "SD write was incomplete; the partial file was removed.");
    return;
  }
  if (stagedAssetsReady) {
    if (SD_MMC.exists(assetBackup) && !clearFlatDirectory(assetBackup)) {
      SD_MMC.remove(temporary);
      clearFlatDirectory(stagedAssets);
      if (storeStatus) lv_label_set_text(storeStatus, "Could not clean the previous asset backup.");
      return;
    }
    assetDirectoryHadPrevious = SD_MMC.exists(assetDirectory);
    if (assetDirectoryHadPrevious && !SD_MMC.rename(assetDirectory, assetBackup)) {
      SD_MMC.remove(temporary);
      clearFlatDirectory(stagedAssets);
      if (storeStatus) lv_label_set_text(storeStatus, "Could not safely replace the old game assets.");
      return;
    }
    if (!SD_MMC.rename(stagedAssets, assetDirectory)) {
      if (assetDirectoryHadPrevious) SD_MMC.rename(assetBackup, assetDirectory);
      SD_MMC.remove(temporary);
      clearFlatDirectory(stagedAssets);
      if (storeStatus) lv_label_set_text(storeStatus, "Could not activate the downloaded assets.");
      return;
    }
  }
  const String backup = target + ".bak";
  const bool hadPreviousCart = SD_MMC.exists(target);
  SD_MMC.remove(backup);
  if (hadPreviousCart && !SD_MMC.rename(target, backup)) {
    SD_MMC.remove(temporary);
    if (stagedAssetsReady) {
      clearFlatDirectory(assetDirectory);
      if (assetDirectoryHadPrevious) SD_MMC.rename(assetBackup, assetDirectory);
    }
    if (storeStatus) lv_label_set_text(storeStatus, "Could not safely replace the existing cart.");
    return;
  }
  if (!SD_MMC.rename(temporary, target)) {
    SD_MMC.remove(temporary);
    if (hadPreviousCart) SD_MMC.rename(backup, target);
    if (stagedAssetsReady) {
      clearFlatDirectory(assetDirectory);
      if (assetDirectoryHadPrevious) SD_MMC.rename(assetBackup, assetDirectory);
    }
    if (storeStatus) lv_label_set_text(storeStatus, "Could not finish saving the cart to SD.");
    return;
  }
  SD_MMC.remove(backup);
  if (stagedAssetsReady && assetDirectoryHadPrevious) clearFlatDirectory(assetBackup);

  addGame(downloaded);
  item->installed = true;
  if (storeStatus) {
    String message = String(downloaded.title) + " SAVED TO SD";
    lv_label_set_text(storeStatus, message.c_str());
  }
  lv_obj_t *button = lv_event_get_target(event);
  if (button) {
    lv_label_set_text(lv_obj_get_child(button, 0), "SAVED");
    lv_obj_add_state(button, LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(button, color(0x344259), 0);
  }
}

static void drawStoreItems() {
  if (!storeGrid) return;
  lv_obj_clean(storeGrid);
  if (storeCount == 0) {
    makeLabel(storeGrid, "No catalog loaded yet. Set the GitHub raw URL, then press REFRESH.",
              12, 18, 410, 48, C_MUTED);
    return;
  }
  for (uint8_t i = 0; i < storeCount; ++i) {
    StoreItem *item = &storeItems[i];
    lv_obj_t *card = lv_obj_create(storeGrid);
    lv_obj_set_size(card, 210, 88);
    lv_obj_set_style_bg_color(card, color(C_PANEL), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, color(item->game.accent), 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 5, 0);
    makeLabel(card, item->game.title, 7, 5, 190, 20, item->game.accent, &lv_font_montserrat_20);
    makeLabel(card, item->game.description, 7, 29, 190, 27, C_WHITE);
    makeLabel(card, gameTypeName(item->game.type), 7, 61, 88, 18, C_MUTED);
    lv_obj_t *button = makeButton(card, item->installed ? "SAVED" : "GET", 136, 57, 63, 25,
                                  item->installed ? 0x344259 : C_MAGENTA,
                                  item->installed ? nullptr : installStoreItem, item);
    if (item->installed) lv_obj_add_state(button, LV_STATE_DISABLED);
  }
}

static void onRefreshStore(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  loadStoreCatalog();
  drawStoreItems();
}

static void showStore() {
  screenMode = SCREEN_STORE;
  clearUiRefs();
  lv_obj_clean(lv_scr_act());
  screenBackground();
  topBar("PIXELDECK GAME STORE", true);
  storeStatus = makeLabel(lv_scr_act(), "PUBLIC GITHUB CATALOG  //  GAME DATA ONLY", 18, 50, 340, 20, C_MUTED);
  makeButton(lv_scr_act(), "REFRESH", 378, 47, 88, 30, C_PANEL_HI, onRefreshStore);
  storeGrid = lv_obj_create(lv_scr_act());
  lv_obj_set_pos(storeGrid, 14, 82);
  lv_obj_set_size(storeGrid, 452, 224);
  lv_obj_set_style_bg_opa(storeGrid, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(storeGrid, 0, 0);
  lv_obj_set_style_pad_all(storeGrid, 3, 0);
  lv_obj_set_style_pad_row(storeGrid, 8, 0);
  lv_obj_set_style_pad_column(storeGrid, 8, 0);
  lv_obj_set_flex_flow(storeGrid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(storeGrid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_scroll_dir(storeGrid, LV_DIR_VER);
  if (storeCount == 0) {
    makeLabel(storeGrid, "Set a public raw GitHub repository in Settings, then press REFRESH.",
              12, 18, 410, 48, C_MUTED);
  } else drawStoreItems();
}

static void showHelp() {
  screenMode = SCREEN_HELP;
  clearUiRefs();
  lv_obj_clean(lv_scr_act());
  screenBackground();
  topBar("QUICK START", true);
  makeLabel(lv_scr_act(), "CONTROLS", 22, 58, 140, 24, C_CYAN, &lv_font_montserrat_20);
  makeLabel(lv_scr_act(), "<  >  move left / right     ^  v  move up / down     A / B  game actions",
            22, 91, 436, 32, C_WHITE);
  makeLabel(lv_scr_act(), "Script API 3: original rules, up to 160 entities, custom bitmap sprites, sound, physics, collisions, and A/B actions.",
            22, 130, 436, 42, C_MUTED);
  makeLabel(lv_scr_act(), "STORE SETUP", 22, 182, 180, 24, C_MAGENTA, &lv_font_montserrat_20);
  makeLabel(lv_scr_act(), "Connect to 2.4 GHz Wi-Fi. Publish catalog.txt, games/<id>.cart, and each game's games/<id>/assets/ files in a public GitHub repo. Set its raw URL in Settings.",
            22, 215, 436, 53, C_WHITE);
  makeLabel(lv_scr_act(), "The SD card stores downloaded games, sprites, and audio. Built-in games and high scores work without a card.",
            22, 275, 436, 34, C_MUTED);
}

static void saveSettings() {
  if (ssidField) savedSsid = lv_textarea_get_text(ssidField);
  if (passwordField) savedPassword = lv_textarea_get_text(passwordField);
  if (storeField) savedStoreRoot = lv_textarea_get_text(storeField);
  savedSsid.trim();
  savedStoreRoot.trim();
  preferences.putString("ssid", savedSsid);
  preferences.putString("password", savedPassword);
  preferences.putString("repo", savedStoreRoot);
  if (settingsStatus) lv_label_set_text(settingsStatus, "SETTINGS SAVED ON DEVICE");
}

static void onSaveSettings(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) saveSettings();
}

static void onConnectWifi(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  saveSettings();
  if (savedSsid.length() == 0) {
    if (settingsStatus) lv_label_set_text(settingsStatus, "ENTER A WI-FI NAME FIRST");
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
  wifiAttemptActive = true;
  wifiAttemptStarted = millis();
  if (settingsStatus) lv_label_set_text(settingsStatus, "CONNECTING TO WI-FI...");
}

static void onKeyboardFocus(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_FOCUSED || !keyboard) return;
  lv_keyboard_set_textarea(keyboard, lv_event_get_target(event));
  lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void onKeyboardEvent(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_READY || lv_event_get_code(event) == LV_EVENT_CANCEL) {
    if (keyboard) lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
  }
}

static lv_obj_t *makeTextField(const char *label, int y, const char *value, bool password = false) {
  makeLabel(lv_scr_act(), label, 20, y + 7, 104, 24, C_MUTED);
  lv_obj_t *field = lv_textarea_create(lv_scr_act());
  lv_obj_set_pos(field, 128, y);
  lv_obj_set_size(field, 332, 34);
  lv_textarea_set_one_line(field, true);
  lv_textarea_set_max_length(field, 112);
  lv_textarea_set_text(field, value);
  lv_textarea_set_password_mode(field, password);
  lv_obj_set_style_bg_color(field, color(C_PANEL), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(field, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(field, color(0x344259), LV_PART_MAIN);
  lv_obj_set_style_text_color(field, color(C_WHITE), LV_PART_MAIN);
  lv_obj_set_style_text_font(field, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_add_event_cb(field, onKeyboardFocus, LV_EVENT_FOCUSED, nullptr);
  return field;
}

static void showSettings() {
  screenMode = SCREEN_SETTINGS;
  clearUiRefs();
  lv_obj_clean(lv_scr_act());
  screenBackground();
  topBar("CONSOLE SETTINGS", true);
  ssidField = makeTextField("WI-FI NAME", 52, savedSsid.c_str());
  passwordField = makeTextField("PASSWORD", 96, savedPassword.c_str(), true);
  storeField = makeTextField("GITHUB RAW URL", 140, savedStoreRoot.c_str());
  makeButton(lv_scr_act(), "SAVE", 128, 186, 96, 34, C_PANEL_HI, onSaveSettings);
  makeButton(lv_scr_act(), "CONNECT", 236, 186, 112, 34, C_CYAN, onConnectWifi);
  settingsStatus = makeLabel(lv_scr_act(), "WI-FI 2.4 GHz  //  STORE REQUIRES SD CARD",
                             20, 232, 440, 22, C_MUTED);
  keyboard = lv_keyboard_create(lv_scr_act());
  lv_obj_set_size(keyboard, SCREEN_W, 126);
  lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(keyboard, onKeyboardEvent, LV_EVENT_ALL, nullptr);
}

static bool loadCartFile(File &file, GameCard &card) {
  if (!file || file.isDirectory() || file.size() > MAX_CART_BYTES) return false;
  String payload;
  payload.reserve(file.size());
  while (file.available()) payload += (char)file.read();
  return parseCart(payload, card);
}

static void loadInstalledGames() {
  if (!sdReady) return;
  File directory = SD_MMC.open(GAMES_DIR);
  if (!directory || !directory.isDirectory()) return;
  File file = directory.openNextFile();
  while (file) {
    const String name = file.name();
    if (!file.isDirectory() && name.endsWith(".cart")) {
      GameCard card;
      if (loadCartFile(file, card)) addGame(card);
    }
    file.close();
    file = directory.openNextFile();
  }
  directory.close();
}

static void mountStorage() {
  SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN, SD_D1_PIN, SD_D2_PIN, SD_D3_PIN);
  sdReady = SD_MMC.begin("/sdcard", false, false, 20000, 8);
  if (sdReady) {
    SD_MMC.mkdir("/ARCADE");
    SD_MMC.mkdir(GAMES_DIR);
    loadInstalledGames();
    Serial.printf("[SD] Ready, %llu MB\n", SD_MMC.cardSize() / (1024ULL * 1024ULL));
  } else {
    Serial.println("[SD] Not mounted; built-in games remain available.");
  }
}

static void beginWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  if (savedSsid.length() > 0) {
    WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
    wifiAttemptActive = true;
    wifiAttemptStarted = millis();
  }
}

static void beginDisplay() {
  panel = new ST77922();
  panel->Init();
  panel->Set_Rotation(1);
  touch = new ST77922_TOUCH();
  touch->init();
  touch->Set_Rotation(1);

  lv_init();
  lv_disp_draw_buf_init(&displayDrawBuffer, displayBuffer, nullptr, SCREEN_W * 20);
  lv_disp_drv_init(&displayDriver);
  displayDriver.hor_res = SCREEN_W;
  displayDriver.ver_res = SCREEN_H;
  displayDriver.draw_buf = &displayDrawBuffer;
  displayDriver.flush_cb = drawFlush;
  lv_disp_drv_register(&displayDriver);

  lv_indev_drv_init(&touchDriver);
  touchDriver.type = LV_INDEV_TYPE_POINTER;
  touchDriver.read_cb = readTouch;
  lv_indev_drv_register(&touchDriver);
}

void setup() {
  Serial.begin(115200);
  delay(150);
  Serial.println("\n[PIXELDECK] FNK0104N boot");
  preferences.begin("pixeldeck", false);
  savedSsid = preferences.getString("ssid", "");
  savedPassword = preferences.getString("password", "");
  savedStoreRoot = preferences.getString("repo", STORE_ROOT_DEFAULT);

  randomSeed(esp_random());
  beginDisplay();
  mountStorage();
  audioReady = beginAudio();
  addBuiltIn("snake", "POCKET SNAKE", "Collect the signal. Avoid your own trail.", GAME_SNAKE, 12, C_CYAN);
  addBuiltIn("pong", "VECTOR PONG", "A neon rally against the cabinet.", GAME_PONG, 10, C_MAGENTA);
  addBuiltIn("breakout", "BRICK BREAKER", "Clear four rows and keep the ball alive.", GAME_BREAKOUT, 9, C_AMBER);
  addBuiltIn("invaders", "ASTRO RAIDERS", "Hold the line and clear the alien wave.", GAME_INVADERS, 11, C_LIME);
  beginWiFi();
  showHome();
  lastStatusTick = millis();
}

void loop() {
  if (audioReady) {
    audio.loop();
    if (effectAudioActive && !audio.isRunning()) {
      effectAudioActive = false;
      if (backgroundAudioName[0] && backgroundAudioLoops) playGameAudio(backgroundAudioName, true, backgroundAudioVolume);
    }
  }
  const uint32_t now = millis();
  if (wifiAttemptActive) {
    if (WiFi.status() == WL_CONNECTED) {
      wifiAttemptActive = false;
      Serial.printf("[WIFI] Connected: %s\n", WiFi.localIP().toString().c_str());
      if (settingsStatus) {
        String message = String("CONNECTED  //  ") + WiFi.localIP().toString();
        lv_label_set_text(settingsStatus, message.c_str());
      }
      refreshStatus();
    } else if (now - wifiAttemptStarted > 18000) {
      wifiAttemptActive = false;
      if (settingsStatus) lv_label_set_text(settingsStatus, "CONNECTION TIMEOUT  //  CHECK WI-FI DETAILS");
    }
  }
  const uint16_t frameMs = activeGame.tickMs ? activeGame.tickMs : max(35, 145 - activeGame.speed * 5);
  if (screenMode == SCREEN_PLAY && now - lastGameTick >= frameMs) {
    lastGameTick = now;
    gameStep();
  }
  if (screenMode == SCREEN_PLAY && activeGame.type != GAME_SCRIPT && now - lastStarTick >= 420) {
    lastStarTick = now;
    for (uint8_t i = 0; i < 15; ++i) {
      const uint8_t index = 48 + i;
      if (entities[index] && random(0, 4) == 0) {
        const uint32_t twinkle = (random(0, 2) == 0) ? 0x25334B : 0x52647F;
        lv_obj_set_style_bg_color(entities[index], color(twinkle), 0);
      }
    }
  }
  if (now - lastStatusTick >= 1000) {
    lastStatusTick = now;
    refreshStatus();
    if (screenMode == SCREEN_SETTINGS && settingsStatus && WiFi.status() == WL_CONNECTED) {
      String message = String("CONNECTED  //  ") + WiFi.localIP().toString();
      lv_label_set_text(settingsStatus, message.c_str());
    }
  }
  lv_timer_handler();
  delay(2);
}
