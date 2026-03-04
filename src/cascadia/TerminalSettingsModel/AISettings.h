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
        bool Enabled() const noexcept { return _enabled; }
        void Enabled(bool value) noexcept { _enabled = value; }

        Model::AIProvider Provider() const noexcept { return _provider; }
        void Provider(Model::AIProvider value) noexcept { _provider = value; }

        winrt::hstring ApiKey() const { return _apiKey; }
        void ApiKey(const winrt::hstring& value) { _apiKey = value; }

        winrt::hstring ApiEndpoint() const { return _apiEndpoint; }
        void ApiEndpoint(const winrt::hstring& value) { _apiEndpoint = value; }

        winrt::hstring ModelName() const { return _model; }
        void ModelName(const winrt::hstring& value) { _model = value; }

        winrt::hstring AzureDeployment() const { return _azureDeployment; }
        void AzureDeployment(const winrt::hstring& value) { _azureDeployment = value; }

        winrt::hstring AzureApiVersion() const { return _azureApiVersion; }
        void AzureApiVersion(const winrt::hstring& value) { _azureApiVersion = value; }

        int32_t MaxContextBlocks() const noexcept { return _maxContextBlocks; }
        void MaxContextBlocks(int32_t value) noexcept { _maxContextBlocks = value; }

        winrt::hstring CopilotCliPath() const { return _copilotCliPath; }
        void CopilotCliPath(const winrt::hstring& value) { _copilotCliPath = value; }

        bool IsCopilotCliDetected() const;

    private:
        bool _enabled{ false };
        Model::AIProvider _provider{ Model::AIProvider::OpenAI };
        winrt::hstring _apiKey;
        winrt::hstring _apiEndpoint;
        winrt::hstring _model{ L"gpt-4o" };
        winrt::hstring _azureDeployment;
        winrt::hstring _azureApiVersion{ L"2024-12-01-preview" };
        int32_t _maxContextBlocks{ 5 };
        winrt::hstring _copilotCliPath;
    };
}

namespace winrt::Microsoft::Terminal::Settings::Model::factory_implementation
{
    BASIC_FACTORY(AISettings);
}
