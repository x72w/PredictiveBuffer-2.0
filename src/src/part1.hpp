// PART 1 of 3 - preamble + first half of TrajectoryBufferSim (class continues in part2.hpp)
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

// ============================================================================
//  USER SETTINGS
// ============================================================================

// True Wave Spam Guard: only bypasses the buffer if you do at least
// SPAM_STREAK_REQUIRED consecutive inputs faster than SPAM_INTERVAL_TICKS apart.
// This keeps isolated micro-clicks and spike/sawblade dodges 100% protected!
constexpr int SPAM_INTERVAL_TICKS = 8;
constexpr int SPAM_STREAK_REQUIRED = 3;

// ============================================================================

// printf-style logger: Geode log on every platform + the live console on Windows.
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

    // Tracks actual physical mouse state vs what PlayLayer currently has active
    bool m_physicalHolding = false;
    bool m_gameHolding = false;

    // F6 Cycle State (Starts on Stage 0: 2 ticks Normal / 2 ticks Wave)
    int  m_presetIndex = 0;
    bool m_enabled = true;
    int  m_maxTicksNormal = 2;
    int  m_maxTicksWave = 2;

    // Buffer state
    int  m_waitTicks = -1;          // -1 = inactive, >= 0 = countdown ticks, -2 = queued opposite tick
    bool m_bufferedPush = true;     // true = buffered PRESS, false = buffered RELEASE
    bool m_queuedOpposite = false;  // true if user quickly released/re-pressed during wait
    float m_frameDt = 0.25f;        // GD 240 TPS normalized dt is 0.25f

    // Streak-based Spam Guard state
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

    // Matches Show Trajectory's exact attribute + hitbox size synchronization
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

    // Cycles through the 5 F6 stages:
    // 0: 2 All / 2 Wave -> 1: 4 All / 4 Wave -> 2: 8 All / 8 Wave -> 3: 16 All / 8 Wave -> 4: OFF
    void cyclePreset() {
        m_presetIndex = (m_presetIndex + 1) % 5;
        m_waitTicks = -1;
        m_queuedOpposite = false;

        switch (m_presetIndex) {
            case 0:
                m_enabled = true;
                m_maxTicksNormal = 2;
                m_maxTicksWave = 2;
                bufLog("[InputBuffer] F6 Cycle -> STAGE 1: 2 Ticks All | 2 Ticks Wave (0.5f @ 60fps)\n");
                break;
            case 1:
                m_enabled = true;
                m_maxTicksNormal = 4;
                m_maxTicksWave = 4;
                bufLog("[InputBuffer] F6 Cycle -> STAGE 2: 4 Ticks All | 4 Ticks Wave (1.0f @ 60fps)\n");
                break;
            case 2:
                m_enabled = true;
                m_maxTicksNormal = 8;
                m_maxTicksWave = 8;
                bufLog("[InputBuffer] F6 Cycle -> STAGE 3: 8 Ticks All | 8 Ticks Wave (2.0f @ 60fps)\n");
                break;
            case 3:
                m_enabled = true;
                m_maxTicksNormal = 16;
                m_maxTicksWave = 8;
                bufLog("[InputBuffer] F6 Cycle -> STAGE 4: 16 Ticks All | 8 Ticks Wave (4.0f / 2.0f @ 60fps)\n");
                break;
            case 4:
                m_enabled = false;
                m_maxTicksNormal = 0;
                m_maxTicksWave = 0;
                bufLog("[InputBuffer] F6 Cycle -> STAGE 5: OFF (100%% Raw Input)\n");
                break;
        }
    }

    std::string presetLabel() const {
        switch (m_presetIndex) {
            case 0: return "Buffer: S1";
            case 1: return "Buffer: S2";
            case 2: return "Buffer: S3";
            case 3: return "Buffer: S4";
            default: return "Buffer: OFF";
        }
    }

    bool isSimulating() const { return m_simulating; }
    bool isFiringBuffered() const { return m_firingBuffered; }

    bool checkSimulationDeath(PlayerObject* player) {
        if (m_simulating && (player == m_simPlayer1 || player == m_simPlayer2)) {
            m_simulationDead = true;
            return true;
        }
        // Real player died -> clear any pending buffer so respawn is 100% clean
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

