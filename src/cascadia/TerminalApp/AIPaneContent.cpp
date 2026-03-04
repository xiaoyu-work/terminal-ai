// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "AIPaneContent.h"
#include "AIPaneContent.g.cpp"
#include "AIContextCollector.h"
#include "Utils.h"

using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::System;
using namespace winrt::Microsoft::Terminal::Settings;
using namespace winrt::Microsoft::Terminal::Settings::Model;

namespace winrt
{
    namespace WUX = Windows::UI::Xaml;
    using IInspectable = Windows::Foundation::IInspectable;
}

namespace winrt::TerminalApp::implementation
{
    AIPaneContent::AIPaneContent()
    {
        InitializeComponent();
        _dispatcherQueue = winrt::Windows::System::DispatcherQueue::GetForCurrentThread();
    }

    WUX::FrameworkElement AIPaneContent::GetRoot()
    {
        return *this;
    }

    void AIPaneContent::UpdateSettings(const CascadiaSettings& settings)
    {
        _settings = settings;
    }

    Windows::Foundation::Size AIPaneContent::MinimumSize()
    {
        return { 200, 200 };
    }

    void AIPaneContent::Focus(WUX::FocusState reason)
    {
        _inputBox().Focus(reason);
    }

    void AIPaneContent::Close()
    {
        if (_copilotClient)
        {
            _copilotClient->Stop();
        }
        CloseRequested.raise(*this, nullptr);
    }

    INewContentArgs AIPaneContent::GetNewTerminalArgs(BuildStartupKind /*kind*/) const
    {
        return BaseContentArgs(L"ai");
    }

    winrt::hstring AIPaneContent::Icon() const
    {
        static constexpr std::wstring_view glyph{ L"\xE945" }; // Sparkle
        return winrt::hstring{ glyph };
    }

    WUX::Media::Brush AIPaneContent::BackgroundBrush()
    {
        static const auto key = winrt::box_value(L"SettingsUiTabBrush");
        return ThemeLookup(WUX::Application::Current().Resources(),
                           _settings.GlobalSettings().CurrentTheme().RequestedTheme(),
                           key)
            .try_as<WUX::Media::Brush>();
    }

    void AIPaneContent::SetLastActiveControl(const Microsoft::Terminal::Control::TermControl& control)
    {
        _control = control;
    }

    void AIPaneContent::StatusText(const winrt::hstring& value)
    {
        if (_statusText != value)
        {
            _statusText = value;
            PropertyChanged.raise(*this, WUX::Data::PropertyChangedEventArgs{ L"StatusText" });
        }
    }

    void AIPaneContent::IsStreaming(bool value)
    {
        if (_isStreaming != value)
        {
            _isStreaming = value;
            PropertyChanged.raise(*this, WUX::Data::PropertyChangedEventArgs{ L"IsStreaming" });
        }
    }

    void AIPaneContent::_closePaneClick(const IInspectable& /*sender*/,
                                        const WUX::RoutedEventArgs&)
    {
        Close();
    }

    void AIPaneContent::_sendButtonClicked(const IInspectable& /*sender*/,
                                           const WUX::RoutedEventArgs&)
    {
        auto text = _inputBox().Text();
        if (text.empty() || IsStreaming())
            return;

        _addMessageToChat(L"user", text);
        _inputBox().Text(L"");

        _sendToCopilot(std::wstring(text));
    }

    void AIPaneContent::_inputBoxKeyDown(const IInspectable& /*sender*/,
                                         const WUX::Input::KeyRoutedEventArgs& e)
    {
        if (e.Key() == VirtualKey::Enter)
        {
            // Check for Shift+Enter (should insert newline, not send)
            const auto state = winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread().GetKeyState(VirtualKey::Shift);
            if (WI_IsFlagSet(state, winrt::Windows::UI::Core::CoreVirtualKeyStates::Down))
            {
                return; // Let the TextBox handle Shift+Enter as a newline
            }

            e.Handled(true);
            _sendButtonClicked(nullptr, nullptr);
        }
    }

    void AIPaneContent::_addMessageToChat(const winrt::hstring& role, const winrt::hstring& content)
    {
        // Create a simple text display for the message
        WUX::Controls::TextBlock messageBlock;
        messageBlock.TextWrapping(WUX::TextWrapping::Wrap);
        messageBlock.Margin({ 8, 4, 8, 4 });
        messageBlock.IsTextSelectionEnabled(true);

        if (role == L"user")
        {
            messageBlock.Text(L"> " + content);
            messageBlock.Opacity(0.9);
        }
        else if (role == L"assistant")
        {
            messageBlock.Text(content);
        }
        else
        {
            messageBlock.Text(L"[" + role + L"] " + content);
            messageBlock.Opacity(0.7);
        }

        _chatPanel().Children().Append(messageBlock);

        // Auto-scroll to bottom
        _chatScrollViewer().ChangeView(nullptr, _chatScrollViewer().ScrollableHeight(), nullptr);
    }

    void AIPaneContent::_updateLastMessage(const std::wstring& content)
    {
        auto children = _chatPanel().Children();
        if (children.Size() > 0)
        {
            auto lastElement = children.GetAt(children.Size() - 1);
            if (auto textBlock = lastElement.try_as<WUX::Controls::TextBlock>())
            {
                textBlock.Text(winrt::hstring{ content });
                _chatScrollViewer().ChangeView(nullptr, _chatScrollViewer().ScrollableHeight(), nullptr);
            }
        }
    }

    safe_void_coroutine AIPaneContent::_sendToCopilot(std::wstring userMessage)
    {
        if (!_settings)
            co_return;

        auto aiSettings = _settings.GlobalSettings().AISettings();
        if (!aiSettings)
            co_return;

        // Lazily start copilot client if not running
        if (!_copilotClient || !_copilotClient->IsRunning())
        {
            _copilotClient = std::make_shared<CopilotClient>();

            auto cliPath = std::wstring(aiSettings.CopilotCliPath());
            if (cliPath.empty())
            {
                cliPath = L"copilot"; // rely on PATH
            }

            std::wstring cwd;
            if (auto control = _control.get())
            {
                cwd = AIContextCollector::GetWorkingDirectory(control);
            }

            try
            {
                co_await _copilotClient->StartAsync(cliPath, cwd);
            }
            catch (...)
            {
                _addMessageToChat(L"error",
                    L"Failed to start copilot CLI. Is it installed and in PATH?");
                IsStreaming(false);
                StatusText(L"Error");
                co_return;
            }

            // Create session with BYOK provider config
            std::optional<CopilotProviderConfig> providerConfig;
            if (!aiSettings.ApiKey().empty())
            {
                providerConfig = CopilotClient::MapProviderConfig(
                    aiSettings.Provider(),
                    std::wstring(aiSettings.ApiKey()),
                    std::wstring(aiSettings.ApiEndpoint()),
                    std::wstring(aiSettings.ModelName()));
            }

            try
            {
                co_await _copilotClient->CreateSessionAsync(
                    std::wstring(aiSettings.ModelName()),
                    providerConfig);
            }
            catch (...)
            {
                _addMessageToChat(L"error",
                    L"Failed to create copilot session.");
                IsStreaming(false);
                StatusText(L"Error");
                co_return;
            }
        }

        IsStreaming(true);
        StatusText(L"Thinking...");
        _addMessageToChat(L"assistant", L"");
        _currentAssistantContent.clear();

        // Optionally inject terminal context as part of message
        std::wstring enrichedMessage = userMessage;
        if (auto control = _control.get())
        {
            auto termContext = AIContextCollector::CollectFromTermControl(control);
            if (!termContext.empty())
            {
                enrichedMessage = L"[Terminal context]\n" + termContext +
                                  L"\n[/Terminal context]\n\n" + userMessage;
            }
        }

        // Send and handle streaming events
        try
        {
            co_await _copilotClient->SendMessageAsync(enrichedMessage,
                [this](const CopilotEvent& event) {
                    _dispatcherQueue.TryEnqueue([this, event]() {
                        _onCopilotEvent(event);
                    });
                });
        }
        catch (...)
        {
            _dispatcherQueue.TryEnqueue([this]() {
                _addMessageToChat(L"error", L"Request failed.");
                IsStreaming(false);
                StatusText(L"Error");
            });
        }

        _dispatcherQueue.TryEnqueue([this]() {
            IsStreaming(false);
            StatusText(L"Ready");
            _hideConfirmation();
        });
    }

    void AIPaneContent::_onCopilotEvent(const CopilotEvent& event)
    {
        switch (event.type)
        {
        case CopilotEventType::MessageDelta:
            _currentAssistantContent += event.content;
            _updateLastMessage(_currentAssistantContent);
            break;

        case CopilotEventType::Message:
            if (!event.content.empty())
            {
                _currentAssistantContent = event.content;
                _updateLastMessage(_currentAssistantContent);
            }
            break;

        case CopilotEventType::ReasoningDelta:
            // Could display thinking in a separate collapsible section
            break;

        case CopilotEventType::ToolExecutionStart:
            StatusText(winrt::hstring{ L"Running: " + event.toolName });
            _addMessageToChat(L"tool", winrt::hstring{ L"[Tool: " + event.toolName + L"]" });
            // Reset assistant content for next chunk after tool
            _currentAssistantContent.clear();
            _addMessageToChat(L"assistant", L"");
            break;

        case CopilotEventType::ToolPartialResult:
            // Could show partial output
            break;

        case CopilotEventType::ToolComplete:
        {
            auto status = event.toolSuccess ? L"done" : L"failed";
            _addMessageToChat(L"tool",
                winrt::hstring{ L"[" + event.toolName + L" " + status + L"]" });
            break;
        }

        case CopilotEventType::PermissionRequest:
            StatusText(L"Awaiting approval...");
            _pendingPermissionRpcId = event.rpcId;
            _showConfirmation(event.permissionKind, event.permissionDesc);
            break;

        case CopilotEventType::UserInputRequest:
            // For now, auto-respond with empty (could show input dialog)
            if (_copilotClient)
            {
                _copilotClient->RespondToUserInput(event.rpcId, L"");
            }
            break;

        case CopilotEventType::Usage:
        {
            auto total = event.inputTokens + event.outputTokens;
            StatusText(winrt::hstring{ L"Tokens: " + std::to_wstring(total) });
            break;
        }

        case CopilotEventType::SessionError:
            _addMessageToChat(L"error", winrt::hstring{ event.errorMessage });
            IsStreaming(false);
            StatusText(L"Error");
            break;

        case CopilotEventType::SessionIdle:
            // Handled by SendMessageAsync completion
            break;

        case CopilotEventType::CompactionStart:
            StatusText(L"Compacting context...");
            break;

        case CopilotEventType::CompactionComplete:
            break;
        }
    }

    void AIPaneContent::_showConfirmation(const std::wstring& kind, const std::wstring& description)
    {
        auto displayText = L"[" + kind + L"] " + description;
        _confirmationText().Text(winrt::hstring{ displayText });
        _confirmationBar().Visibility(WUX::Visibility::Visible);
    }

    void AIPaneContent::_hideConfirmation()
    {
        _confirmationBar().Visibility(WUX::Visibility::Collapsed);
        _pendingPermissionRpcId = 0;
    }

    void AIPaneContent::_approveButtonClicked(const IInspectable& /*sender*/, const WUX::RoutedEventArgs&)
    {
        if (_pendingPermissionRpcId != 0 && _copilotClient)
        {
            auto rpcId = _pendingPermissionRpcId;
            _pendingPermissionRpcId = 0;
            _hideConfirmation();
            _copilotClient->RespondToPermission(rpcId, true);
        }
    }

    void AIPaneContent::_denyButtonClicked(const IInspectable& /*sender*/, const WUX::RoutedEventArgs&)
    {
        if (_pendingPermissionRpcId != 0 && _copilotClient)
        {
            auto rpcId = _pendingPermissionRpcId;
            _pendingPermissionRpcId = 0;
            _hideConfirmation();
            _copilotClient->RespondToPermission(rpcId, false);
        }
    }
}
