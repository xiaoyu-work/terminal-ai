// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "AIViewModel.g.h"
#include "ViewModelHelpers.h"

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    struct AIViewModel : AIViewModelT<AIViewModel>, ViewModelHelper<AIViewModel>
    {
    public:
        AIViewModel(Model::AISettings aiSettings);

        // DON'T YOU DARE ADD A `WINRT_CALLBACK(PropertyChanged` TO A CLASS DERIVED FROM ViewModelHelper. Do this instead:
        using ViewModelHelper<AIViewModel>::PropertyChanged;

        int32_t MaxContextBlocks() const;
        void MaxContextBlocks(int32_t value);

        winrt::hstring CopilotCliPath() const;
        void CopilotCliPath(const winrt::hstring& value);

        bool IsCopilotCliDetected() const;
        bool IsCopilotAuthLoggedIn() const;

        // Device code flow
        winrt::hstring UserCode() const { return _userCode; }
        winrt::hstring VerificationUri() const { return _verificationUri; }
        bool IsOAuthInProgress() const { return _isOAuthInProgress; }
        winrt::hstring OAuthError() const { return _oauthError; }

        void CopilotAuthLogin();
        void OpenVerificationUri();
        void CopyUserCode();
        void CancelOAuth();
        void Disconnect();

    private:
        Model::AISettings _aiSettings;

        // Device code flow state
        winrt::hstring _userCode;
        winrt::hstring _verificationUri;
        bool _isOAuthInProgress{ false };
        winrt::hstring _oauthError;
        std::wstring _deviceCode;
        int _pollInterval{ 5 };
        std::atomic<bool> _pollCancelled{ false };

        winrt::fire_and_forget _startDeviceCodeFlow();
    };
};

namespace winrt::Microsoft::Terminal::Settings::Editor::factory_implementation
{
    BASIC_FACTORY(AIViewModel);
}
