// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "AISettings.h"
#include "AISettings.g.cpp"

#include "TerminalSettingsSerializationHelpers.h"
#include "JsonUtils.h"

using namespace Microsoft::Terminal::Settings::Model;
using namespace winrt::Microsoft::Terminal::Settings::Model::implementation;

static constexpr std::string_view EnabledKey{ "enabled" };
static constexpr std::string_view ProviderKey{ "provider" };
static constexpr std::string_view ApiKeyKey{ "apiKey" };
static constexpr std::string_view ApiEndpointKey{ "apiEndpoint" };
static constexpr std::string_view ModelKey{ "model" };
static constexpr std::string_view AzureDeploymentKey{ "azureDeployment" };
static constexpr std::string_view AzureApiVersionKey{ "azureApiVersion" };
static constexpr std::string_view MaxContextBlocksKey{ "maxContextBlocks" };
static constexpr std::string_view CopilotCliPathKey{ "copilotCliPath" };

AISettings::AISettings() = default;

com_ptr<AISettings> AISettings::Copy() const
{
    auto copy{ winrt::make_self<AISettings>() };
    copy->_enabled = _enabled;
    copy->_provider = _provider;
    copy->_apiKey = _apiKey;
    copy->_apiEndpoint = _apiEndpoint;
    copy->_model = _model;
    copy->_azureDeployment = _azureDeployment;
    copy->_azureApiVersion = _azureApiVersion;
    copy->_maxContextBlocks = _maxContextBlocks;
    copy->_copilotCliPath = _copilotCliPath;
    return copy;
}

com_ptr<AISettings> AISettings::FromJson(const Json::Value& json)
{
    auto result{ winrt::make_self<AISettings>() };
    result->LayerJson(json);
    return result;
}

void AISettings::LayerJson(const Json::Value& json)
{
    JsonUtils::GetValueForKey(json, EnabledKey, _enabled);
    JsonUtils::GetValueForKey(json, ProviderKey, _provider);
    JsonUtils::GetValueForKey(json, ApiKeyKey, _apiKey);
    JsonUtils::GetValueForKey(json, ApiEndpointKey, _apiEndpoint);
    JsonUtils::GetValueForKey(json, ModelKey, _model);
    JsonUtils::GetValueForKey(json, AzureDeploymentKey, _azureDeployment);
    JsonUtils::GetValueForKey(json, AzureApiVersionKey, _azureApiVersion);
    JsonUtils::GetValueForKey(json, MaxContextBlocksKey, _maxContextBlocks);
    JsonUtils::GetValueForKey(json, CopilotCliPathKey, _copilotCliPath);
}

Json::Value AISettings::ToJson() const
{
    Json::Value json{ Json::ValueType::objectValue };
    JsonUtils::SetValueForKey(json, EnabledKey, _enabled);
    JsonUtils::SetValueForKey(json, ProviderKey, _provider);
    JsonUtils::SetValueForKey(json, ApiKeyKey, _apiKey);
    JsonUtils::SetValueForKey(json, ApiEndpointKey, _apiEndpoint);
    JsonUtils::SetValueForKey(json, ModelKey, _model);
    JsonUtils::SetValueForKey(json, AzureDeploymentKey, _azureDeployment);
    JsonUtils::SetValueForKey(json, AzureApiVersionKey, _azureApiVersion);
    JsonUtils::SetValueForKey(json, MaxContextBlocksKey, _maxContextBlocks);
    JsonUtils::SetValueForKey(json, CopilotCliPathKey, _copilotCliPath);
    return json;
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
