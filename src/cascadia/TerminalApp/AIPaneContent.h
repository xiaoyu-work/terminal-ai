// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once
#include "AIPaneContent.g.h"
#include "BasicPaneEvents.h"
#include "CopilotClient.h"
#include <memory>

namespace winrt::TerminalApp::implementation
{
    struct AIPaneContent : AIPaneContentT<AIPaneContent>, BasicPaneEvents
    {
        AIPaneContent();

        winrt::Windows::UI::Xaml::FrameworkElement GetRoot();

        void UpdateSettings(const winrt::Microsoft::Terminal::Settings::Model::CascadiaSettings& settings);

        winrt::Windows::Foundation::Size MinimumSize();
        void Focus(winrt::Windows::UI::Xaml::FocusState reason = winrt::Windows::UI::Xaml::FocusState::Programmatic);
        void Close();
        winrt::Microsoft::Terminal::Settings::Model::INewContentArgs GetNewTerminalArgs(BuildStartupKind kind) const;

        winrt::hstring Title() { return L"AI Assistant"; }
        uint64_t TaskbarState() { return 0; }
        uint64_t TaskbarProgress() { return 0; }
        bool ReadOnly() { return false; }
        winrt::hstring Icon() const;
        Windows::Foundation::IReference<winrt::Windows::UI::Color> TabColor() const noexcept { return nullptr; }
        winrt::Windows::UI::Xaml::Media::Brush BackgroundBrush();

        void SetLastActiveControl(const Microsoft::Terminal::Control::TermControl& control);

        // Observable properties
        winrt::hstring StatusText() const { return _statusText; }
        void StatusText(const winrt::hstring& value);
        bool IsStreaming() const { return _isStreaming; }
        void IsStreaming(bool value);

        // See BasicPaneEvents for most generic event definitions

        til::typed_event<winrt::Windows::Foundation::IInspectable,
                         Microsoft::Terminal::Settings::Model::ActionAndArgs>
            DispatchActionRequested;

        til::property_changed_event PropertyChanged;

    private:
        friend struct AIPaneContentT<AIPaneContent>; // for Xaml to bind events

        winrt::weak_ref<Microsoft::Terminal::Control::TermControl> _control{ nullptr };
        winrt::Microsoft::Terminal::Settings::Model::CascadiaSettings _settings{ nullptr };
        winrt::hstring _statusText{ L"Ready" };
        bool _isStreaming{ false };

        // Copilot SDK client (replaces AILLMClient + AIToolRegistry + AIAgentLoop)
        std::shared_ptr<CopilotClient> _copilotClient;
        winrt::Windows::System::DispatcherQueue _dispatcherQueue{ nullptr };

        // Accumulated assistant content during streaming
        std::wstring _currentAssistantContent;
        // Permission request RPC id pending approval
        int64_t _pendingPermissionRpcId{ 0 };

        void _closePaneClick(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs&);
        void _sendButtonClicked(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs&);
        void _inputBoxKeyDown(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::KeyRoutedEventArgs& e);

        void _addMessageToChat(const winrt::hstring& role, const winrt::hstring& content);
        void _updateLastMessage(const std::wstring& content);
        safe_void_coroutine _sendToCopilot(std::wstring userMessage);
        void _onCopilotEvent(const CopilotEvent& event);

        void _showConfirmation(const std::wstring& kind, const std::wstring& description);
        void _hideConfirmation();
        void _approveButtonClicked(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs&);
        void _denyButtonClicked(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs&);
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(AIPaneContent);
}
