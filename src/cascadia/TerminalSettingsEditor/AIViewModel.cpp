// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "AIViewModel.h"
#include "AIViewModel.g.cpp"
#include "EnumEntry.h"

using namespace winrt::Windows::Foundation;
using namespace winrt::Microsoft::Terminal::Settings::Model;

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    AIViewModel::AIViewModel(Model::AISettings aiSettings) :
        _aiSettings{ aiSettings }
    {
        INITIALIZE_BINDABLE_ENUM_SETTING(Provider, AIProvider, AIProvider, L"AI_Provider", L"Content");

        // When Provider changes, update IsAzureProvider visibility
        PropertyChanged([this](auto&&, const Windows::UI::Xaml::Data::PropertyChangedEventArgs& args) {
            const auto viewModelProperty{ args.PropertyName() };
            if (viewModelProperty == L"CurrentProvider")
            {
                _NotifyChanges(L"IsAzureProvider");
            }
        });
    }

    bool AIViewModel::Enabled() const { return _aiSettings.Enabled(); }
    void AIViewModel::Enabled(bool value) { _aiSettings.Enabled(value); _NotifyChanges(L"Enabled"); }

    winrt::hstring AIViewModel::ApiKey() const { return _aiSettings.ApiKey(); }
    void AIViewModel::ApiKey(const winrt::hstring& value) { _aiSettings.ApiKey(value); _NotifyChanges(L"ApiKey"); }

    winrt::hstring AIViewModel::ApiEndpoint() const { return _aiSettings.ApiEndpoint(); }
    void AIViewModel::ApiEndpoint(const winrt::hstring& value) { _aiSettings.ApiEndpoint(value); _NotifyChanges(L"ApiEndpoint"); }

    winrt::hstring AIViewModel::ModelName() const { return _aiSettings.ModelName(); }
    void AIViewModel::ModelName(const winrt::hstring& value) { _aiSettings.ModelName(value); _NotifyChanges(L"ModelName"); }

    winrt::hstring AIViewModel::AzureDeployment() const { return _aiSettings.AzureDeployment(); }
    void AIViewModel::AzureDeployment(const winrt::hstring& value) { _aiSettings.AzureDeployment(value); _NotifyChanges(L"AzureDeployment"); }

    winrt::hstring AIViewModel::AzureApiVersion() const { return _aiSettings.AzureApiVersion(); }
    void AIViewModel::AzureApiVersion(const winrt::hstring& value) { _aiSettings.AzureApiVersion(value); _NotifyChanges(L"AzureApiVersion"); }

    int32_t AIViewModel::MaxContextBlocks() const { return _aiSettings.MaxContextBlocks(); }
    void AIViewModel::MaxContextBlocks(int32_t value) { _aiSettings.MaxContextBlocks(value); _NotifyChanges(L"MaxContextBlocks"); }

    winrt::hstring AIViewModel::CopilotCliPath() const { return _aiSettings.CopilotCliPath(); }
    void AIViewModel::CopilotCliPath(const winrt::hstring& value)
    {
        _aiSettings.CopilotCliPath(value);
        _NotifyChanges(L"CopilotCliPath", L"IsCopilotCliDetected");
    }

    bool AIViewModel::IsAzureProvider() const
    {
        return _aiSettings.Provider() == AIProvider::AzureOpenAI;
    }

    bool AIViewModel::IsCopilotCliDetected() const
    {
        return _aiSettings.IsCopilotCliDetected();
    }
}
