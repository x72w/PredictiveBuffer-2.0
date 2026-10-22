// Predictive Input Buffer (Windows + Android64). Options are in the mod settings.
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <cstdio>
#include <cstdarg>
#include <algorithm>
#include <chrono>
using namespace geode::prelude;
constexpr int SPAM_INTERVAL_TICKS = 8;
constexpr int SPAM_HORIZON_TICKS = 8;
static void bufLog(const char* fmt, ...) {
 char buf[512];
 va_list args;
 va_start(args, fmt);
 vsnprintf(buf, sizeof(buf), fmt, args);
 va_end(args);
 std::string line = buf;
 while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
 log::info("{}", line);
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
 int  m_stageIdx = -1;
 bool m_enabled = false;
 int  m_maxTicksNormal = 0;
 int  m_maxTicksWave = 0;
 bool m_modeOn[8] = {};
 bool m_logSaves = false;
 bool m_spamOn = true;
 bool m_counter = false;
 int  m_spamStreakReq = 3;
 int  m_spamDelay = 3;
 int  m_saves = 0;
 int  m_coolTicks = 0;
 int  m_horizonPct = 100;
 int  m_budgetUs = 3000;
 std::chrono::steady_clock::time_point m_deadline;
 bool m_overBudget = false;
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
  m_stageIdx = -1;
  applySettings();
  m_waitTicks = -1;
  m_queuedOpposite = false;
  m_physicalHolding = false;
  m_gameHolding = false;
  m_simulating = false;
  m_firingBuffered = false;
  m_ticksSinceLastInput = 999;
  m_rapidInputStreak = 0;
  resetSaves();
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
 static int modeIdx(PlayerObject* p) {
  if (p->m_isShip) return 1;
  if (p->m_isDart) return 2;
  if (p->m_isBird) return 3;
  if (p->m_isBall) return 4;
  if (p->m_isRobot) return 5;
  if (p->m_isSpider) return 6;
  if (p->m_isSwing) return 7;
  return 0;
 }
 bool blocked(PlayLayer* pl) const {
  if (!m_modeOn[modeIdx(pl->m_player1)]) return true;
  return pl->m_gameState.m_isDualMode && pl->m_player2 && !m_modeOn[modeIdx(pl->m_player2)];
 }
 void applySettings() {
  static const int kAll[5] = {0, 2, 4, 8, 16};
  static const char* kKeys[8] = {"mode-cube", "mode-ship", "mode-wave", "mode-ufo", "mode-ball", "mode-robot", "mode-spider", "mode-swing"};
  auto* m = Mod::get();
  for (int i = 0; i < 8; i++) m_modeOn[i] = m->getSettingValue<bool>(kKeys[i]);
  m_logSaves = m->getSettingValue<bool>("log-saves");
  m_spamOn = m->getSettingValue<bool>("buffer-spam");
  m_counter = m->getSettingValue<bool>("show-counter");
  m_spamStreakReq = (int)m->getSettingValue<int64_t>("spam-streak");
  m_spamDelay = (int)m->getSettingValue<int64_t>("spam-delay");
  m_horizonPct = (int)m->getSettingValue<int64_t>("horizon-pct");
  m_budgetUs = (int)m->getSettingValue<int64_t>("max-ms") * 1000;
  std::string v = m->getSettingValue<std::string>("stage");
  int idx = 0;
  if (v.rfind("Stage 1", 0) == 0) idx = 1;
  else if (v.rfind("Stage 2", 0) == 0) idx = 2;
  else if (v.rfind("Stage 3", 0) == 0) idx = 3;
  else if (v.rfind("Stage 4", 0) == 0) idx = 4;
  int waveCap = (int)m->getSettingValue<int64_t>("wave-cap");
  int wl = std::min(kAll[idx], waveCap);
  if (idx == m_stageIdx && wl == m_maxTicksWave) return;
  m_stageIdx = idx;
  m_waitTicks = -1;
  m_queuedOpposite = false;
  m_enabled = idx > 0;
  m_maxTicksNormal = kAll[idx];
  m_maxTicksWave = wl;
  bufLog("[InputBuffer] Stage -> %s (%d ticks All | %d ticks Wave/Ship)\n", v.c_str(), m_maxTicksNormal, m_maxTicksWave);
 }
 void bumpCounter(PlayLayer* pl) {
  m_saves++;
  if (!m_counter || !pl->m_uiLayer) return;
  auto* n = pl->m_uiLayer->getChildByTag(7733);
  auto* lbl = n ? static_cast<CCLabelBMFont*>(n) : nullptr;
  if (!lbl) {
   lbl = CCLabelBMFont::create("", "bigFont.fnt");
   lbl->setTag(7733);
   lbl->setScale(0.4f);
   lbl->setOpacity(150);
   lbl->setAnchorPoint({0.f, 1.f});
   auto sz = CCDirector::get()->getWinSize();
   lbl->setPosition({6.f, sz.height - 6.f});
   pl->m_uiLayer->addChild(lbl, 100);
  }
  lbl->setString(fmt::format("Saves: {}", m_saves).c_str());
 }
 void resetSaves() { m_saves = 0; m_coolTicks = 0; }
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
  bool isContinuousMode = realP1->m_isDart || realP1->m_isBird || realP1->m_isSwing || realP1->m_isShip ||
        (isDual && realP2 && (realP2->m_isDart || realP2->m_isBird || realP2->m_isSwing || realP2->m_isShip));
  int airborneTicks = 0;
  int safeLandedTicks = 0;
  for (int i = 0; i < delayFrames + maxTicks; i++) {
   if ((i & 3) == 0 && std::chrono::steady_clock::now() > m_deadline) {
    m_overBudget = true;
    break;
   }
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
  applySettings();
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
  if (blocked(pl) || m_coolTicks > 0) {
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
  bool isWave = p1->m_isDart || p1->m_isShip || (isDual && p2 && (p2->m_isDart || p2->m_isShip));
  bool isMiniWave = (p1->m_isDart && p1->m_vehicleSize < 1.0f) ||
      (isDual && p2 && p2->m_isDart && p2->m_vehicleSize < 1.0f);
  if (m_ticksSinceLastInput < SPAM_INTERVAL_TICKS) {
   m_rapidInputStreak++;
  } else {
   m_rapidInputStreak = 0;
  }
  m_ticksSinceLastInput = 0;
  bool spam = isWave && m_rapidInputStreak >= m_spamStreakReq;
  if (spam && !m_spamOn) {
   m_gameHolding = push;
   return true;
  }
  bool releaseAffectsPhysics = isWave || p1->m_isRobot || (isDual && p2 && p2->m_isRobot);
  if (!push && !releaseAffectsPhysics) {
   m_gameHolding = false;
   return true;
  }
  m_queuedOpposite = false;
  const char* act = push ? "PRESS" : "RELEASE";
  int targetHorizon = std::max(8, (isMiniWave ? 50 : (isWave ? 65 : 96)) * m_horizonPct / 100);
  int maxDelayFrames = isWave ? m_maxTicksWave : m_maxTicksNormal;
  if (spam) {
   targetHorizon = SPAM_HORIZON_TICKS;
   maxDelayFrames = std::min(maxDelayFrames, m_spamDelay);
  }
  m_deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(m_budgetUs);
  m_overBudget = false;
  int ticksFrame0 = getSurvivalTicks(pl, p1, p2, isDual, 0, push, targetHorizon);
  if (m_overBudget || ticksFrame0 >= targetHorizon) {
   if (m_overBudget) m_coolTicks = 6;
   m_gameHolding = push;
   return true;
  }
  int bestDelay = 0;
  int bestTicks = ticksFrame0;
  for (int delay = 1; delay <= maxDelayFrames; delay++) {
   int ticks = getSurvivalTicks(pl, p1, p2, isDual, delay, push, targetHorizon);
   if (m_overBudget) {
    bestDelay = 0;
    m_coolTicks = 6;
    break;
   }
   if (spam) {
    if (ticks >= targetHorizon) {
     bestDelay = delay;
     bestTicks = ticks;
     break;
    }
   } else if (isWave) {
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
   bumpCounter(pl);
   if (m_logSaves) bufLog("[InputBuffer] *** SAVED +%d TICKS (%.2f frames @ 60fps) %s! (Frame0: %d -> Delayed: %d) [Limit: %d] ***\n",
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
  if (!m_enabled) return;
  if (m_ticksSinceLastInput < 999) {
   m_ticksSinceLastInput++;
  }
  if (m_coolTicks > 0) m_coolTicks--;
  if (blocked(pl)) {
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
 bufLog("[InputBuffer] v1.2 loaded - pick stage and modes in the mod settings\n");
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
   static gd::vector<GameObject*> extra;
   extra.clear();
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
