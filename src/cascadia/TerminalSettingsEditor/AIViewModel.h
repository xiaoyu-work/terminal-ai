// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "AIViewModel.g.h"
#include "ViewModelHelpers.h"
#include "Utils.h"

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    struct AIViewModel : AIViewModelT<AIViewModel>, ViewModelHelper<AIViewModel>
    {
    public:
        AIViewModel(Model::AISettings aiSettings);

        // DON'T YOU DARE ADD A `WINRT_CALLBACK(PropertyChanged` TO A CLASS DERIVED FROM ViewModelHelper. Do this instead:
        using ViewModelHelper<AIViewModel>::PropertyChanged;

        // Simple property forwarding
        bool Enabled() const;
        void Enabled(bool value);

        winrt::hstring ApiKey() const;
        void ApiKey(const winrt::hstring& value);

        winrt::hstring ApiEndpoint() const;
        void ApiEndpoint(const winrt::hstring& value);

        winrt::hstring ModelName() const;
        void ModelName(const winrt::hstring& value);

        winrt::hstring AzureDeployment() const;
        void AzureDeployment(const winrt::hstring& value);

        winrt::hstring AzureApiVersion() const;
        void AzureApiVersion(const winrt::hstring& value);

        int32_t MaxContextBlocks() const;
        void MaxContextBlocks(int32_t value);

        winrt::hstring CopilotCliPath() const;
        void CopilotCliPath(const winrt::hstring& value);

        bool IsAzureProvider() const;
        bool IsCopilotCliDetected() const;

        GETSET_BINDABLE_ENUM_SETTING(Provider, Model::AIProvider, _aiSettings.Provider);

    private:
        Model::AISettings _aiSettings;
    };
};

namespace winrt::Microsoft::Terminal::Settings::Editor::factory_implementation
{
    BASIC_FACTORY(AIViewModel);
}
