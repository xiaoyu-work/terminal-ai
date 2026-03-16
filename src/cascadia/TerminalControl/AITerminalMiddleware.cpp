// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "AITerminalMiddleware.h"

using namespace winrt::TerminalApp::implementation;

namespace winrt::Microsoft::Terminal::Control::implementation
{
    AITerminalMiddleware::AITerminalMiddleware()
    {
        // Client is lazily created when first needed
    }

    void AITerminalMiddleware::UpdateSettings(const AIMiddlewareConfig& config)
    {
        _copilotCliPath = config.copilotCliPath;
        _maxContextBlocks = config.maxContextBlocks;
        _githubToken = config.githubToken;
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

        // In Normal/Pending state, check if the string starts with "@ "
        if (_state == AIMiddlewareState::Normal && str.size() >= 2 &&
            str[0] == L'@' && str[1] == L' ')
        {
            _enterCapturing();
            _writeText(std::wstring{ L"@ " });
            if (str.size() > 2)
            {
                auto rest = str.substr(2);
                _promptBuffer.append(rest);
                _writeText(std::wstring{ rest });
            }
            return true;
        }

        // Handle character-by-character input (e.g. from IME/TSF path).
        // When each character arrives as a separate single-char string,
        // we need to run them through the same state machine as HandleInput.
        if (str.size() == 1)
        {
            return HandleInput(str[0], 0, {});
        }

        // In Pending state, check if this string starts with a space
        // (the user typed '@' previously, now we get " rest..." via paste/TSF)
        if (_state == AIMiddlewareState::Pending && !str.empty() && str[0] == L' ')
        {
            _enterCapturing();
            _writeText(std::wstring{ L"@ " });
            if (str.size() > 1)
            {
                auto rest = str.substr(1);
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
                                                     ::Microsoft::Terminal::Core::ControlKeyStates /*modifiers*/)
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
                // Pre-flight check: is copilot CLI available?
                // npm installs produce .cmd shims, so look for .exe and .cmd.
                auto cliPath = self->_copilotCliPath.empty() ? L"copilot" : self->_copilotCliPath;
                {
                    wchar_t resolvedPath[MAX_PATH];
                    const auto foundExe = SearchPathW(nullptr, cliPath.c_str(), L".exe",
                                                      MAX_PATH, resolvedPath, nullptr);
                    const auto foundCmd = !foundExe &&
                                          SearchPathW(nullptr, cliPath.c_str(), L".cmd",
                                                      MAX_PATH, resolvedPath, nullptr);
                    if (!foundExe && !foundCmd && self->_copilotCliPath.empty())
                    {
                        self->_writeNewline();
                        self->_writeColored(L"\u2717 Copilot CLI not found", 31);
                        self->_writeNewline();
                        self->_writeColored(L"  Install it: ", 90);
                        self->_writeText(L"npm install -g @github/copilot");
                        self->_writeNewline();
                        self->_writeColored(L"  Or set the path in Settings \u2192 AI \u2192 Copilot CLI Path", 90);
                        self->_writeNewline();
                        self->_enterNormal();
                        if (self->_writeToConnection)
                            self->_writeToConnection(L"\r");
                        co_return;
                    }
                    else if (!foundExe && !foundCmd)
                    {
                        self->_writeNewline();
                        self->_writeColored(L"\u2717 Copilot CLI not found at: ", 31);
                        self->_writeText(self->_copilotCliPath);
                        self->_writeNewline();
                        self->_writeColored(L"  Check your Settings \u2192 AI \u2192 Copilot CLI Path", 90);
                        self->_writeNewline();
                        self->_enterNormal();
                        if (self->_writeToConnection)
                            self->_writeToConnection(L"\r");
                        co_return;
                    }
                }

                self->_copilotClient = std::make_shared<CopilotClient>();
                std::wstring cwd;
                if (self->_getCwd)
                    cwd = self->_getCwd();

                try
                {
                    co_await self->_copilotClient->StartAsync(cliPath, cwd, self->_githubToken);
                    co_await self->_copilotClient->CreateSessionAsync(L"");
                }
                catch (const winrt::hresult_error& e)
                {
                    self->_copilotClient.reset();
                    self->_writeNewline();
                    self->_writeColored(L"\u2717 Failed to start Copilot", 31);
                    self->_writeNewline();
                    self->_writeColored(L"  ", 90);
                    self->_writeColored(std::wstring{ e.message() }, 90);
                    self->_writeNewline();
                    self->_enterNormal();
                    if (self->_writeToConnection)
                        self->_writeToConnection(L"\r");
                    co_return;
                }
                catch (...)
                {
                    self->_copilotClient.reset();
                    self->_writeNewline();
                    self->_writeColored(L"\u2717 Failed to start Copilot (unknown error)", 31);
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
            catch (const winrt::hresult_error& e)
            {
                self->_writeNewline();
                self->_writeColored(L"\u2717 Request failed: ", 31);
                self->_writeColored(std::wstring{ e.message() }, 90);
                self->_writeNewline();
            }
            catch (...)
            {
                self->_writeNewline();
                self->_writeColored(L"\u2717 Request failed (unknown error)", 31);
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
