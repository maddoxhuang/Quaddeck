#pragma once

#include <algorithm>
#include <cctype>
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

}  // namespace shader_compat_detail

inline std::string adaptPotPlayerShader(std::string source) {
    if (source.size() >= 3 && static_cast<unsigned char>(source[0]) == 0xEF &&
        static_cast<unsigned char>(source[1]) == 0xBB && static_cast<unsigned char>(source[2]) == 0xBF) {
        source.erase(0, 3);
    }
    using namespace shader_compat_detail;
    const std::string code = codeMask(source);
    const bool legacy = std::regex_search(code, std::regex(
        R"(\btex2D(?:lod|bias|proj)?\s*\(|\bregister\s*\(\s*c0\s*\)|:\s*COLOR0?\b)",
        std::regex::icase));
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

}  // namespace quaddeck
