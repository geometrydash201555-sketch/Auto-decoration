// Auto Deco - one-tap decoration for the whole level (editor only).
// Adds three buttons to the editor pause menu:
//   "Auto Deco"   -> decorates the exposed edges of every solid block, in tidy rows
//   "Undo Deco"   -> removes what the last Auto Deco added
//   "Learn Style" -> uses the decoration objects you have selected as the palette
//   "AI Deco"     -> you write the style you want in the mod settings (for example "slaughterhouse"),
//                    an AI picks the decoration and the settings, and the level is decorated with them
#include <Geode/Geode.hpp>
#include <Geode/modify/EditorPauseLayer.hpp>
#include <Geode/utils/web.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>
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

// Everything Auto Deco needs to know; it comes from the mod settings or from the AI.
struct DecoParams {
    std::vector<int> palette;       // empty = copy the decoration already in the level
    int density = 70;
    int spacing = 2;
    int maxObjects = 1500;
    int colorChannel = 0;
    bool topOnly = false;
};

DecoParams settingsParams() {
    auto mod = Mod::get();
    DecoParams p;
    p.palette = builtinPalette(mod->getSettingValue<std::string>("palette"));
    if (p.palette.empty()) p.palette = parseIds(mod->getSettingValue<std::string>("deco-ids"));
    p.density = static_cast<int>(std::clamp<int64_t>(mod->getSettingValue<int64_t>("density"), 1, 100));
    p.spacing = static_cast<int>(std::clamp<int64_t>(mod->getSettingValue<int64_t>("spacing"), 1, 6));
    p.maxObjects = static_cast<int>(std::clamp<int64_t>(mod->getSettingValue<int64_t>("max-objects"), 50, 5000));
    p.colorChannel = static_cast<int>(std::clamp<int64_t>(mod->getSettingValue<int64_t>("color-channel"), 0, 999));
    p.topOnly = mod->getSettingValue<bool>("top-only");
    return p;
}

DecoResult runDeco(DecoParams p) {
    DecoResult res;
    auto lel = LevelEditorLayer::get();
    if (!lel || !lel->m_objects) {
        res.error = "No se encontro el editor.";
        return res;
    }

    // Read the level: solid cells (and decoration IDs if the palette must come from the level)
    bool fromLevel = p.palette.empty();
    std::unordered_set<int> foundIds;
    std::unordered_set<long long> solids;
    for (auto obj : CCArrayExt<GameObject*>(lel->m_objects)) {
        if (!obj) continue;
        if (obj->m_objectType == GameObjectType::Solid) {
            auto pos = obj->getPosition();
            solids.insert(cellKey(static_cast<int>(std::floor(pos.x / CELL)), static_cast<int>(std::floor(pos.y / CELL))));
        } else if (fromLevel && obj->m_objectType == GameObjectType::Decoration) {
            if (foundIds.insert(obj->m_objectID).second) p.palette.push_back(obj->m_objectID);
        }
    }
    if (solids.empty()) {
        res.error = "No hay bloques solidos en el nivel para decorar.";
        return res;
    }
    if (p.palette.empty()) {
        res.error = "La paleta esta vacia. Elige una paleta (Bushes, Grass, Dots o Mixed) en los ajustes del mod, o pulsa Learn Style con decoraciones seleccionadas.";
        return res;
    }

    // Find the free cells that touch a solid block, grouped per direction and per line.
    // dir 0 = above a block, 1 = right of it, 2 = below it, 3 = left of it.
    struct Dir {
        int dx, dy;
        float rot;
    };
    static const Dir dirs[] = {{0, 1, 0.f}, {1, 0, 90.f}, {0, -1, 180.f}, {-1, 0, 270.f}};

    // (dir, line) -> positions along that line (up/down walk along x, left/right walk along y)
    std::map<std::pair<int, int>, std::vector<int>> lines;
    std::unordered_set<long long> seen;
    for (auto const& key : solids) {
        int cx = static_cast<int>(key / 4000000LL - 1000000LL);
        int cy = static_cast<int>(key % 4000000LL - 1000000LL);
        for (int d = 0; d < 4; d++) {
            if (p.topOnly && d != 0) continue;
            int nx = cx + dirs[d].dx, ny = cy + dirs[d].dy;
            long long nk = cellKey(nx, ny);
            if (solids.count(nk)) continue;
            if (!seen.insert(nk * 4 + d).second) continue;
            bool horizontal = (d == 0 || d == 2);
            lines[{d, horizontal ? ny : nx}].push_back(horizontal ? nx : ny);
        }
    }

    // Split every line into runs of touching cells. One run = one tidy row with ONE kind of decoration.
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
    std::uniform_int_distribution<size_t> pick(0, p.palette.size() - 1);

    s_made.clear();
    for (auto const& run : runs) {
        if (static_cast<int>(s_made.size()) >= p.maxObjects) break;
        if (pct(rng) > p.density) continue;         // density = chance that a whole row gets decorated

        int id = p.palette[pick(rng)];
        bool horizontal = (run.dir == 0 || run.dir == 2);
        for (int a = run.from; a <= run.to; a += p.spacing) {
            if (static_cast<int>(s_made.size()) >= p.maxObjects) break;
            int cx = horizontal ? a : run.line;
            int cy = horizontal ? run.line : a;
            CCPoint pos((cx + .5f) * CELL, (cy + .5f) * CELL);
            auto obj = lel->createObject(id, pos, false);
            if (!obj) continue;
            obj->setRotation(dirs[run.dir].rot);
            if (p.colorChannel > 0 && obj->m_baseColor) obj->m_baseColor->m_colorID = p.colorChannel;
            s_made.push_back(obj);
        }
    }

    res.created = static_cast<int>(s_made.size());
    return res;
}

DecoResult autoDecorate() {
    return runDeco(settingsParams());
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

// ----------------------------------------------------------------------------
//  AI Deco: the style you typed in the settings is sent to an AI (your own API key). Only that text is
//  sent, never your level. The AI answers with a small JSON: decoration IDs and a few numbers.
// ----------------------------------------------------------------------------

// IDs that do not exist in GD 2.2 (from a community object list). IDs outside 1-4539 or in here are rejected.
bool isMissingId(int id) {
    static const int ranges[][2] = {
        {14, 14}, {38, 38}, {100, 100}, {102, 102}, {108, 109}, {189, 189}, {214, 214}, {262, 262}, {276, 276},
        {288, 288}, {290, 290}, {292, 293}, {298, 298}, {300, 300}, {302, 304}, {306, 306}, {308, 308}, {310, 310},
        {312, 314}, {316, 316}, {318, 320}, {322, 322}, {330, 330}, {332, 332}, {334, 336}, {338, 338}, {340, 342},
        {344, 344}, {346, 348}, {350, 350}, {352, 352}, {354, 354}, {356, 357}, {359, 362}, {379, 391}, {400, 404},
        {415, 418}, {445, 445}, {746, 746}, {748, 748}, {750, 751}, {760, 761}, {776, 806}, {834, 840}, {849, 849},
        {851, 852}, {858, 858}, {860, 860}, {864, 866}, {875, 876}, {879, 879}, {892, 892}, {897, 898}, {912, 913},
        {922, 922}, {962, 963}, {978, 979}, {993, 996}, {1008, 1008}, {1023, 1023}, {1072, 1074}, {1119, 1119},
        {1121, 1121}, {1128, 1131}, {1211, 1219}, {1321, 1321}, {1323, 1324}, {1335, 1337}, {1396, 1430},
        {1465, 1470}, {1474, 1495}, {1497, 1506}, {1508, 1509}, {1541, 1551}, {1570, 1581}, {1822, 1822},
        {1918, 1918}, {1929, 1930}, {2056, 2061}, {2705, 2707}, {2774, 2775}, {2906, 2906}, {2908, 2908},
        {2918, 2918}, {3003, 3003}, {3025, 3026}, {3028, 3028}, {3098, 3100}, {3616, 3616}, {3644, 3644},
        {3663, 3699}, {4400, 4400},
    };
    if (id < 1 || id > 4539) return true;
    for (auto const& r : ranges) {
        if (id >= r[0] && id <= r[1]) return true;
    }
    return false;
}

// Keeps only IDs that really are decoration: each one is created once, checked, and removed again.
std::vector<int> validateDecoIds(std::vector<int> const& ids) {
    std::vector<int> ok;
    auto lel = LevelEditorLayer::get();
    if (!lel) return ok;
    for (int id : ids) {
        if (ok.size() >= 8) break;
        if (isMissingId(id) || std::find(ok.begin(), ok.end(), id) != ok.end()) continue;
        auto obj = lel->createObject(id, CCPoint(-3000.f, -3000.f), false);
        if (!obj) continue;
        bool isDeco = obj->m_objectType == GameObjectType::Decoration;
        lel->removeObject(obj, false);
        if (isDeco) ok.push_back(id);
    }
    return ok;
}

bool s_aiBusy = false;

void say(std::string const& title, std::string const& text);

// Takes the AI answer text, finds the JSON in it and decorates the level with it
void applyAiAnswer(std::string const& answer) {
    auto first = answer.find('{');
    auto last = answer.rfind('}');
    if (first == std::string::npos || last == std::string::npos || last < first) {
        say("AI Deco", "La IA no devolvio una respuesta que pueda usar. Intenta otra vez o cambia la descripcion.");
        return;
    }
    auto parsed = matjson::parse(answer.substr(first, last - first + 1));
    if (parsed.isErr() || !parsed.unwrap().isObject()) {
        say("AI Deco", "La IA no devolvio una respuesta que pueda usar. Intenta otra vez o cambia la descripcion.");
        return;
    }
    matjson::Value j = parsed.unwrap();

    auto base = settingsParams();
    DecoParams p;
    p.maxObjects = base.maxObjects;
    p.colorChannel = base.colorChannel;
    p.density = base.density;
    p.spacing = base.spacing;
    p.topOnly = base.topOnly;

    std::vector<int> wanted;
    if (j.contains("ids") && j["ids"].isArray()) {
        for (size_t i = 0; i < j["ids"].size() && i < 16; i++) {
            wanted.push_back(static_cast<int>(j["ids"][i].asDouble().unwrapOr(0.0)));
        }
    }
    p.palette = validateDecoIds(wanted);
    if (p.palette.empty()) {
        // none of the AI ids was usable: fall back to the verified built-in set
        p.palette = builtinPalette("Mixed");
    }
    if (j.contains("density")) p.density = static_cast<int>(std::clamp(j["density"].asDouble().unwrapOr(70.0), 1.0, 100.0));
    if (j.contains("spacing")) p.spacing = static_cast<int>(std::clamp(j["spacing"].asDouble().unwrapOr(2.0), 1.0, 6.0));
    if (j.contains("top_only")) p.topOnly = j["top_only"].asBool().unwrapOr(false);

    std::string note = j.contains("explanation") ? j["explanation"].asString().unwrapOr("") : "";
    std::string color = j.contains("color") ? j["color"].asString().unwrapOr("") : "";

    auto res = runDeco(p);
    if (!res.error.empty()) {
        say("AI Deco", res.error);
        return;
    }
    std::string msg = fmt::format("Listo: se agregaron {} decoraciones.", res.created);
    if (!note.empty()) msg += "\n\n" + note;
    if (!color.empty()) msg += fmt::format("\n\nColor sugerido: {}. Usa el ajuste Color channel y edita ese canal en el menu de colores.", color);
    say("AI Deco", msg);
}

bool hasAny(std::string const& text, std::initializer_list<char const*> words) {
    for (auto w : words) {
        if (text.find(w) != std::string::npos) return true;
    }
    return false;
}

// Free mode without internet and without key: it does NOT think, it looks for key words in what you wrote
// (Spanish or English) and picks one of the verified decoration sets. Returns the same JSON the AI returns.
std::string localAnswer(std::string prompt) {
    std::transform(prompt.begin(), prompt.end(), prompt.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::string ids = "123,124,125,126,127,128,406,407,408,414,1764";   // Mixed
    int density = 70, spacing = 2;
    std::string note = "Use una mezcla de arbustos, pasto y puntos.";

    if (hasAny(prompt, {"slaughter", "blood", "sangre", "horror", "terror", "dark", "oscur", "hell", "infierno", "demon"})) {
        ids = "1764"; density = 55; spacing = 2;
        note = "Estilo oscuro: puntos espaciados en los bordes.";
    } else if (hasAny(prompt, {"forest", "bosque", "nature", "natur", "plant", "arbust", "bush", "jungle", "selva"})) {
        ids = "123,124,125,126,127,128,406,407,408,414"; density = 80; spacing = 2;
        note = "Estilo naturaleza: arbustos y pasto.";
    } else if (hasAny(prompt, {"grass", "pasto", "cesped", "hierba"})) {
        ids = "406,407,408,414"; density = 80; spacing = 1;
        note = "Estilo pasto.";
    } else if (hasAny(prompt, {"minimal", "simple", "limpio", "clean"})) {
        ids = "1764"; density = 40; spacing = 3;
        note = "Estilo minimalista: pocos puntos.";
    }
    if (hasAny(prompt, {"denso", "dense", "lleno", "mucho", "heavy"})) { density = 95; spacing = 1; }

    std::string color;
    if (hasAny(prompt, {"morad", "purple", "violet"})) color = "morado";
    else if (hasAny(prompt, {"rojo", "red", "slaughter", "blood", "sangre"})) color = "rojo";
    else if (hasAny(prompt, {"azul", "blue", "ocean", "mar"})) color = "azul";
    else if (hasAny(prompt, {"verde", "green", "forest", "bosque"})) color = "verde";

    std::string json = "{\"ids\":[" + ids + "],\"density\":" + std::to_string(density) +
        ",\"spacing\":" + std::to_string(spacing) + ",\"top_only\":false,\"color\":\"" + color +
        "\",\"explanation\":\"" + note + " (Modo local, no es IA)\"}";
    return json;
}

static const char* AI_SYSTEM =
    "You are a decoration helper inside the Geometry Dash level editor (version 2.2). The user describes the "
    "decoration style they want. Reply with ONLY one JSON object and no other text: "
    "{\"ids\":[...],\"density\":1-100,\"spacing\":1-6,\"top_only\":true|false,\"color\":\"short color suggestion\","
    "\"explanation\":\"one short sentence, in the same language as the user\"}. "
    "'ids' are Geometry Dash decoration object IDs (non-solid, not hazards, not triggers), at most 8, and only IDs you "
    "are sure exist in GD 2.2 (range 1-4539). Verified decoration IDs you can always use: bushes 123-128, "
    "grass 406, 407, 408, 414, dot 1764. If you are not sure about an ID, use only the verified ones. "
    "Prefer a few consistent objects over many random ones.";

std::string httpFailText(int code) {
    std::string why = (code == 401 || code == 403) ? "Tu clave de API no es valida."
        : code == 429 ? "Demasiadas peticiones, o sin cuota disponible."
        : code == 404 ? "El modelo no existe. Revisa AI model en los ajustes."
        : code == 400 ? "La peticion no fue aceptada (puede ser tu pais o el modelo)."
        : "La peticion fallo.";
    return fmt::format("{} (codigo {})", why, code);
}

void aiDecorate() {
    if (s_aiBusy) {
        say("AI Deco", "Ya hay una peticion en curso. Espera un momento.");
        return;
    }
    auto mod = Mod::get();
    std::string provider = mod->getSettingValue<std::string>("ai-provider");
    std::string key = mod->getSettingValue<std::string>("ai-api-key");
    std::string prompt = mod->getSettingValue<std::string>("ai-prompt");
    std::string model = mod->getSettingValue<std::string>("ai-model");
    if (prompt.empty()) {
        say("AI Deco", "Escribe en los ajustes del mod, en AI style, la decoracion que quieres (por ejemplo: slaughterhouse).");
        return;
    }

    // Free local mode: no internet, no key
    if (provider.rfind("Local", 0) == 0) {
        applyAiAnswer(localAnswer(prompt));
        return;
    }
    if (key.empty()) {
        say("AI Deco", "Falta tu clave de API. Pegala en los ajustes del mod, en AI API key. Si no tienes, cambia AI provider a Local (free).");
        return;
    }

    bool gemini = provider.rfind("Gemini", 0) == 0;
    web::WebRequest req;
    req.header("content-type", "application/json");
    req.timeout(std::chrono::seconds(40));
    std::string url;

    if (gemini) {
        if (model.empty()) model = "gemini-flash-latest";
        url = "https://generativelanguage.googleapis.com/v1beta/models/" + model + ":generateContent";
        req.header("x-goog-api-key", key);

        matjson::Value sysPart;
        sysPart["text"] = std::string(AI_SYSTEM);
        matjson::Value sysInst;
        sysInst["parts"] = std::vector<matjson::Value>{sysPart};
        matjson::Value userPart;
        userPart["text"] = prompt;
        matjson::Value userMsg;
        userMsg["role"] = "user";
        userMsg["parts"] = std::vector<matjson::Value>{userPart};
        matjson::Value cfg;
        cfg["maxOutputTokens"] = 600;
        matjson::Value body;
        body["systemInstruction"] = sysInst;
        body["contents"] = std::vector<matjson::Value>{userMsg};
        body["generationConfig"] = cfg;
        req.bodyJSON(body);
    } else {
        if (model.empty()) model = "claude-haiku-5-5";
        url = "https://api.anthropic.com/v1/messages";
        req.header("x-api-key", key);
        req.header("anthropic-version", "2023-06-01");

        matjson::Value msg;
        msg["role"] = "user";
        msg["content"] = prompt;
        matjson::Value body;
        body["model"] = model;
        body["max_tokens"] = 600;
        body["system"] = std::string(AI_SYSTEM);
        body["messages"] = std::vector<matjson::Value>{msg};
        req.bodyJSON(body);
    }

    s_aiBusy = true;
    async::spawn(req.post(url), [gemini](web::WebResponse resp) {
        s_aiBusy = false;
        if (!resp.ok()) {
            say("AI Deco", httpFailText(resp.code()));
            return;
        }
        auto json = resp.json();
        if (json.isErr()) {
            say("AI Deco", "No pude leer la respuesta de la IA.");
            return;
        }
        matjson::Value j = json.unwrap();
        std::string text;
        if (gemini) {
            // {"candidates":[{"content":{"parts":[{"text":"..."}]}}]}
            if (j.contains("candidates") && j["candidates"].isArray() && j["candidates"].size() > 0) {
                auto& c = j["candidates"][0];
                if (c.isObject() && c.contains("content") && c["content"].contains("parts") && c["content"]["parts"].isArray()) {
                    auto& parts = c["content"]["parts"];
                    for (size_t i = 0; i < parts.size(); i++) {
                        if (parts[i].isObject() && parts[i].contains("text")) text += parts[i]["text"].asString().unwrapOr("");
                    }
                }
            }
        } else if (j.contains("content") && j["content"].isArray()) {
            for (size_t i = 0; i < j["content"].size(); i++) {
                auto& part = j["content"][i];
                if (part.isObject() && part.contains("text")) text += part["text"].asString().unwrapOr("");
            }
        }
        if (text.empty()) {
            say("AI Deco", "La IA no devolvio texto.");
            return;
        }
        applyAiAnswer(text);
    });
    say("AI Deco", "Pidiendo la decoracion a la IA... cuando termine te aviso.");
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
        menu->setPosition({85.f, 140.f});

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

        auto aiSpr = ButtonSprite::create("AI Deco");
        aiSpr->setScale(.7f);
        auto aiBtn = CCMenuItemExt::createSpriteExtra(aiSpr, [](CCMenuItemSpriteExtra*) {
            aiDecorate();
        });
        aiBtn->setPosition({0.f, -102.f});
        menu->addChild(aiBtn);

        this->addChild(menu, 10);
        return true;
    }
};
