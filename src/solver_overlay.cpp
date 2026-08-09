#include "solver_overlay.hpp"

#include <Geode/Geode.hpp>
#include <algorithm>
#include <vector>
#include <string>

using namespace geode::prelude;
using namespace cocos2d;

namespace {

constexpr float kPanelW = 440.f;
constexpr float kPanelH = 290.f;
constexpr int   kLogLines = 9;

// Panel-local layout (origin bottom-left of the panel container).
constexpr float kMargin   = 22.f;
constexpr float kBarY     = 232.f;
constexpr float kBarH     = 24.f;
constexpr float kStatsY   = 206.f;
constexpr float kLogTop   = 182.f;
constexpr float kLogLineH = 16.f;

ccColor3B colorForLog(const std::string& m, bool isLast, bool done) {
    if (!m.empty() && m[0] == '+')                            return {82, 214, 129};
    if (m.find("SOLVED") != std::string::npos ||
        m.find("Solution") != std::string::npos ||
        m.find("Path found") != std::string::npos)           return {82, 214, 129};
    if (m.find("Backtrack") != std::string::npos ||
        m.find("Restart")   != std::string::npos)            return {255, 165, 87};
    if (m.find("Stuck")   != std::string::npos ||
        m.find("Timeout") != std::string::npos ||
        m.find("failed")  != std::string::npos ||
        m.find("Dead")    != std::string::npos)               return {255, 102, 102};
    if (m.find("Cancel") != std::string::npos)               return {170, 170, 170};
    if (isLast && !done)                                     return {120, 175, 255};
    return {170, 185, 210};
}

class SolverOverlay : public CCLayer {
    std::shared_ptr<gdsim::SolverProgressReport> m_progress;
    std::shared_ptr<std::atomic<bool>>           m_cancelled;
    int   m_levelId   = 0;
    bool  m_finished  = false;

    CCNode*         m_panel      = nullptr;
    CCLabelBMFont*  m_title      = nullptr;
    CCLayerColor*   m_barFill    = nullptr;
    CCLabelBMFont*  m_pctLabel   = nullptr;
    CCLabelBMFont*  m_statsLabel = nullptr;
    CCLabelBMFont*  m_logLabels[kLogLines] = {};
    ButtonSprite*   m_actionSpr  = nullptr;

public:
    static SolverOverlay* create(int levelId,
                                 std::shared_ptr<gdsim::SolverProgressReport> progress,
                                 std::shared_ptr<std::atomic<bool>> cancelled) {
        auto ret = new SolverOverlay();
        if (ret && ret->initOverlay(levelId, std::move(progress), std::move(cancelled))) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

private:
    bool initOverlay(int levelId,
                     std::shared_ptr<gdsim::SolverProgressReport> progress,
                     std::shared_ptr<std::atomic<bool>> cancelled) {
        if (!CCLayer::init()) return false;
        m_progress  = std::move(progress);
        m_cancelled = std::move(cancelled);
        m_levelId   = levelId;

        const auto win = CCDirector::sharedDirector()->getWinSize();

        // Dim background.
        auto dim = CCLayerColor::create({0, 0, 0, 165});
        dim->setContentSize(win);
        this->addChild(dim);

        // Centered panel container.
        m_panel = CCNode::create();
        m_panel->setContentSize({kPanelW, kPanelH});
        m_panel->setAnchorPoint({0.5f, 0.5f});
        m_panel->setPosition({win.width / 2.f, win.height / 2.f});
        this->addChild(m_panel);

        auto bg = CCScale9Sprite::create("GJ_square02.png");
        bg->setContentSize({kPanelW, kPanelH});
        bg->setPosition({kPanelW / 2.f, kPanelH / 2.f});
        m_panel->addChild(bg);

        // Everything below lives in a single vertical ColumnLayout so the spacing
        // is computed automatically (no magic Y constants) and stays even. The
        // content box is the panel minus a uniform inner margin.
        const float barW = kPanelW - 2.f * kMargin;
        auto content = CCNode::create();
        content->setContentSize({barW, kPanelH - 2.f * kMargin});
        content->setAnchorPoint({0.5f, 0.5f});
        content->setPosition({kPanelW / 2.f, kPanelH / 2.f});
        m_panel->addChild(content);

        // Title.
        m_title = CCLabelBMFont::create("Pathfinding", "goldFont.fnt");
        m_title->setScale(0.7f);
        content->addChild(m_title);

        // Progress bar (bg + fill + centred % label) wrapped in a fixed-size box
        // so the column can place it as one unit.
        auto barBox = CCNode::create();
        barBox->setContentSize({barW, kBarH});
        auto barBg = CCLayerColor::create({28, 32, 44, 255}, barW, kBarH);
        barBg->setPosition({0.f, 0.f});
        barBox->addChild(barBg);
        m_barFill = CCLayerColor::create({99, 162, 255, 255}, 1.f, kBarH);
        m_barFill->setPosition({0.f, 0.f});
        barBox->addChild(m_barFill);
        m_pctLabel = CCLabelBMFont::create("0%", "bigFont.fnt");
        m_pctLabel->setPosition({barW / 2.f, kBarH / 2.f});
        m_pctLabel->setScale(0.42f);
        barBox->addChild(m_pctLabel, 2);
        content->addChild(barBox);

        // Stats line.
        m_statsLabel = CCLabelBMFont::create("Clicks: 0   Passes: 0", "chatFont.fnt");
        m_statsLabel->setScale(0.78f);
        m_statsLabel->setColor({180, 195, 220});
        content->addChild(m_statsLabel);

        // Log block: a fixed-size box holding left-aligned lines (positioned
        // top-down inside it). The box is one column item; the lines inside stay
        // absolutely placed since they update every frame and can be empty.
        auto logBox = CCNode::create();
        logBox->setContentSize({barW, kLogLines * kLogLineH});
        for (int i = 0; i < kLogLines; ++i) {
            auto lbl = CCLabelBMFont::create("", "chatFont.fnt");
            lbl->setAnchorPoint({0.f, 1.f});
            lbl->setPosition({0.f, kLogLines * kLogLineH - i * kLogLineH});
            lbl->setScale(0.62f);
            logBox->addChild(lbl);
            m_logLabels[i] = lbl;
        }
        content->addChild(logBox);

        // Action button (Cancel → Close), wrapped in a sized menu so the column
        // places it correctly (a bare CCMenu is win-sized by default).
        m_actionSpr = ButtonSprite::create("Cancel", "bigFont.fnt", "GJ_button_06.png", 0.7f);
        const auto btnSize = m_actionSpr->getContentSize();
        auto menu = CCMenu::create();
        menu->setContentSize(btnSize);
        auto btn = CCMenuItemSpriteExtra::create(
            m_actionSpr, this, menu_selector(SolverOverlay::onAction));
        btn->setPosition({btnSize.width / 2.f, btnSize.height / 2.f});
        menu->addChild(btn);
        content->addChild(menu);

        // Stack top-to-bottom, spread across the full content height, centred.
        content->setLayout(
            ColumnLayout::create()
                ->setGap(10.f)
                ->setAxisAlignment(AxisAlignment::Between)
                ->setCrossAxisAlignment(AxisAlignment::Center)
                ->setAutoScale(false));

        this->setKeypadEnabled(true);
        this->scheduleUpdate();
        return true;
    }

    void update(float) override { refresh(); }

    void refresh() {
        auto* prog = m_progress.get();
        if (!prog) return;

        const bool  done   = prog->done.load();
        const float bestX  = prog->bestX.load();
        const float levelE = prog->levelEndX.load();
        const int   clicks = prog->clicksFound.load();
        const int   iters  = prog->iteration.load();

        const float pct = (levelE > 0.f) ? std::min(1.f, std::max(0.f, bestX / levelE)) : 0.f;
        const float barW = kPanelW - 2.f * kMargin;

        m_barFill->setContentSize({std::max(1.f, barW * pct), kBarH});
        const bool complete = done && pct >= 0.999f;
        if (complete)        m_barFill->setColor({82, 214, 129});
        else if (done)       m_barFill->setColor({255, 165, 87});
        else                 m_barFill->setColor({99, 162, 255});

        m_pctLabel->setString(
            fmt::format("{}%  (X {:.0f} / {:.0f})", (int)(pct * 100), bestX, levelE).c_str());
        m_statsLabel->setString(
            fmt::format("Clicks: {}    Forward passes: {}", clicks, iters).c_str());
        m_title->setString(
            fmt::format("Pathfinding - L{}  |  {}", m_levelId, done ? "Done" : "Running...").c_str());

        // Log.
        std::vector<std::string> logCopy;
        {
            std::lock_guard<std::mutex> lk(prog->logMtx);
            logCopy.assign(prog->log.begin(), prog->log.end());
        }
        int start = (int)logCopy.size() - kLogLines;
        if (start < 0) start = 0;
        for (int i = 0; i < kLogLines; ++i) {
            int idx = start + i;
            if (idx < (int)logCopy.size()) {
                bool isLast = (idx == (int)logCopy.size() - 1);
                m_logLabels[i]->setString(logCopy[idx].c_str());
                m_logLabels[i]->setColor(colorForLog(logCopy[idx], isLast, done));
            } else {
                m_logLabels[i]->setString("");
            }
        }

        if (done && !m_finished) {
            m_finished = true;
            m_actionSpr->setString("Close");
            m_actionSpr->updateBGImage(complete ? "GJ_button_01.png" : "GJ_button_04.png");
        }
    }

    void onAction(CCObject*) {
        if (!m_finished) {
            if (m_cancelled) m_cancelled->store(true);
            m_actionSpr->setString("Cancelling...");
            return;
        }
        this->removeFromParentAndCleanup(true);
    }

    void keyBackClicked() override {
        if (m_finished) this->removeFromParentAndCleanup(true);
        else onAction(nullptr);
    }
};

} // namespace

void showSolverOverlay(int levelId,
                       std::shared_ptr<gdsim::SolverProgressReport> progress,
                       std::shared_ptr<std::atomic<bool>> cancelled) {
    auto scene = CCDirector::sharedDirector()->getRunningScene();
    if (!scene) return;
    auto overlay = SolverOverlay::create(levelId, std::move(progress), std::move(cancelled));
    if (overlay) scene->addChild(overlay, 99999);
}
