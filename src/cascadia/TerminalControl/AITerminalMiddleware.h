// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "../TerminalApp/CopilotClient.h"
#include "../../cascadia/TerminalCore/ControlKeyStates.hpp"

#include <functional>
#include <mutex>

namespace winrt::Microsoft::Terminal::Control::implementation
{
    // State machine for AI middleware (mirrors aterm's middleware)
    enum class AIMiddlewareState
    {
        Normal,           // Pass-through to shell
        Pending,          // Detected '@', waiting for space
        Capturing,        // Capturing user prompt
        AgentStreaming,   // AI is generating response
        AgentConfirming,  // Waiting for tool approval (y/n/Enter)
        AgentExecuting,   // Tool is running
    };

    // Plain C++ struct for AI settings (avoids dependency on WinRT generated headers)
    struct AIMiddlewareConfig
    {
        int32_t maxContextBlocks{ 5 };
        std::wstring copilotCliPath;
        std::wstring githubToken;
    };

    class AITerminalMiddleware
    {
    public:
        AITerminalMiddleware();

        // Configure from settings. Call this when settings change.
        void UpdateSettings(const AIMiddlewareConfig& config);

        // Main interception point. Returns true if the middleware consumed the input.
        // If false, caller should forward input to the connection normally.
        bool HandleInput(wchar_t ch, WORD scanCode,
                         ::Microsoft::Terminal::Core::ControlKeyStates modifiers);

        // Handle string input (e.g., paste). Returns true if consumed.
        bool HandleStringInput(std::wstring_view str);

        // Set the function to write output to the terminal display.
        // This should call _terminal->Write() under a write lock.
        void SetTerminalWriter(std::function<void(std::wstring_view)> writer);

        // Set the function to send input to the real connection.
        void SetConnectionWriter(std::function<void(std::wstring_view)> writer);

        // Set terminal context provider (gets buffer text and CWD).
        void SetContextProvider(
            std::function<std::wstring()> getBuffer,
            std::function<std::wstring()> getCwd);

        // Get current state.
        AIMiddlewareState State() const noexcept { return _state; }

        // Is the middleware active (not in Normal state)?
        bool IsActive() const noexcept { return _state != AIMiddlewareState::Normal; }

        // Cancel current operation.
        void Cancel();

    private:
        AIMiddlewareState _state{ AIMiddlewareState::Normal };

        // Copilot SDK client(replaces _llmClient + _toolRegistry + _agentLoop)
        std::shared_ptr<winrt::TerminalApp::implementation::CopilotClient> _copilotClient;

        std::wstring _copilotCliPath;
        std::wstring _githubToken;
        int _maxContextBlocks{ 5 };

        // Input capture buffer
        std::wstring _promptBuffer;

        // Output writers
        std::function<void(std::wstring_view)> _writeToTerminal;
        std::function<void(std::wstring_view)> _writeToConnection;
        std::function<std::wstring()> _getTerminalBuffer;
        std::function<std::wstring()> _getCwd;

        // Permission state
        int64_t _pendingPermissionRpcId{ 0 };

        // State transitions
        void _enterNormal();
        void _enterPending();
        void _enterCapturing();
        void _startAgent();

        // Terminal output helpers
        void _writeText(const std::wstring& text);
        void _writeColored(const std::wstring& text, int colorCode);
        void _writeNewline();
        void _eraseLine();

        // Handle input in specific states
        bool _handleNormalInput(wchar_t ch);
        bool _handlePendingInput(wchar_t ch);
        bool _handleCapturingInput(wchar_t ch, WORD scanCode,
                                   ::Microsoft::Terminal::Core::ControlKeyStates modifiers);
        bool _handleStreamingInput(wchar_t ch);
        bool _handleConfirmingInput(wchar_t ch);

        // Fire-and-forget coroutine to run the agent via Copilot SDK.
        void _runAgent(std::wstring prompt);
    };
}
