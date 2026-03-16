// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "AISettings.h"
#include "AISettings.g.cpp"

#include "JsonUtils.h"

using namespace Microsoft::Terminal::Settings::Model;
using namespace winrt::Microsoft::Terminal::Settings::Model::implementation;

static constexpr std::string_view MaxContextBlocksKey{ "maxContextBlocks" };
static constexpr std::string_view CopilotCliPathKey{ "copilotCliPath" };
static constexpr std::string_view GithubTokenKey{ "githubToken" };

AISettings::AISettings() = default;

winrt::com_ptr<AISettings> AISettings::Copy() const
{
    auto copy{ winrt::make_self<AISettings>() };
    copy->_maxContextBlocks = _maxContextBlocks;
    copy->_copilotCliPath = _copilotCliPath;
    copy->_githubToken = _githubToken;
    return copy;
}

winrt::com_ptr<AISettings> AISettings::FromJson(const Json::Value& json)
{
    auto result{ winrt::make_self<AISettings>() };
    result->LayerJson(json);
    return result;
}

void AISettings::LayerJson(const Json::Value& json)
{
    JsonUtils::GetValueForKey(json, MaxContextBlocksKey, _maxContextBlocks);
    JsonUtils::GetValueForKey(json, CopilotCliPathKey, _copilotCliPath);
    JsonUtils::GetValueForKey(json, GithubTokenKey, _githubToken);
}

Json::Value AISettings::ToJson() const
{
    Json::Value json{ Json::ValueType::objectValue };
    JsonUtils::SetValueForKey(json, MaxContextBlocksKey, _maxContextBlocks);
    JsonUtils::SetValueForKey(json, CopilotCliPathKey, _copilotCliPath);
    JsonUtils::SetValueForKey(json, GithubTokenKey, _githubToken);
    return json;
}

std::wstring AISettings::_resolvedCliPath() const
{
    if (!_copilotCliPath.empty())
    {
        return std::wstring{ _copilotCliPath };
    }

    // Search PATH for "copilot.exe"
    wchar_t pathBuf[MAX_PATH];
    const auto result = SearchPathW(nullptr, L"copilot.exe", nullptr, MAX_PATH, pathBuf, nullptr);
    if (result > 0)
    {
        return pathBuf;
    }
    return L"copilot";
}

bool AISettings::IsCopilotCliDetected() const
{
    // If explicit path is set, check that file exists
    if (!_copilotCliPath.empty())
    {
        return GetFileAttributesW(_copilotCliPath.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    // Otherwise search PATH for "copilot.exe"
    wchar_t pathBuf[MAX_PATH];
    const auto result = SearchPathW(nullptr, L"copilot.exe", nullptr, MAX_PATH, pathBuf, nullptr);
    return result > 0;
}

bool AISettings::IsCopilotAuthLoggedIn() const
{
    return !_githubToken.empty();
}
