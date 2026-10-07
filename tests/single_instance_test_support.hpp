#pragma once

#include "SingleInstance.hpp"

#include <filesystem>
#include <optional>
#include <nlohmann/json.hpp>

namespace quaddeck {

// This executable's test-only child modes always inject an isolated namespace.
// They never enter App::run or load user settings, auth, media or devices.
inline constexpr wchar_t kSingleInstanceTestWindowClass[] = L"QuadDeck.SingleInstance.RegressionReceiver";
inline constexpr UINT kSingleInstanceTestControl = WM_APP + 91;
enum class SingleInstanceTestControl : WPARAM { Stop = 1, Cancel, ReplaceLast, Minimize, CloseSecond, StopAccepting, AbruptExit };

HWND createSingleInstanceTestWindow(const std::wstring& testNamespace);
void writeSingleInstanceTestState(const std::filesystem::path& directory, const nlohmann::json& state);
std::optional<int> singleInstanceProcessMode(int argc, char** argv);

// Defined alongside the existing AppRegressionTests friend so the receiving
// child exercises the actual App intake/chooser methods and synthetic sources.
int singleInstanceAppReceiver(SingleInstance& instance, const SingleInstance::Request& initial,
                              const std::wstring& testNamespace, const std::filesystem::path& directory);

} // namespace quaddeck
