#include "ShaderCompat.hpp"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <iostream>
#include <string>

namespace {

bool compile(const std::string& source, const char* target, const std::string& name) {
    Microsoft::WRL::ComPtr<ID3DBlob> bytecode, errors;
    const HRESULT result = D3DCompile(source.data(), source.size(), name.c_str(), nullptr,
        nullptr, "main", target, D3DCOMPILE_ENABLE_STRICTNESS, 0, &bytecode, &errors);
    if (SUCCEEDED(result)) return true;
    std::cerr << name << " failed for " << target << '\n';
    if (errors) std::cerr << static_cast<const char*>(errors->GetBufferPointer());
    return false;
}

bool check(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

std::size_t occurrences(const std::string& source, const std::string& text) {
    std::size_t count = 0;
    for (std::size_t i = 0; (i = source.find(text, i)) != std::string::npos; i += text.size()) ++count;
    return count;
}

bool legacyCase(const std::string& name, const std::string& body,
                const std::string& extra = "") {
    const std::string source = "sampler VideoSampler : register(s0);\n" + extra +
        "float4 main(float2 uv : TEXCOORD) : COLOR0 {\n" + body + "\n}\n";
    // The old compiler is the independent oracle that these inputs really are
    // legal legacy HLSL; a successful string rewrite alone proves very little.
    const bool original = compile(source, "ps_3_0", name + " original");
    const bool adapted = compile(quaddeck::adaptPotPlayerShader(source), "ps_4_0", name + " adapted");
    return original && adapted;
}

}  // namespace

int main() {
    bool passed = true;
    int legacyCases = 0;
    for (const std::string function : {"tex2Dlod", "tex2Dbias", "tex2Dproj"}) {
        const std::string sample = function + "(VideoSampler, float4(uv, 0, 1))";
        const auto run = [&](const char* scenario, const std::string& body,
                             const std::string& extra = "") {
            ++legacyCases;
            passed = legacyCase(function + " " + scenario, body, extra) && passed;
        };
        run("outer function", "return saturate(" + sample + ");");
        run("same expression", "return lerp(" + sample + ", " + sample + ", .5);");
        run("multiline", "return " + function + "(\n VideoSampler,\n float4(\n uv,\n 0,\n 1\n )\n );");
        run("nested constructor", "return " + function +
            "(VideoSampler, float4(normalize(float2(uv.x + 1, uv.y + 1)), 0, max(1, uv.x)));");
        run("nested sample", "return " + function + "(VideoSampler, float4(" + sample + ".xy, 0, 1));");
        run("nested ordinary sample", "return " + function +
            "(VideoSampler, float4(tex2D(VideoSampler, uv).xy, 0, 1));");
        run("suffix and following expression", "return float4(" + sample +
            ".rgb, 1) * 0.5 + float4(uv, 0, 1);");
        run("conditional coordinates", "return " + function +
            "(VideoSampler, uv.x > .5 ? float4(uv, 0, 1) : float4(1 - uv, 0, 1));");
        run("comments", "return saturate(" + function +
            " /* ( , */ ( /* sampler */ VideoSampler /* ) */ , // ) ; ,\n"
            "float4(uv, /* ), */ 0, 1) /* ( */ ));");
        run("inout coordinates", "float2 p = uv; float4 color = " + function +
            "(VideoSampler, nextCoordinates(p)); return color + float4(p, 0, 0);",
            "float4 nextCoordinates(inout float2 p) { p += .125; return float4(p, 0, 1); }\n");
    }
    passed = legacyCase("ordinary sample", "return saturate(tex2D(VideoSampler, normalize(uv)));") && passed;
    ++legacyCases;
    passed = legacyCase("array coordinates",
        "float4 coordinates[2] = { float4(uv, 0, 1), float4(1 - uv, 0, 1) };\n"
        "return tex2Dlod(VideoSampler, coordinates[0]);") && passed;
    ++legacyCases;

    const std::string preserved = R"(// tex2Dlod(s0, float4(uv, 0, 1)) : COLOR
/* sampler2D fake : register(s0); : TEXCOORD */
#define NOTE "tex2Dbias(s0, float4(uv, 0, 1)) : COLOR \" quoted"
Texture2D qdTexture : register(t0);
SamplerState qdSampler : register(s0);
float4 main(float2 uv : TEXCOORD0) : SV_TARGET {
    float tex2DResult = 1;
    return qdTexture.Sample(qdSampler, uv) * tex2DResult;
}
)";
    passed = check(quaddeck::adaptPotPlayerShader(preserved) == preserved,
                   "Modern HLSL changed because of a comment, string or identifier") && passed;
    passed = compile(preserved, "ps_4_0", "modern shader") && passed;
    passed = check(quaddeck::adaptPotPlayerShader("\xEF\xBB\xBF" + preserved) == preserved,
                   "UTF-8 BOM was not removed") && passed;

    const std::string comment = "/* sampler s0 : register(s0); tex2Dlod(s0, uv) : COLOR */";
    const std::string quoted = "#define NOTE \"tex2Dproj(s0, uv) : TEXCOORD\"\n";
    const std::string legacy = comment + "\n" + quoted +
        "sampler /* keep sampler comment */ s0 : register(s0);\n"
        "float4 main(float2 uv : /* keep semantic comment */ TEXCOORD) : COLOR {\n"
        "return tex2Dlod(s0, nextCoordinates(uv)); }\n";
    const std::string adapted = quaddeck::adaptPotPlayerShader(legacy);
    passed = check(adapted.find(comment) != std::string::npos && adapted.find(quoted) != std::string::npos &&
                   adapted.find("/* keep sampler comment */") != std::string::npos &&
                   adapted.find(": /* keep semantic comment */ TEXCOORD0") != std::string::npos,
                   "Legacy adaptation changed comments or quoted text") && passed;
    passed = check(occurrences(adapted, "nextCoordinates(uv)") == 1,
                   "Coordinate expression was duplicated") && passed;
    passed = check(adapted.find("qdTexture.SampleLevel(qdSampler, p.xy, p.w)") != std::string::npos,
                   "LOD coordinates do not map xy and w to SampleLevel") && passed;
    passed = check(quaddeck::adaptPotPlayerShader("float4 main(float2 uv:TEXCOORD):COLOR { "
                   "return tex2Dproj(s0, uv; }").find("tex2Dproj(s0, uv;") != std::string::npos,
                   "Malformed call consumed the following source") && passed;

    if (!passed) return 1;
    std::cout << "Shader compatibility passed: " << legacyCases
              << " legacy ps_3_0/ps_4_0 pairs plus lexical preservation checks\n";
    return 0;
}
