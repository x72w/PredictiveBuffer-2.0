// Predictive Input Buffer - Windows + Android64 (single file).
// Trajectory-sim input buffer: F6 (Windows) or the pause-menu "Buffer" button cycles
// 14 stages (ticks: 2 4 8 16 20 24 28 32 40 48 56 64 80 96; wave capped at 8) -> OFF.
// 4 ticks = 1 frame @ 60fps.
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/PauseLayer.hpp>
#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#endif
#include <cstdio>
#include <cstdarg>
#include <algorithm>
using namespace geode::prelude;
constexpr int SPAM_INTERVAL_TICKS = 8;
constexpr int SPAM_STREAK_REQUIRED = 3;
static void bufLog(const char* fmt, ...) {
  char buf[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  {
    std::string line = buf;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    log::info("{}", line);
  }
#ifdef GEODE_IS_WINDOWS
  printf("%s", buf);
#endif
}
class TrajectoryBufferSim {
private:
  PlayerObject* m_simPlayer1 = nullptr;
  PlayerObject* m_simPlayer2 = nullptr;
  bool m_simulating = false;
  bool m_firingBuffered = false;
  bool m_simulationDead = false;
  bool m_physicalHolding = false;
  bool m_gameHolding = false;
  int  m_presetIndex = 0;
  bool m_enabled = true;
  int  m_maxTicksNormal = 2;
  int  m_maxTicksWave = 2;
  int  m_waitTicks = -1;
  bool m_bufferedPush = true;
  bool m_queuedOpposite = false;
  float m_frameDt = 0.25f;
  int  m_ticksSinceLastInput = 999;
  int  m_rapidInputStreak = 0;
  PlayerObject* createTrajectoryPlayer() {
    PlayLayer* pl = PlayLayer::get();
    if (!pl) return nullptr;
    PlayerObject* player = PlayerObject::create(1, 1, pl, pl, true);
    player->retain();
    player->setPosition({0, 105});
    player->setVisible(false);
    pl->m_objectLayer->addChild(player);
    return player;
  }
  void resetCollisionLog(PlayerObject* self) {
    self->m_collisionLogTop->removeAllObjects();
    self->m_collisionLogBottom->removeAllObjects();
    self->m_collisionLogLeft->removeAllObjects();
    self->m_collisionLogRight->removeAllObjects();
  }
  void syncSimPlayer(PlayerObject* sim, PlayerObject* real) {
    sim->setVisible(false);
    sim->copyAttributes(real);
    sim->setScale(real->getScale());
    sim->setContentSize(real->getContentSize());
    sim->m_width = real->m_width;
    sim->m_height = real->m_height;
    sim->m_gravityMod = real->m_gravityMod;
    sim->m_isOnGround = real->m_isOnGround;
    sim->m_isUpsideDown = real->m_isUpsideDown;
    sim->m_vehicleSize = real->m_vehicleSize;
    sim->m_playerSpeed = real->m_playerSpeed;
  }
  float getValidDt() const {
    if (m_frameDt > 0.001f && m_frameDt < 2.0f) {
      return m_frameDt;
    }
    return 0.25f;
  }
public:
  void init() {
    PlayLayer* pl = PlayLayer::get();
    if (!pl) return;
    m_simPlayer1 = createTrajectoryPlayer();
    m_simPlayer2 = createTrajectoryPlayer();
    m_waitTicks = -1;
    m_queuedOpposite = false;
    m_physicalHolding = false;
    m_gameHolding = false;
    m_simulating = false;
    m_firingBuffered = false;
    m_ticksSinceLastInput = 999;
    m_rapidInputStreak = 0;
  }
  void quit() {
    if (m_simPlayer1) {
      m_simPlayer1->release();
      m_simPlayer1 = nullptr;
    }
    if (m_simPlayer2) {
      m_simPlayer2->release();
      m_simPlayer2 = nullptr;
    }
    m_simulating = false;
    m_firingBuffered = false;
    m_waitTicks = -1;
    m_queuedOpposite = false;
    m_rapidInputStreak = 0;
  }
  void cyclePreset() {
    static const int kAll[15] = {2, 4, 8, 16, 20, 24, 28, 32, 40, 48, 56, 64, 80, 96, 0};
    static const int kWave[15] = {2, 4, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 0};
    m_presetIndex = (m_presetIndex + 1) % 15;
    m_waitTicks = -1;
    m_queuedOpposite = false;
    m_enabled = m_presetIndex < 14;
    m_maxTicksNormal = kAll[m_presetIndex];
    m_maxTicksWave = kWave[m_presetIndex];
    if (m_enabled) bufLog("[InputBuffer] F6 Cycle -> STAGE %d: %d Ticks All | %d Ticks Wave\n", m_presetIndex + 1, m_maxTicksNormal, m_maxTicksWave);
    else bufLog("[InputBuffer] F6 Cycle -> STAGE 15: OFF (100%% Raw Input)\n");
  }
  std::string presetLabel() const {
    return m_presetIndex < 14 ? "Buffer: S" + std::to_string(m_presetIndex + 1) : std::string("Buffer: OFF");
  }
  bool isSimulating() const { return m_simulating; }
  bool isFiringBuffered() const { return m_firingBuffered; }
  bool checkSimulationDeath(PlayerObject* player) {
    if (m_simulating && (player == m_simPlayer1 || player == m_simPlayer2)) {
      m_simulationDead = true;
      return true;
    }
    m_waitTicks = -1;
    m_queuedOpposite = false;
    m_gameHolding = false;
    m_rapidInputStreak = 0;
    return false;
  }
  void setFrameDelta(float dt) {
    PlayLayer* pl = PlayLayer::get();
    if (pl && pl->m_gameState.m_timeWarp > 0.f) {
      m_frameDt = dt / pl->m_gameState.m_timeWarp;
    } else {
      m_frameDt = dt;
    }
  }
  int getSurvivalTicks(PlayLayer* pl, PlayerObject* realP1, PlayerObject* realP2, bool isDual, int delayFrames, bool actionPush, int maxTicks) {
    if (!pl || !m_simPlayer1 || !realP1) return maxTicks;
    m_simulating = true;
    m_simulationDead = false;
    syncSimPlayer(m_simPlayer1, realP1);
    if (isDual && m_simPlayer2 && realP2) {
      syncSimPlayer(m_simPlayer2, realP2);
    }
    auto setBtn = [&](bool push) {
      auto act = [&](PlayerObject* p) {
        if (push) p->pushButton(PlayerButton::Jump);
        else p->releaseButton(PlayerButton::Jump);
      };
      act(m_simPlayer1);
      if (isDual && m_simPlayer2) act(m_simPlayer2);
    };
    if (delayFrames > 0) setBtn(!actionPush);
    float dt = getValidDt();
    int survived = 0;
    bool isContinuousMode = realP1->m_isDart || realP1->m_isBird || realP1->m_isSwing ||
                (isDual && realP2 && (realP2->m_isDart || realP2->m_isBird || realP2->m_isSwing));
    int airborneTicks = 0;
    int safeLandedTicks = 0;
    for (int i = 0; i < delayFrames + maxTicks; i++) {
      if (i == delayFrames) setBtn(actionPush);
      m_simPlayer1->update(dt);
      m_simPlayer1->updateRotation(dt);
      if (isDual && m_simPlayer2) {
        m_simPlayer2->update(dt);
        m_simPlayer2->updateRotation(dt);
      }
      if (m_simulationDead) break;
      resetCollisionLog(m_simPlayer1);
      pl->checkCollisions(m_simPlayer1, dt, false);
      if (m_simulationDead) break;
      if (isDual && m_simPlayer2 && realP2) {
        resetCollisionLog(m_simPlayer2);
        pl->checkCollisions(m_simPlayer2, dt, false);
        if (m_simulationDead) break;
      }
      if (i >= delayFrames) {
        survived++;
        if (!isContinuousMode) {
          bool p1Grounded = m_simPlayer1->m_isOnGround;
          bool p2Grounded = (!isDual || !m_simPlayer2) ? true : m_simPlayer2->m_isOnGround;
          if (!p1Grounded || !p2Grounded) {
            airborneTicks++;
            safeLandedTicks = 0;
          } else if (airborneTicks >= 15) {
            safeLandedTicks++;
            if (safeLandedTicks >= 10) {
              survived = maxTicks;
              break;
            }
          }
        }
      }
    }
    m_simulating = false;
    return survived;
  }
  bool interceptInput(GJBaseGameLayer* layer, bool push, bool isPlayer1) {
    PlayLayer* pl = PlayLayer::get();
    if (!pl) return true;
    bool isDual = pl->m_gameState.m_isDualMode && pl->m_player2 != nullptr;
    bool isTwoPlayerControl = pl->m_level && pl->m_level->m_twoPlayerMode;
    if (!isPlayer1) {
      if (isDual && !isTwoPlayerControl && (m_waitTicks >= 0 || m_waitTicks == -2)) {
        return false;
      }
      return true;
    }
    m_physicalHolding = push;
    if (!m_enabled) {
      m_gameHolding = push;
      return true;
    }
    PlayerObject* p1 = layer->m_player1;
    PlayerObject* p2 = isDual ? pl->m_player2 : nullptr;
    if (!p1 || p1->m_isDead) {
      m_waitTicks = -1;
      m_queuedOpposite = false;
      m_gameHolding = push;
      return true;
    }
    if (p1->m_isShip || (isDual && p2 && p2->m_isShip)) {
      m_waitTicks = -1;
      m_queuedOpposite = false;
      m_gameHolding = push;
      return true;
    }
    if (m_waitTicks >= 0) {
      if (push != m_bufferedPush) {
        m_queuedOpposite = true;
      } else {
        m_queuedOpposite = false;
      }
      m_ticksSinceLastInput = 0;
      return false;
    }
    if (m_waitTicks == -2) {
      m_waitTicks = -1;
      m_queuedOpposite = false;
      if (m_gameHolding != !m_bufferedPush) {
        m_SimFireButton(pl, !m_bufferedPush);
      }
    }
    bool isWave = p1->m_isDart || (isDual && p2 && p2->m_isDart);
    bool isMiniWave = (p1->m_isDart && p1->m_vehicleSize < 1.0f) ||
             (isDual && p2 && p2->m_isDart && p2->m_vehicleSize < 1.0f);
    if (m_ticksSinceLastInput < SPAM_INTERVAL_TICKS) {
      m_rapidInputStreak++;
    } else {
      m_rapidInputStreak = 0;
    }
    m_ticksSinceLastInput = 0;
    if (isWave && m_rapidInputStreak >= SPAM_STREAK_REQUIRED) {
      m_gameHolding = push;
      return true;
    }
    bool releaseAffectsPhysics = p1->m_isDart || p1->m_isRobot ||
                  (isDual && p2 && (p2->m_isDart || p2->m_isRobot));
    if (!push && !releaseAffectsPhysics) {
      m_gameHolding = false;
      return true;
    }
    m_queuedOpposite = false;
    const char* act = push ? "PRESS" : "RELEASE";
    int targetHorizon = isMiniWave ? 50 : (isWave ? 65 : 160);
    int maxDelayFrames = isWave ? m_maxTicksWave : m_maxTicksNormal;
    int ticksFrame0 = getSurvivalTicks(pl, p1, p2, isDual, 0, push, targetHorizon);
    if (ticksFrame0 >= targetHorizon) {
      m_gameHolding = push;
      return true;
    }
    int bestDelay = 0;
    int bestTicks = ticksFrame0;
    for (int delay = 1; delay <= maxDelayFrames; delay++) {
      int ticks = getSurvivalTicks(pl, p1, p2, isDual, delay, push, targetHorizon);
      if (isWave) {
        int minClearedGain = (delay * 2) + 10;
        if (ticks >= targetHorizon && (ticks - ticksFrame0) >= (delay * 2) + 6) {
          bestDelay = delay;
          bestTicks = ticks;
          break;
        }
        if (ticks >= ticksFrame0 + minClearedGain && ticks > bestTicks + 2) {
          bestDelay = delay;
          bestTicks = ticks;
          if (ticks >= targetHorizon) break;
        }
      } else {
        if (ticks >= targetHorizon) {
          bestDelay = delay;
          bestTicks = ticks;
          break;
        }
        if (ticks > bestTicks + 12) {
          bestDelay = delay;
          bestTicks = ticks;
        }
      }
    }
    if (bestDelay > 0) {
      bufLog("[InputBuffer] *** SAVED +%d TICKS (%.2f frames @ 60fps) %s! (Frame0: %d -> Delayed: %d) [Limit: %d] ***\n",
        bestDelay, bestDelay / 4.0f, act, ticksFrame0, bestTicks, maxDelayFrames);
      m_bufferedPush = push;
      m_waitTicks = bestDelay;
      return false;
    }
    m_gameHolding = push;
    return true;
  }
  void onPlayerUpdate(PlayerObject* player) {
    PlayLayer* pl = PlayLayer::get();
    if (!pl || player != pl->m_player1) return;
#ifdef GEODE_IS_WINDOWS
    if (GetAsyncKeyState(VK_F6) & 1) {
      cyclePreset();
    }
#endif
    if (!m_enabled) return;
    if (m_ticksSinceLastInput < 999) {
      m_ticksSinceLastInput++;
    }
    if (player->m_isShip || (pl->m_gameState.m_isDualMode && pl->m_player2 && pl->m_player2->m_isShip)) {
      m_waitTicks = -1;
      m_queuedOpposite = false;
      if (m_gameHolding != m_physicalHolding) {
        m_SimFireButton(pl, m_physicalHolding);
      }
      return;
    }
    if (m_waitTicks == 0) {
      m_SimFireButton(pl, m_bufferedPush);
      if (m_queuedOpposite) {
        m_waitTicks = -2;
        m_queuedOpposite = false;
      } else {
        m_waitTicks = -1;
      }
    } else if (m_waitTicks > 0) {
      m_waitTicks--;
    } else if (m_waitTicks == -2) {
      m_SimFireButton(pl, !m_bufferedPush);
      m_queuedOpposite = false;
      m_waitTicks = -1;
    } else if (m_waitTicks == -1) {
      if (m_gameHolding != m_physicalHolding) {
        m_SimFireButton(pl, m_physicalHolding);
      }
    }
  }
  void m_SimFireButton(PlayLayer* pl, bool push) {
    m_firingBuffered = true;
    m_gameHolding = push;
    pl->handleButton(push, 1, true);
    if (pl->m_gameState.m_isDualMode && (!pl->m_level || !pl->m_level->m_twoPlayerMode)) {
      pl->handleButton(push, 1, false);
    }
    m_firingBuffered = false;
  }
};
static TrajectoryBufferSim s_sim;
$on_mod(Loaded) {
#ifdef GEODE_IS_WINDOWS
  AllocConsole();
  FILE* f;
  freopen_s(&f, "CONOUT$", "w", stdout);
  SetConsoleTitleA("Geode Predictive Input Buffer - Live Console");
#endif
  bufLog("============================================================\n");
  bufLog(" [InputBuffer] v4.6 LOADED (Wave Spikes/Saws Trajectory Fix)\n");
  bufLog(" [InputBuffer] Starting Mode: STAGE 1 (2 All / 2 Wave)\n");
  bufLog(" [InputBuffer] F6 (Windows) or the pause-menu Buffer button cycles 14 stages (2..96 ticks) -> OFF\n");
  bufLog("============================================================\n");
}
class $modify(BufPlayLayer, PlayLayer) {
  static void onModify(auto& self) {
    (void)self.setHookPriority("PlayLayer::destroyPlayer", -1000000);
    (void)self.setHookPriority("PlayLayer::playEndAnimationToPos", -1000000);
  }
  bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
    if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
    s_sim.init();
    return true;
  }
  void destroyPlayer(PlayerObject* player, GameObject* gameObject) {
    if (s_sim.checkSimulationDeath(player)) return;
    PlayLayer::destroyPlayer(player, gameObject);
  }
  void playEndAnimationToPos(cocos2d::CCPoint p0) {
    if (s_sim.isSimulating()) return;
    PlayLayer::playEndAnimationToPos(p0);
  }
  void onQuit() {
    PlayLayer::onQuit();
    s_sim.quit();
  }
};
class $modify(BufEditorLayer, LevelEditorLayer) {
  bool init(GJGameLevel* level, bool unk) {
    bool res = LevelEditorLayer::init(level, unk);
    s_sim.quit();
    return res;
  }
};
class $modify(BufHardStreak, HardStreak) {
  static void onModify(auto& self) {
    (void)self.setHookPriority("HardStreak::addPoint", -1000000);
  }
  void addPoint(cocos2d::CCPoint p0) {
    if (s_sim.isSimulating()) return;
    HardStreak::addPoint(p0);
  }
};
class $modify(BufEffectGameObject, EffectGameObject) {
  static void onModify(auto& self) {
    (void)self.setHookPriority("EffectGameObject::triggerObject", -1000000);
  }
  void triggerObject(GJBaseGameLayer* p0, int p1, const gd::vector<int>* p2) {
    if (s_sim.isSimulating()) return;
    EffectGameObject::triggerObject(p0, p1, p2);
  }
};
class $modify(BufGameObject, GameObject) {
  static void onModify(auto& self) {
    (void)self.setHookPriority("GameObject::playShineEffect", -1000000);
  }
  void playShineEffect() {
    if (s_sim.isSimulating()) return;
    GameObject::playShineEffect();
  }
};
class $modify(BufPlayerObject, PlayerObject) {
  static void onModify(auto& self) {
    (void)self.setHookPriority("PlayerObject::playSpiderDashEffect", -1000000);
    (void)self.setHookPriority("PlayerObject::incrementJumps", -1000000);
    (void)self.setHookPriority("PlayerObject::ringJump", -1000000);
  }
  void playSpiderDashEffect(cocos2d::CCPoint from, cocos2d::CCPoint to) {
    if (s_sim.isSimulating()) return;
    PlayerObject::playSpiderDashEffect(from, to);
  }
  void incrementJumps() {
    if (s_sim.isSimulating()) return;
    PlayerObject::incrementJumps();
  }
  void update(float dt) {
    if (!s_sim.isSimulating()) {
      s_sim.onPlayerUpdate(this);
    }
    PlayerObject::update(dt);
    if (PlayLayer::get() && !s_sim.isSimulating()) {
      s_sim.setFrameDelta(dt);
    }
  }
  void ringJump(RingObject* p0, bool p1) {
    if (s_sim.isSimulating()) return;
    PlayerObject::ringJump(p0, p1);
  }
};
class $modify(BufBaseGameLayer, GJBaseGameLayer) {
  static void onModify(auto& self) {
    (void)self.setHookPriority("GJBaseGameLayer::canBeActivatedByPlayer", -1000000);
    (void)self.setHookPriority("GJBaseGameLayer::handleButton", -1000000);
    (void)self.setHookPriority("GJBaseGameLayer::collisionCheckObjects", -1000000);
    (void)self.setHookPriority("GJBaseGameLayer::playerTouchedRing", -1000000);
  }
  bool canBeActivatedByPlayer(PlayerObject* p0, EffectGameObject* p1) {
    if (s_sim.isSimulating()) return false;
    return GJBaseGameLayer::canBeActivatedByPlayer(p0, p1);
  }
  void handleButton(bool down, int button, bool isPlayer1) {
    if (!s_sim.isSimulating() && !s_sim.isFiringBuffered() && button == 1 && PlayLayer::get()) {
      if (!s_sim.interceptInput(this, down, isPlayer1)) {
        return;
      }
    }
    GJBaseGameLayer::handleButton(down, button, isPlayer1);
  }
  void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* vec, int objectsCount, float dt) {
    if (s_sim.isSimulating()) {
      gd::vector<GameObject*> extra;
      extra.reserve(objectsCount);
      for (int i = 0; i < objectsCount; i++) {
        GameObject* obj = vec->at(i);
        if (!obj) continue;
        if (obj->m_objectType == GameObjectType::Solid ||
          obj->m_objectType == GameObjectType::Hazard ||
          obj->m_objectType == GameObjectType::AnimatedHazard ||
          obj->m_objectType == GameObjectType::CollisionObject ||
          obj->m_objectType == GameObjectType::Slope) {
          extra.push_back(obj);
        }
      }
      GJBaseGameLayer::collisionCheckObjects(player, &extra, extra.size(), dt);
      return;
    }
    GJBaseGameLayer::collisionCheckObjects(player, vec, objectsCount, dt);
  }
  void playerTouchedRing(PlayerObject* player, RingObject* ring) {
    if (s_sim.isSimulating()) return;
    GJBaseGameLayer::playerTouchedRing(player, ring);
  }
};
class $modify(BufPauseLayer, PauseLayer) {
  struct Fields {
    ButtonSprite* label = nullptr;
  };
  void customSetup() {
    PauseLayer::customSetup();
    auto win = CCDirector::sharedDirector()->getWinSize();
    auto spr = ButtonSprite::create(s_sim.presetLabel().c_str(), "goldFont.fnt", "GJ_button_04.png", .6f);
    m_fields->label = spr;
    auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(BufPauseLayer::onCycleBuffer));
    auto menu = CCMenu::create();
    menu->addChild(btn);
    menu->setPosition({win.width - 70.f, 30.f});
    this->addChild(menu, 10);
  }
  void onCycleBuffer(CCObject*) {
    s_sim.cyclePreset();
    if (m_fields->label) m_fields->label->setString(s_sim.presetLabel().c_str());
  }
};
