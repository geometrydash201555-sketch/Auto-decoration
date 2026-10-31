// Auto Deco - one-tap decoration for the whole level (editor only).
// Adds three buttons to the editor pause menu:
//   "Auto Deco"   -> decorates the exposed edges of every solid block, in tidy rows
//   "Undo Deco"   -> removes what the last Auto Deco added
//   "Learn Style" -> uses the decoration objects you have selected as the palette
#include <Geode/Geode.hpp>
#include <Geode/modify/EditorPauseLayer.hpp>

#include <algorithm>
#include <cmath>
#include <map>
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

// Built-in palettes. IDs come from a community object ID list (Library of Geometria);
// they have not been tested in game here.
std::vector<int> builtinPalette(std::string const& name) {
    if (name == "Bushes") return {123, 124, 125, 126, 127, 128};
    if (name == "Grass") return {406, 407, 408, 414};
    if (name == "Dots") return {1764};
    if (name == "Mixed") return {123, 124, 125, 126, 127, 128, 406, 407, 408, 414, 1764};
    return {};
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
    auto mod = Mod::get();

    // 1) Palette: built-in set, or your own / learned IDs (or the decoration already in the level)
    std::string paletteName = mod->getSettingValue<std::string>("palette");
    std::vector<int> palette = builtinPalette(paletteName);
    bool custom = palette.empty();
    if (custom) palette = parseIds(mod->getSettingValue<std::string>("deco-ids"));
    bool fromLevel = custom && palette.empty();

    // 2) Read the level: solid cells (and decoration IDs if the palette comes from the level)
    std::unordered_set<int> foundIds;
    std::unordered_set<long long> solids;
    for (auto obj : CCArrayExt<GameObject*>(lel->m_objects)) {
        if (!obj) continue;
        if (obj->m_objectType == GameObjectType::Solid) {
            auto p = obj->getPosition();
            solids.insert(cellKey(static_cast<int>(std::floor(p.x / CELL)), static_cast<int>(std::floor(p.y / CELL))));
        } else if (fromLevel && obj->m_objectType == GameObjectType::Decoration) {
            if (foundIds.insert(obj->m_objectID).second) palette.push_back(obj->m_objectID);
        }
    }
    if (solids.empty()) {
        res.error = "No hay bloques solidos en el nivel para decorar.";
        return res;
    }
    if (palette.empty()) {
        res.error = "La paleta esta vacia. Elige una paleta (Bushes, Grass, Dots o Mixed) en los ajustes del mod, o pulsa Learn Style con decoraciones seleccionadas.";
        return res;
    }

    int density = static_cast<int>(std::clamp<int64_t>(mod->getSettingValue<int64_t>("density"), 1, 100));
    int spacing = static_cast<int>(std::clamp<int64_t>(mod->getSettingValue<int64_t>("spacing"), 1, 6));
    int maxObjects = static_cast<int>(std::clamp<int64_t>(mod->getSettingValue<int64_t>("max-objects"), 50, 5000));
    int colorChannel = static_cast<int>(std::clamp<int64_t>(mod->getSettingValue<int64_t>("color-channel"), 0, 999));
    bool topOnly = mod->getSettingValue<bool>("top-only");

    // 3) Find the free cells that touch a solid block, grouped per direction and per line.
    //    dir 0 = above a block, 1 = right of it, 2 = below it, 3 = left of it.
    struct Dir {
        int dx, dy;
        float rot;
    };
    static const Dir dirs[] = {{0, 1, 0.f}, {1, 0, 90.f}, {0, -1, 180.f}, {-1, 0, 270.f}};

    // (dir, line) -> positions along that line; for up/down the line is y and we walk along x, for left/right the opposite
    std::map<std::pair<int, int>, std::vector<int>> lines;
    std::unordered_set<long long> seen;
    for (auto const& key : solids) {
        // decode the cell from the key
        long long kx = key / 4000000LL - 1000000LL;
        long long ky = key % 4000000LL - 1000000LL;
        int cx = static_cast<int>(kx), cy = static_cast<int>(ky);
        for (int d = 0; d < 4; d++) {
            if (topOnly && d != 0) continue;
            int nx = cx + dirs[d].dx, ny = cy + dirs[d].dy;
            long long nk = cellKey(nx, ny);
            if (solids.count(nk)) continue;
            if (!seen.insert(nk * 4 + d).second) continue;
            bool horizontal = (d == 0 || d == 2);
            lines[{d, horizontal ? ny : nx}].push_back(horizontal ? nx : ny);
        }
    }

    // 4) Split every line into runs of touching cells. One run = one tidy row with ONE kind of decoration.
    struct Run {
        int dir, line, from, to;
    };
    std::vector<Run> runs;
    for (auto& [k, v] : lines) {
        std::sort(v.begin(), v.end());
        size_t start = 0;
        for (size_t i = 1; i <= v.size(); i++) {
            if (i == v.size() || v[i] != v[i - 1] + 1) {
                runs.push_back({k.first, k.second, v[start], v[i - 1]});
                start = i;
            }
        }
    }

    std::mt19937 rng(std::random_device{}());
    std::shuffle(runs.begin(), runs.end(), rng);    // so the cap spreads over the whole level
    std::uniform_int_distribution<int> pct(1, 100);
    std::uniform_int_distribution<size_t> pick(0, palette.size() - 1);

    s_made.clear();
    for (auto const& run : runs) {
        if (static_cast<int>(s_made.size()) >= maxObjects) break;
        if (pct(rng) > density) continue;           // density = chance that a whole row gets decorated

        int id = palette[pick(rng)];
        bool horizontal = (run.dir == 0 || run.dir == 2);
        // a fixed offset per row so neighbouring rows do not all start at the same spot
        for (int a = run.from; a <= run.to; a += spacing) {
            if (static_cast<int>(s_made.size()) >= maxObjects) break;
            int cx = horizontal ? a : run.line;
            int cy = horizontal ? run.line : a;
            CCPoint pos((cx + .5f) * CELL, (cy + .5f) * CELL);
            auto obj = lel->createObject(id, pos, false);
            if (!obj) continue;
            obj->setRotation(dirs[run.dir].rot);
            if (colorChannel > 0 && obj->m_baseColor) obj->m_baseColor->m_colorID = colorChannel;
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

// Reads the objects currently selected in the editor and saves their IDs as your own palette.
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
    Mod::get()->setSettingValue<std::string>("palette", "My IDs");
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
