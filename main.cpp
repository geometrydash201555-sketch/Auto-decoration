// Auto Deco - one-tap decoration for the whole level (editor only).
// Adds two buttons to the editor pause menu:
//   "Auto Deco"  -> places decoration objects along the exposed edges of every solid block
//   "Undo Deco"  -> removes what the last Auto Deco added
#include <Geode/Geode.hpp>
#include <Geode/modify/EditorPauseLayer.hpp>

#include <algorithm>
#include <cmath>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

using namespace geode::prelude;

namespace {

constexpr float CELL = 30.f;    // one block in the editor

// Objects created by the last Auto Deco (kept so it can be undone)
std::vector<Ref<GameObject>> s_made;

long long cellKey(int x, int y) {
    return (static_cast<long long>(x) + 1000000LL) * 4000000LL + (static_cast<long long>(y) + 1000000LL);
}

// "1888, 1889;2000" -> {1888, 1889, 2000}
std::vector<int> parseIds(std::string const& text) {
    std::vector<int> out;
    std::string cur;
    auto flush = [&] {
        if (cur.empty()) return;
        try {
            int v = std::stoi(cur);
            if (v > 0 && v < 10000) out.push_back(v);
        } catch (...) {}
        cur.clear();
    };
    for (char c : text) {
        if (c >= '0' && c <= '9') cur += c;
        else flush();
    }
    flush();
    return out;
}

struct DecoResult {
    int created = 0;
    std::string error;
};

DecoResult autoDecorate() {
    DecoResult res;
    auto lel = LevelEditorLayer::get();
    if (!lel || !lel->m_objects) {
        res.error = "No se encontro el editor.";
        return res;
    }

    // 1) Read the level: solid cells, and (if the player gave no IDs) decoration IDs already used
    std::vector<int> palette = parseIds(Mod::get()->getSettingValue<std::string>("deco-ids"));
    bool ownPalette = !palette.empty();
    std::unordered_set<int> foundIds;
    std::unordered_set<long long> solids;
    std::vector<std::pair<int, int>> solidList;

    for (auto obj : CCArrayExt<GameObject*>(lel->m_objects)) {
        if (!obj) continue;
        if (obj->m_objectType == GameObjectType::Solid) {
            auto p = obj->getPosition();
            int cx = static_cast<int>(std::floor(p.x / CELL));
            int cy = static_cast<int>(std::floor(p.y / CELL));
            if (solids.insert(cellKey(cx, cy)).second) solidList.push_back({cx, cy});
        } else if (!ownPalette && obj->m_objectType == GameObjectType::Decoration) {
            if (foundIds.insert(obj->m_objectID).second) palette.push_back(obj->m_objectID);
        }
    }

    if (solidList.empty()) {
        res.error = "No hay bloques solidos en el nivel para decorar.";
        return res;
    }
    if (palette.empty()) {
        res.error = "No hay decoraciones para usar. Escribe IDs de objetos en los ajustes del mod (Decoration object IDs) o pon algunas decoraciones en tu nivel y vuelve a intentar.";
        return res;
    }

    int density = static_cast<int>(std::clamp<int64_t>(Mod::get()->getSettingValue<int64_t>("density"), 1, 100));
    int maxObjects = static_cast<int>(std::clamp<int64_t>(Mod::get()->getSettingValue<int64_t>("max-objects"), 50, 5000));
    bool topOnly = Mod::get()->getSettingValue<bool>("top-only");

    // 2) Place decorations in the free cells that touch a solid block, facing outward
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> pct(1, 100);
    std::uniform_int_distribution<size_t> pick(0, palette.size() - 1);
    std::shuffle(solidList.begin(), solidList.end(), rng);   // so the cap spreads over the whole level

    struct Dir {
        int dx, dy;
        float rot;
    };
    static const Dir dirs[] = {{0, 1, 0.f}, {1, 0, 90.f}, {0, -1, 180.f}, {-1, 0, 270.f}};

    std::unordered_set<long long> used;
    s_made.clear();

    for (auto const& cell : solidList) {
        if (static_cast<int>(s_made.size()) >= maxObjects) break;
        for (auto const& d : dirs) {
            if (topOnly && !(d.dx == 0 && d.dy == 1)) continue;
            if (static_cast<int>(s_made.size()) >= maxObjects) break;

            int nx = cell.first + d.dx;
            int ny = cell.second + d.dy;
            long long k = cellKey(nx, ny);
            if (solids.count(k) || used.count(k)) continue;     // occupied by a block or already decorated
            if (pct(rng) > density) continue;
            used.insert(k);

            CCPoint pos((nx + .5f) * CELL, (ny + .5f) * CELL);
            auto obj = lel->createObject(palette[pick(rng)], pos, false);
            if (!obj) continue;
            obj->setRotation(d.rot);
            s_made.push_back(obj);
        }
    }

    res.created = static_cast<int>(s_made.size());
    return res;
}

int undoDecorate() {
    auto lel = LevelEditorLayer::get();
    if (!lel || !lel->m_objects) return 0;
    int removed = 0;
    for (auto& obj : s_made) {
        // skip objects the player already deleted by hand
        if (obj && lel->m_objects->containsObject(obj)) {
            lel->removeObject(obj, false);
            removed++;
        }
    }
    s_made.clear();
    return removed;
}

// Reads the objects currently selected in the editor and saves their IDs as the decoration palette,
// so Auto Deco decorates with that style. Returns how many different IDs were learned (0 = none).
int learnStyle() {
    auto lel = LevelEditorLayer::get();
    if (!lel || !lel->m_editorUI) return 0;
    auto selected = lel->m_editorUI->getSelectedObjects();
    if (!selected) return 0;

    std::vector<int> ids;
    std::unordered_set<int> seen;
    for (auto obj : CCArrayExt<GameObject*>(selected)) {
        if (!obj || obj->m_objectType == GameObjectType::Solid) continue;   // only decoration, not blocks
        if (seen.insert(obj->m_objectID).second) ids.push_back(obj->m_objectID);
    }
    if (ids.empty()) return 0;

    std::string text;
    for (size_t i = 0; i < ids.size(); i++) {
        if (i) text += ", ";
        text += std::to_string(ids[i]);
    }
    Mod::get()->setSettingValue<std::string>("deco-ids", text);
    return static_cast<int>(ids.size());
}

void say(std::string const& title, std::string const& text) {
    FLAlertLayer::create(nullptr, title.c_str(), text, "OK", nullptr)->show();
}

} // namespace

class $modify(AutoDecoPauseLayer, EditorPauseLayer) {
    bool init(LevelEditorLayer* editor) {
        if (!EditorPauseLayer::init(editor)) return false;

        auto menu = CCMenu::create();
        menu->setID("auto-deco-menu"_spr);
        menu->setPosition({85.f, 95.f});

        auto decoSpr = ButtonSprite::create("Auto Deco");
        decoSpr->setScale(.7f);
        auto decoBtn = CCMenuItemExt::createSpriteExtra(decoSpr, [](CCMenuItemSpriteExtra*) {
            createQuickPopup(
                "Auto Deco",
                "Se decorara <cy>todo el nivel</c> con un toque. Si no te gusta, usa <cy>Undo Deco</c> para quitar lo que se agrego.",
                "Cancelar", "Decorar",
                [](FLAlertLayer*, bool decorate) {
                    if (!decorate) return;
                    auto res = autoDecorate();
                    if (!res.error.empty()) {
                        say("Auto Deco", res.error);
                    } else {
                        say("Auto Deco", fmt::format("Listo: se agregaron {} decoraciones.", res.created));
                    }
                });
        });
        decoBtn->setPosition({0.f, 0.f});
        menu->addChild(decoBtn);

        auto undoSpr = ButtonSprite::create("Undo Deco");
        undoSpr->setScale(.7f);
        auto undoBtn = CCMenuItemExt::createSpriteExtra(undoSpr, [](CCMenuItemSpriteExtra*) {
            int n = undoDecorate();
            say("Auto Deco", n > 0 ? fmt::format("Se quitaron {} decoraciones.", n)
                                   : std::string("No hay nada que deshacer."));
        });
        undoBtn->setPosition({0.f, -34.f});
        menu->addChild(undoBtn);

        auto learnSpr = ButtonSprite::create("Learn Style");
        learnSpr->setScale(.7f);
        auto learnBtn = CCMenuItemExt::createSpriteExtra(learnSpr, [](CCMenuItemSpriteExtra*) {
            int n = learnStyle();
            if (n > 0) {
                say("Auto Deco", fmt::format("Aprendi {} decoraciones distintas de tu seleccion. Ahora Auto Deco decorara con ese estilo.", n));
            } else {
                say("Auto Deco", "Cierra este menu, selecciona en el editor algunas decoraciones (no bloques) con el estilo que quieres, y vuelve a pulsar Learn Style.");
            }
        });
        learnBtn->setPosition({0.f, -68.f});
        menu->addChild(learnBtn);

        this->addChild(menu, 10);
        return true;
    }
};
