#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <iterator>
#include <regex>
#include <string>
#include <vector>

namespace quaddeck {

namespace shader_compat_detail {

// Keep positions stable while making comments and quoted text invisible to
// matching. This is a lexical filter, not an HLSL/preprocessor parser.
inline std::string codeMask(const std::string& source) {
    std::string code = source;
    for (std::size_t i = 0; i < source.size();) {
        const std::size_t begin = i;
        if (source.compare(i, 2, "//") == 0) {
            i = source.find('\n', i + 2);
            if (i == std::string::npos) i = source.size();
        } else if (source.compare(i, 2, "/*") == 0) {
            i = source.find("*/", i + 2);
            i = i == std::string::npos ? source.size() : i + 2;
        } else if (source[i] == '"' || source[i] == '\'') {
            const char quote = source[i++];
            while (i < source.size()) {
                if (source[i] == '\\' && i + 1 < source.size()) i += 2;
                else if (source[i++] == quote) break;
            }
        } else {
            ++i;
            continue;
        }
        for (std::size_t j = begin; j < i; ++j) {
            if (code[j] != '\r' && code[j] != '\n') code[j] = ' ';
        }
    }
    return code;
}

struct Edit {
    std::size_t position;
    std::size_t length;
    std::string replacement;
};

// Find only commas belonging to this call. Parentheses in constructors,
// nested samples, array indexing and comments cannot end its coordinates.
inline bool callArguments(const std::string& code, std::size_t open,
                          std::vector<std::size_t>& commas, std::size_t& close) {
    std::string delimiters;
    for (std::size_t i = open + 1; i < code.size(); ++i) {
        const char c = code[i];
        if (c == '(' || c == '[' || c == '{') delimiters += c;
        else if (c == ')' || c == ']' || c == '}') {
            if (delimiters.empty()) {
                if (c != ')') return false;
                close = i;
                return true;
            }
            const char expected = c == ')' ? '(' : c == ']' ? '[' : '{';
            if (delimiters.back() != expected) return false;
            delimiters.pop_back();
        } else if (c == ',' && delimiters.empty()) commas.push_back(i);
        else if (c == ';') return false;
    }
    return false;
}

// Marks the colons of the masked code that can introduce a semantic: after a
// parameter, a struct member or a function's parameter list. A colon that
// closes a `?` still open at its bracket depth in the statement belongs to a
// ternary, one after `case ...` or `default` to a label, and `::` is a scope;
// none of those is marked. `x ? 1 - color : color` and `default: color = c;`
// therefore neither make a shader legacy nor have `color` rewritten.
inline std::vector<bool> declarationColons(const std::string& code) {
    std::vector<bool> declaration(code.size());
    std::vector<int> questions{0};  // unmatched '?' per open bracket
    std::size_t statement = 0;
    const auto isLabel = [&](std::size_t colon) {
        std::size_t begin = code.find_first_not_of(" \t\r\n", statement);
        if (begin == std::string::npos || begin >= colon) return false;
        std::size_t end = begin;
        while (end < colon && (std::isalnum(static_cast<unsigned char>(code[end])) || code[end] == '_')) ++end;
        const std::string word = code.substr(begin, end - begin);
        return word == "case" || word == "default";
    };
    for (std::size_t i = 0; i < code.size(); ++i) {
        const char c = code[i];
        if (c == '(' || c == '[' || c == '{') {
            questions.push_back(0);
            if (c == '{') statement = i + 1;
        } else if (c == ')' || c == ']' || c == '}') {
            if (questions.size() > 1) questions.pop_back();
            if (c == '}') {
                questions.back() = 0;
                statement = i + 1;
            }
        } else if (c == ';') {
            questions.back() = 0;
            statement = i + 1;
        } else if (c == '?') {
            ++questions.back();
        } else if (c == ':') {
            if (i + 1 < code.size() && code[i + 1] == ':') {
                ++i;
            } else if (questions.back() > 0) {
                --questions.back();
            } else if (isLabel(i)) {
                statement = i + 1;
            } else {
                declaration[i] = true;
            }
        }
    }
    return declaration;
}

}  // namespace shader_compat_detail

// How adaptPotPlayerShader gives a legacy main() the position.
enum class MainPosition {
    // First, unless main's first input is a position already: a parameter,
    // or the first member of a struct parameter, with SV_POSITION, VPOS or
    // POSITION.
    Given,
    // Not at all: main's parameters as written, as they were before main()
    // was given the position.
    AsWritten,
    // As Given, but before a struct parameter whatever its struct is found
    // to begin with. The code is not preprocessed, so the struct found is
    // the first definition of that name in the code, which may be one an #if
    // leaves out.
    BeforeStructs,
};

// A PotPlayer/MPC (D3D9) shader made a ps_4_0 one; a native shader is
// returned as it is.
inline std::string adaptPotPlayerShader(std::string source, MainPosition mainPosition = MainPosition::Given) {
    if (source.size() >= 3 && static_cast<unsigned char>(source[0]) == 0xEF &&
        static_cast<unsigned char>(source[1]) == 0xBB && static_cast<unsigned char>(source[2]) == 0xBF) {
        source.erase(0, 3);
    }
    using namespace shader_compat_detail;
    const std::string code = codeMask(source);
    const std::vector<bool> declarationColon = declarationColons(code);
    bool legacy = std::regex_search(code, std::regex(
        R"(\btex2D(?:lod|bias|proj)?\s*\(|\bregister\s*\(\s*c0\s*\))", std::regex::icase));
    // A COLOR output semantic alone makes a shader legacy, since ps_4_0
    // refuses it, but only where a semantic can stand.
    const std::regex color(R"(:\s*COLOR0?\b)", std::regex::icase);
    for (std::sregex_iterator it(code.begin(), code.end(), color), end; !legacy && it != end; ++it) {
        legacy = declarationColon[static_cast<std::size_t>(it->position())];
    }
    if (!legacy) return source;
    std::vector<Edit> edits;
    // Legacy D3D9 samplers combine the texture and sampler in one register.
    // D3D11 splits those resources, so remove any name bound to s0 and route
    // every legacy tex2D call through QuadDeck's t0 texture and s0 sampler.
    const std::regex declarations(
        R"(\bsampler(?:1D|2D|3D|CUBE)?\s+[A-Za-z_]\w*\s*:\s*register\s*\(\s*s0\s*\)\s*;|\bfloat[234]?\s+(?:p0\s*:\s*register\s*\(\s*c0\s*\)|p1\s*:\s*register\s*\(\s*c1\s*\)|pPrev\s*:\s*register\s*\(\s*c2\s*\))\s*;)",
        std::regex::icase);
    for (std::sregex_iterator it(code.begin(), code.end(), declarations), end; it != end; ++it) {
        const auto position = static_cast<std::size_t>(it->position());
        const auto length = static_cast<std::size_t>(it->length());
        std::string blank = source.substr(position, length);
        for (std::size_t i = 0; i < length; ++i) {
            if (!std::isspace(static_cast<unsigned char>(code[position + i]))) blank[i] = ' ';
        }
        edits.push_back({position, length, std::move(blank)});
    }
    // Normalize the unnumbered D3D9 semantic explicitly. Although TEXCOORD
    // conventionally means index 0, legacy bytecode/linking paths do not all
    // expose the same signature to a D3D11 vertex shader.
    const std::regex semantics(R"(:\s*(TEXCOORD\b(?!\s*\d)|COLOR0?\b))", std::regex::icase);
    for (std::sregex_iterator it(code.begin(), code.end(), semantics), end; it != end; ++it) {
        if (!declarationColon[static_cast<std::size_t>(it->position())]) continue;
        std::string semantic = (*it)[1].str();
        const bool texcoord = semantic[0] == 'T' || semantic[0] == 't';
        // Keep the established space after ':' for ordinary legacy scripts,
        // while replacing only the semantic token when comments intervene.
        const auto position = static_cast<std::size_t>(it->position());
        const auto token = static_cast<std::size_t>(it->position(1));
        if (source.substr(position, token - position) == code.substr(position, token - position)) {
            edits.push_back({position, static_cast<std::size_t>(it->length()),
                             texcoord ? ": TEXCOORD0" : ": SV_TARGET"});
        } else {
            edits.push_back({token, semantic.size(), texcoord ? "TEXCOORD0" : "SV_TARGET"});
        }
    }
    // D3D11 links the vertex and pixel shaders by register, not by name, and
    // QuadDeck's vertex shader writes SV_POSITION first and the video
    // coordinate after it, as TEXCOORD0 and again as TEXCOORD1-3. A D3D9
    // main() usually takes no position, so its TEXCOORD0 packed into the
    // position's register and every pane came out one colour. main()
    // therefore takes the position first unless its first input is one
    // already: a parameter, or the first member of a struct parameter, with
    // SV_POSITION, VPOS or D3D9's POSITION, which packs into register 0
    // itself. A position further on stays where it is: the compiler makes
    // one input of an SV_POSITION and the one inserted, and a VPOS or
    // POSITION then follows the coordinate. A single TEXCOORDn is read as
    // TEXCOORD0: it holds the same coordinate, and so matches the vertex
    // shader's name in that register too. VPOS and a struct's members are
    // left as they are; the renderer refuses a file whose inputs the vertex
    // shader still does not feed (unfedShaderInput). A position this lookup
    // does not see, in a typedef'd or nested struct or behind a macro, is
    // pushed a register on by the one inserted; the renderer then compiles
    // the file without it (MainPosition::AsWritten) and takes that when the
    // vertex shader feeds it so.
    // The code is not preprocessed, so every definition of main is adapted:
    // one inside #if 0 is not compiled, whatever it is given. A main() with a
    // parameter of a built-in type but no semantic cannot be the entry point,
    // which is another overload; it is left as written. A struct is looked
    // up by its first definition in the code, so one in #if 0 can say that
    // main() has its position where the struct compiled does not begin with
    // one; the renderer then compiles the file with the position before the
    // struct as well (MainPosition::BeforeStructs).
    const std::regex entry(R"(\bmain\s*\()");
    const std::regex parameterSemantic(R"(:\s*([A-Za-z_]\w*))");
    const std::regex texcoordIndex(R"(TEXCOORD(\d*))", std::regex::icase);
    const std::regex positionSemantic(R"((SV_POSITION|VPOS|POSITION)\d*)", std::regex::icase);
    const std::regex identifier(R"([A-Za-z_]\w*)");
    const std::regex builtInType(
        R"((?:float|half|double|int|uint|dword|bool|min16float|min10float|min16int|min12int|min16uint)(?:[1-4](?:x[1-4])?)?)");
    // The semantic of the declaration in code[begin, finish) and where its
    // name starts, or an empty name.
    struct Semantic {
        std::string name;
        std::size_t position = 0;
    };
    const auto semanticIn = [&](std::size_t begin, std::size_t finish) {
        const auto from = code.begin() + static_cast<std::ptrdiff_t>(begin);
        const auto to = code.begin() + static_cast<std::ptrdiff_t>(finish);
        for (std::sregex_iterator s(from, to, parameterSemantic), stop; s != stop; ++s) {
            if (declarationColon[begin + static_cast<std::size_t>(s->position())]) {
                return Semantic{(*s)[1].str(), begin + static_cast<std::size_t>(s->position(1))};
            }
        }
        return Semantic{};
    };
    // Whether the struct named begins with the position: its first member.
    const auto structPositioned = [&](const std::string& type) {
        if (type.empty()) return false;
        const std::regex definition("\\bstruct\\s+" + type + "\\s*\\{");
        std::smatch found;
        if (!std::regex_search(code, found, definition)) return false;
        const auto open = static_cast<std::size_t>(found.position() + found.length() - 1);
        const std::size_t member = code.find_first_of(";}", open + 1);
        return member != std::string::npos &&
               std::regex_match(semanticIn(open + 1, member).name, positionSemantic);
    };
    // A parameter's type, its first word that is not a modifier, and
    // whether it is a constant (uniform) or an output (out) rather than an
    // input the vertex shader feeds.
    struct Parameter {
        std::string type;
        bool constant = false;
        bool output = false;
    };
    const auto parameterOf = [&](const std::string& text) {
        static const char* const modifiers[] = {"in", "inout", "const", "centroid", "linear",
                                                "noperspective", "nointerpolation", "sample", "precise"};
        Parameter parameter;
        for (std::sregex_iterator w(text.begin(), text.end(), identifier), stop; w != stop; ++w) {
            const std::string word = w->str();
            if (word == "uniform") {
                parameter.constant = true;
            } else if (word == "out") {
                parameter.output = true;
            } else if (std::none_of(std::begin(modifiers), std::end(modifiers),
                                    [&](const char* modifier) { return word == modifier; })) {
                parameter.type = word;
                break;
            }
        }
        return parameter;
    };
    for (std::sregex_iterator it(code.begin(), code.end(), entry), end;
         mainPosition != MainPosition::AsWritten && it != end; ++it) {
        const auto open = static_cast<std::size_t>(it->position() + it->length() - 1);
        std::vector<std::size_t> commas;
        std::size_t close = 0;
        if (!callArguments(code, open, commas, close)) continue;
        // The definition: its parameters, perhaps its semantic, its body.
        std::size_t next = code.find_first_not_of(" \t\r\n", close + 1);
        if (next != std::string::npos && code[next] == ':') {
            next = code.find_first_not_of(" \t\r\n", next + 1);
            while (next < code.size() && (std::isalnum(static_cast<unsigned char>(code[next])) || code[next] == '_')) {
                ++next;
            }
            next = code.find_first_not_of(" \t\r\n", next);
        }
        if (next == std::string::npos || code[next] != '{') continue;
        std::vector<std::pair<std::size_t, std::size_t>> parameters;
        std::size_t first = open + 1;
        for (const std::size_t comma : commas) {
            parameters.push_back({first, comma});
            first = comma + 1;
        }
        parameters.push_back({first, close});
        bool entryPoint = true;
        bool positioned = false;
        bool leading = true;   // the next input is main's first
        bool unnamed = false;  // an input without a semantic: a struct
        std::size_t texcoords = 0;
        std::size_t texcoordToken = 0;
        std::size_t texcoordLength = 0;
        bool texcoordZero = true;
        bool empty = true;
        for (const auto& [begin, finish] : parameters) {
            const std::string text = code.substr(begin, finish - begin);
            const std::size_t word = text.find_first_not_of(" \t\r\n");
            if (word == std::string::npos) continue;
            const std::size_t last = text.find_last_not_of(" \t\r\n");
            if (parameters.size() == 1 && text.substr(word, last + 1 - word) == "void") continue;
            empty = false;
            const Parameter parameter = parameterOf(text);
            if (parameter.constant) continue;
            const Semantic semantic = semanticIn(begin, finish);
            if (semantic.name.empty() && std::regex_match(parameter.type, builtInType)) {
                entryPoint = false;
                break;
            }
            if (parameter.output) continue;
            std::smatch index;
            if (semantic.name.empty()) {
                unnamed = true;
                if (leading) positioned = mainPosition == MainPosition::Given && structPositioned(parameter.type);
            } else if (std::regex_match(semantic.name, positionSemantic)) {
                if (leading) positioned = true;
            } else if (std::regex_match(semantic.name, index, texcoordIndex)) {
                ++texcoords;
                texcoordToken = semantic.position;
                texcoordLength = semantic.name.size();
                texcoordZero = index[1].str().find_first_not_of('0') == std::string::npos;
            }
            leading = false;
        }
        if (!entryPoint) continue;
        if (!positioned) {
            if (empty) {
                // main() or main(void): the position is its one parameter.
                edits.push_back({open + 1, close - open - 1, "float4 qdPosition : SV_POSITION"});
            } else {
                edits.push_back({open + 1, 0, "float4 qdPosition : SV_POSITION, "});
            }
        }
        if (!unnamed && texcoords == 1 && !texcoordZero) {
            edits.push_back({texcoordToken, texcoordLength, "TEXCOORD0"});
        }
    }
    bool level = false, bias = false, project = false;
    const std::regex samples(R"(\b(tex2D(?:lod|bias|proj)?)\s*\()", std::regex::icase);
    const std::regex sampler(R"(\s*([A-Za-z_]\w*)\s*)");
    for (std::sregex_iterator it(code.begin(), code.end(), samples), end; it != end; ++it) {
        const auto open = static_cast<std::size_t>(it->position() + it->length() - 1);
        std::vector<std::size_t> commas;
        std::size_t close = 0;
        if (!callArguments(code, open, commas, close) || commas.size() != 1) continue;
        const std::string first = code.substr(open + 1, commas[0] - open - 1);
        std::smatch name;
        if (!std::regex_match(first, name, sampler) ||
            code.find_first_not_of(" \t\r\n", commas[0] + 1) >= close) continue;
        std::string function = (*it)[1].str();
        for (char& c : function) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const bool plain = function == "tex2d";
        const char* replacement = "qdTexture.Sample";
        if (function == "tex2dlod") { replacement = "qdSampleLevel"; level = true; }
        if (function == "tex2dbias") { replacement = "qdSampleBias"; bias = true; }
        if (function == "tex2dproj") { replacement = "qdSampleProject"; project = true; }
        edits.push_back({static_cast<std::size_t>(it->position(1)), function.size(), replacement});
        edits.push_back({open + 1 + static_cast<std::size_t>(name.position(1)),
                         static_cast<std::size_t>(name.length(1)), plain ? "qdSampler" : ""});
        if (!plain) edits.push_back({commas[0], 1, ""});
    }
    std::sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) {
        return a.position > b.position;
    });
    for (const auto& edit : edits) source.replace(edit.position, edit.length, edit.replacement);
    std::string prefix =
        "Texture2D qdTexture : register(t0);\n"
        "SamplerState qdSampler : register(s0);\n"
        // c2 is used by some shaders as host-provided temporal state. A pixel
        // shader cannot feed arbitrary per-pixel output back into a constant
        // buffer, but defining the register explicitly avoids a second legacy
        // $Globals buffer colliding with b0 and keeps the shader spatially valid.
        "cbuffer PotPlayerParams : register(b0) { float4 p0; float4 p1; float4 pPrev; };\n";
    // A real function evaluates the complete float4 coordinate once. Duplicating
    // it for .xy/.w would execute nested samples or inout expressions twice.
    if (level) prefix += "float4 qdSampleLevel(float4 p) { return qdTexture.SampleLevel(qdSampler, p.xy, p.w); }\n";
    if (bias) prefix += "float4 qdSampleBias(float4 p) { return qdTexture.SampleBias(qdSampler, p.xy, p.w); }\n";
    if (project) prefix += "float4 qdSampleProject(float4 p) { return qdTexture.Sample(qdSampler, p.xy / p.w); }\n";
    return prefix + source;
}

// One element of a compiled shader's input or output signature, as
// D3DReflect reports it.
struct ShaderSignatureValue {
    std::string semantic;     // as declared: TEXCOORD, SV_Position, VPOS...
    unsigned index = 0;
    unsigned registerIndex = 0;
    unsigned mask = 0;        // the components it occupies, bit 0 for x
    unsigned read = 0;        // an input's components the shader reads
    bool position = false;    // the system value SV_POSITION
    bool distance = false;    // SV_ClipDistance or SV_CullDistance
    bool system = false;      // any other system value, SV_IsFrontFace...
    bool floating = true;     // 32-bit float components
};

// Why the vertex shader's outputs do not feed a pixel shader's inputs, or
// empty when they do. D3D11 links the two stages by register and component,
// not by name: an input reads whatever the vertex shader wrote in its
// register's components. QuadDeck's vertex shader writes SV_POSITION in
// register 0 and the same video coordinate as every TEXCOORD after it, a
// float2 in each .xy and .zw. An input the shader reads is therefore fed
// when it is a float and either
// - D3D9's VPOS or POSITION, a semantic of the author's in ps_4_0, where
//   the vertex shader writes SV_POSITION. It loads there as it always did,
//   though the drivers give it zeros (NVIDIA) or the clip position (Intel,
//   WARP), not the pixel position: that matters only to a file whose
//   picture depends on it, and the advice is SV_POSITION; or
// - any other input, a TEXCOORD of any index or another name, where the
//   vertex shader writes TEXCOORDs, reading only components they write: a
//   single float reads the coordinate's u or v, and a wider input starts
//   where a TEXCOORD starts, so that it reads u then v.
// Interpolation modifiers, arrays, wider TEXCOORDs, gaps in the indices,
// the coordinate as two floats and names of the author's own all link so.
// An input never read is not checked, and nor is a system value but
// SV_ClipDistance and SV_CullDistance. The pipeline gives the pixel shader
// SV_POSITION, the pixel centre, SV_IsFrontFace and SV_PrimitiveID itself,
// in whatever register they are. SV_RenderTargetArrayIndex and
// SV_ViewportArrayIndex are written by the stage before it, as the
// distances are, and QuadDeck's vertex shader writes neither, but they are
// skipped all the same: the NVIDIA card and the Intel Arc give 0, the index
// of the one target and viewport there are, and only WARP gives what the
// vertex shader writes in that register (TEXCOORD2.x's bits), so a file
// reading them draws the picture on a card, as it did before. A clip or
// cull distance is the vertex shader's to write, so one that is read is
// fed only where the vertex shader writes one; QuadDeck's writes neither.
inline std::string unfedShaderInput(const std::vector<ShaderSignatureValue>& outputs,
                                    const std::vector<ShaderSignatureValue>& inputs) {
    const auto upper = [](std::string text) {
        for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return text;
    };
    const auto lowest = [](unsigned mask) {
        unsigned component = 0;
        while (component < 4 && !(mask & (1u << component))) ++component;
        return component;
    };
    const std::string order =
        " QuadDeck's vertex shader gives the position in input register 0 and the video coordinate after it, "
        "so main() takes the position first, as a float4, then its TEXCOORD inputs: "
        "float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET "
        "(in a PotPlayer shader too: float4 pos : SV_POSITION, the pixel centre; "
        "D3D9's VPOS is not given the position).";
    for (const auto& input : inputs) {
        if (input.system || input.position || input.read == 0) continue;
        std::string components;
        for (unsigned c = 0; c < 4; ++c) {
            if (input.mask & (1u << c)) components += "xyzw"[c];
        }
        const std::string reads = "The pixel shader's main() reads " + input.semantic +
                                  std::to_string(input.index) + " in input register " +
                                  std::to_string(input.registerIndex) + "." + components;
        if (input.distance) {
            unsigned given = 0;
            for (const auto& output : outputs) {
                if (output.distance && output.registerIndex == input.registerIndex) given |= output.mask;
            }
            if ((input.read & ~given) == 0) continue;
            return reads + ", a clip or cull distance, which only the vertex shader gives, and QuadDeck's "
                           "gives none.";
        }
        const std::string name = upper(input.semantic);
        bool position = false;
        bool positionAligned = false;
        bool written = false;
        bool aligned = false;
        unsigned covered = 0;
        for (const auto& output : outputs) {
            if (output.registerIndex != input.registerIndex) continue;
            written = true;
            if (output.position) {
                position = true;
                positionAligned = lowest(output.mask) == lowest(input.mask) && (input.read & ~output.mask) == 0;
            } else {
                covered |= output.mask;
                aligned = aligned || lowest(output.mask) == lowest(input.mask);
            }
        }
        if (!input.floating) {
            return reads + ", which is not a float; QuadDeck's vertex shader gives float values only.";
        }
        if (name == "VPOS" || name == "POSITION") {
            if (positionAligned) continue;
            return reads + (position ? ", behind another input in the position's register."
                            : written ? ", where the vertex shader gives the video coordinate, not the position."
                                      : ", where the vertex shader gives nothing.") +
                   order;
        }
        if (position) return reads + ", where the vertex shader gives the position." + order;
        if (!written || covered == 0) {
            return reads + ", past the video coordinate's registers: the vertex shader gives TEXCOORD0-3 as "
                           "float2 values in .xy and .zw of input registers 1 and 2.";
        }
        const bool single = (input.mask & (input.mask - 1)) == 0;
        if ((!aligned && !single) || (input.read & ~covered) != 0) {
            return reads + ", across the vertex shader's float2 coordinates in .xy and .zw, so its first "
                           "component is not the coordinate's u.";
        }
    }
    return {};
}

}  // namespace quaddeck
