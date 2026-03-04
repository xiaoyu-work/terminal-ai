// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "AITerminalMiddleware.h"

using namespace winrt::TerminalApp::implementation;
using namespace winrt::Microsoft::Terminal::Settings::Model;

namespace winrt::Microsoft::Terminal::Control::implementation
{
    AITerminalMiddleware::AITerminalMiddleware()
    {
        // Client is lazily created when first needed
    }

    void AITerminalMiddleware::UpdateSettings(
        const winrt::Microsoft::Terminal::Settings::Model::AISettings& settings)
    {
        _enabled = settings.Enabled();
        _copilotCliPath = std::wstring{ settings.CopilotCliPath() };
        _model = std::wstring{ settings.ModelName() };
        _maxContextBlocks = settings.MaxContextBlocks();

        // Build provider config for BYOK
        if (!settings.ApiKey().empty())
        {
            _providerConfig = CopilotClient::MapProviderConfig(
                settings.Provider(),
                std::wstring{ settings.ApiKey() },
                std::wstring{ settings.ApiEndpoint() },
                std::wstring{ settings.ModelName() });
        }
        else
        {
            _providerConfig = std::nullopt;
        }
    }

    void AITerminalMiddleware::SetTerminalWriter(std::function<void(std::wstring_view)> writer)
    {
        _writeToTerminal = std::move(writer);
    }

    void AITerminalMiddleware::SetConnectionWriter(std::function<void(std::wstring_view)> writer)
    {
        _writeToConnection = std::move(writer);
    }

    void AITerminalMiddleware::SetContextProvider(
        std::function<std::wstring()> getBuffer,
        std::function<std::wstring()> getCwd)
    {
        _getTerminalBuffer = std::move(getBuffer);
        _getCwd = std::move(getCwd);
    }

    // ========================================================================
    // Main state machine dispatch
    // ========================================================================

    bool AITerminalMiddleware::HandleInput(wchar_t ch, WORD scanCode,
                                          ::Microsoft::Terminal::Core::ControlKeyStates modifiers)
    {
        if (!_enabled)
        {
            return false;
        }

        switch (_state)
        {
        case AIMiddlewareState::Normal:
            return _handleNormalInput(ch);
        case AIMiddlewareState::Pending:
            return _handlePendingInput(ch);
        case AIMiddlewareState::Capturing:
            return _handleCapturingInput(ch, scanCode, modifiers);
        case AIMiddlewareState::AgentStreaming:
        case AIMiddlewareState::AgentExecuting:
            return _handleStreamingInput(ch);
        case AIMiddlewareState::AgentConfirming:
            return _handleConfirmingInput(ch);
        }
        return false;
    }

    bool AITerminalMiddleware::HandleStringInput(std::wstring_view str)
    {
        if (!_enabled)
        {
            return false;
        }

        // When capturing, append the entire pasted string to the prompt buffer
        if (_state == AIMiddlewareState::Capturing)
        {
            _promptBuffer.append(str);
            _writeText(std::wstring{ str });
            return true;
        }

        // When streaming/executing/confirming, consume and discard pasted text
        if (_state == AIMiddlewareState::AgentStreaming ||
            _state == AIMiddlewareState::AgentExecuting ||
            _state == AIMiddlewareState::AgentConfirming)
        {
            return true;
        }

        // In Normal/Pending, check if the string starts with "@ "
        if (_state == AIMiddlewareState::Normal && str.size() >= 2 &&
            str[0] == L'@' && str[1] == L' ')
        {
            _enterCapturing();
            // Write the "@ " prefix to terminal display
            _writeText(std::wstring{ L"@ " });
            // Append the rest of the pasted text as the prompt
            if (str.size() > 2)
            {
                auto rest = str.substr(2);
                _promptBuffer.append(rest);
                _writeText(std::wstring{ rest });
            }
            return true;
        }

        return false;
    }

    // ========================================================================
    // State handlers
    // ========================================================================

    bool AITerminalMiddleware::_handleNormalInput(wchar_t ch)
    {
        if (ch == L'@')
        {
            _enterPending();
            return true; // consume the '@', wait for next char
        }
        return false; // pass through to shell
    }

    bool AITerminalMiddleware::_handlePendingInput(wchar_t ch)
    {
        if (ch == L' ')
        {
            // "@ " detected - transition to Capturing mode
            _enterCapturing();
            _writeText(std::wstring{ L"@ " });
            return true;
        }

        if (ch == L'@')
        {
            // User typed "@@" - send a literal '@' to the shell
            _state = AIMiddlewareState::Normal;
            if (_writeToConnection)
            {
                _writeToConnection(L"@");
            }
            return true;
        }

        // Not a trigger sequence. Forward the buffered '@' plus this char to the connection.
        _state = AIMiddlewareState::Normal;
        if (_writeToConnection)
        {
            std::wstring buf;
            buf += L'@';
            buf += ch;
            _writeToConnection(buf);
        }
        return true;
    }

    bool AITerminalMiddleware::_handleCapturingInput(wchar_t ch, WORD /*scanCode*/,
                                                     ::Microsoft::Terminal::Core::ControlKeyStates modifiers)
    {
        // Check for Ctrl+C (character 0x03)
        if (ch == L'\x03')
        {
            _writeText(std::wstring{ L"^C" });
            _writeNewline();
            Cancel();
            return true;
        }

        // Escape key
        if (ch == L'\x1b')
        {
            _writeNewline();
            Cancel();
            return true;
        }

        // Enter - submit the prompt
        if (ch == L'\r' || ch == L'\n')
        {
            _writeNewline();
            _startAgent();
            return true;
        }

        // Backspace
        if (ch == L'\b' || ch == L'\x7f')
        {
            if (!_promptBuffer.empty())
            {
                _promptBuffer.pop_back();
                // Move cursor back, overwrite with space, move back again
                _writeText(std::wstring{ L"\b \b" });
            }
            return true;
        }

        // Regular character - append to prompt and echo
        if (ch >= L' ') // printable characters
        {
            _promptBuffer += ch;
            _writeText(std::wstring{ 1, ch });
        }
        return true;
    }

    bool AITerminalMiddleware::_handleStreamingInput(wchar_t ch)
    {
        // During streaming/executing, only Ctrl+C cancels
        if (ch == L'\x03')
        {
            _writeText(std::wstring{ L"^C" });
            _writeNewline();
            Cancel();
            return true;
        }
        // Consume all other input while AI is active
        return true;
    }

    bool AITerminalMiddleware::_handleConfirmingInput(wchar_t ch)
    {
        if (ch == L'y' || ch == L'Y' || ch == L'\r')
        {
            _writeText(std::wstring{ L"y" });
            _writeNewline();
            if (_copilotClient && _pendingPermissionRpcId != 0)
            {
                auto rpcId = _pendingPermissionRpcId;
                _pendingPermissionRpcId = 0;
                _copilotClient->RespondToPermission(rpcId, true);
            }
            return true;
        }

        if (ch == L'n' || ch == L'N' || ch == L'\x03' /* Ctrl+C */)
        {
            _writeText(std::wstring{ L"n" });
            _writeNewline();
            if (_copilotClient && _pendingPermissionRpcId != 0)
            {
                auto rpcId = _pendingPermissionRpcId;
                _pendingPermissionRpcId = 0;
                _copilotClient->RespondToPermission(rpcId, false);
            }
            return true;
        }

        // Consume other keys but do nothing
        return true;
    }

    // ========================================================================
    // State transitions
    // ========================================================================

    void AITerminalMiddleware::_enterNormal()
    {
        _state = AIMiddlewareState::Normal;
        _promptBuffer.clear();
        _pendingPermissionRpcId = 0;
    }

    void AITerminalMiddleware::_enterPending()
    {
        _state = AIMiddlewareState::Pending;
        _promptBuffer.clear();
    }

    void AITerminalMiddleware::_enterCapturing()
    {
        _state = AIMiddlewareState::Capturing;
        _promptBuffer.clear();
    }

    void AITerminalMiddleware::_startAgent()
    {
        if (_promptBuffer.empty())
        {
            // Empty prompt - return to normal
            _enterNormal();
            // Send carriage return to get a new shell prompt
            if (_writeToConnection)
            {
                _writeToConnection(L"\r");
            }
            return;
        }

        _state = AIMiddlewareState::AgentStreaming;
        // Copy the prompt before clearing
        auto prompt = std::move(_promptBuffer);
        _promptBuffer.clear();
        _runAgent(std::move(prompt));
    }

    // ========================================================================
    // Agent execution via Copilot SDK (fire-and-forget)
    // ========================================================================

    void AITerminalMiddleware::_runAgent(std::wstring prompt)
    {
        // Fire-and-forget lambda coroutine
        [](AITerminalMiddleware* self, std::wstring prompt) -> winrt::fire_and_forget
        {
            // Lazily start copilot client
            if (!self->_copilotClient || !self->_copilotClient->IsRunning())
            {
                self->_copilotClient = std::make_shared<CopilotClient>();
                auto cliPath = self->_copilotCliPath.empty() ? L"copilot" : self->_copilotCliPath;
                std::wstring cwd;
                if (self->_getCwd)
                    cwd = self->_getCwd();

                try
                {
                    co_await self->_copilotClient->StartAsync(cliPath, cwd);
                    co_await self->_copilotClient->CreateSessionAsync(
                        self->_model, self->_providerConfig);
                }
                catch (...)
                {
                    self->_writeNewline();
                    self->_writeColored(L"Error: Failed to start copilot CLI. Is it installed and in PATH?", 31);
                    self->_writeNewline();
                    self->_enterNormal();
                    if (self->_writeToConnection)
                        self->_writeToConnection(L"\r");
                    co_return;
                }
            }

            // Enrich prompt with terminal context
            std::wstring context;
            if (self->_getTerminalBuffer)
                context = self->_getTerminalBuffer();

            std::wstring enrichedPrompt = prompt;
            if (!context.empty())
            {
                enrichedPrompt = L"[Terminal context]\n" + context +
                                 L"\n[/Terminal context]\n\n" + prompt;
            }

            try
            {
                co_await self->_copilotClient->SendMessageAsync(enrichedPrompt,
                    [self](const CopilotEvent& event) {
                        switch (event.type)
                        {
                        case CopilotEventType::MessageDelta:
                            self->_writeText(event.content);
                            break;

                        case CopilotEventType::ToolExecutionStart:
                            self->_state = AIMiddlewareState::AgentExecuting;
                            self->_writeNewline();
                            self->_writeColored(L"[Tool: " + event.toolName + L"]", 33); // Yellow
                            self->_writeNewline();
                            break;

                        case CopilotEventType::ToolComplete:
                        {
                            auto status = event.toolSuccess ? L"done" : L"failed";
                            auto color = event.toolSuccess ? 32 : 31; // Green or Red
                            self->_writeColored(L"[" + event.toolName + L" " + status + L"]", color);
                            self->_writeNewline();
                            self->_state = AIMiddlewareState::AgentStreaming;
                            break;
                        }

                        case CopilotEventType::PermissionRequest:
                            self->_state = AIMiddlewareState::AgentConfirming;
                            self->_pendingPermissionRpcId = event.rpcId;
                            self->_writeNewline();
                            self->_writeColored(L"[" + event.permissionKind + L"]", 33); // Yellow
                            self->_writeNewline();
                            self->_writeText(event.permissionDesc);
                            self->_writeNewline();
                            self->_writeColored(L"Approve? [y/N] ", 33);
                            break;

                        case CopilotEventType::SessionError:
                            self->_writeNewline();
                            self->_writeColored(L"Error: " + event.errorMessage, 31); // Red
                            self->_writeNewline();
                            break;

                        case CopilotEventType::SessionIdle:
                            // Handled by SendMessageAsync completion below
                            break;

                        default:
                            break;
                        }
                    });
            }
            catch (...)
            {
                self->_writeNewline();
                self->_writeColored(L"Error: Request failed.", 31);
                self->_writeNewline();
            }

            // Agent loop finished
            self->_writeNewline();
            self->_enterNormal();
            if (self->_writeToConnection)
            {
                self->_writeToConnection(L"\r");
            }
        }(this, std::move(prompt));
    }

    // ========================================================================
    // Cancel
    // ========================================================================

    void AITerminalMiddleware::Cancel()
    {
        if (_copilotClient)
        {
            _copilotClient->AbortCurrentRequest();
        }

        // If we were waiting for permission, deny it
        if (_pendingPermissionRpcId != 0 && _copilotClient)
        {
            _copilotClient->RespondToPermission(_pendingPermissionRpcId, false);
            _pendingPermissionRpcId = 0;
        }

        _enterNormal();
    }

    // ========================================================================
    // Terminal output helpers
    // ========================================================================

    void AITerminalMiddleware::_writeText(const std::wstring& text)
    {
        if (_writeToTerminal)
        {
            _writeToTerminal(text);
        }
    }

    void AITerminalMiddleware::_writeColored(const std::wstring& text, int colorCode)
    {
        if (_writeToTerminal)
        {
            auto seq = L"\x1b[" + std::to_wstring(colorCode) + L"m" + text + L"\x1b[0m";
            _writeToTerminal(seq);
        }
    }

    void AITerminalMiddleware::_writeNewline()
    {
        _writeText(std::wstring{ L"\r\n" });
    }

    void AITerminalMiddleware::_eraseLine()
    {
        // CSI 2K = erase entire line, CSI G = move cursor to column 1
        _writeText(std::wstring{ L"\x1b[2K\x1b[G" });
    }
}
