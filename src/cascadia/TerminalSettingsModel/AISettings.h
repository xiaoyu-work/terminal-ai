// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "AISettings.g.h"

namespace winrt::Microsoft::Terminal::Settings::Model::implementation
{
    struct AISettings : AISettingsT<AISettings>
    {
        AISettings();

        com_ptr<AISettings> Copy() const;

        static com_ptr<AISettings> FromJson(const Json::Value& json);
        void LayerJson(const Json::Value& json);
        Json::Value ToJson() const;

        // Properties with backing fields
        int32_t MaxContextBlocks() const noexcept { return _maxContextBlocks; }
        void MaxContextBlocks(int32_t value) noexcept { _maxContextBlocks = value; }

        winrt::hstring CopilotCliPath() const { return _copilotCliPath; }
        void CopilotCliPath(const winrt::hstring& value) { _copilotCliPath = value; }

        winrt::hstring GithubToken() const { return _githubToken; }
        void GithubToken(const winrt::hstring& value) { _githubToken = value; }

        bool IsCopilotCliDetected() const;
        bool IsCopilotAuthLoggedIn() const;

    private:
        std::wstring _resolvedCliPath() const;
        int32_t _maxContextBlocks{ 5 };
        winrt::hstring _copilotCliPath;
        winrt::hstring _githubToken;
    };
}

namespace winrt::Microsoft::Terminal::Settings::Model::factory_implementation
{
    BASIC_FACTORY(AISettings);
}
